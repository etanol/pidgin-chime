/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <string.h>

#include <glib/gi18n.h>
#include <glib/glist.h>

#include <prpl.h>
#include <blist.h>
#include <roomlist.h>

#include "chime.h"

#include <libsoup/soup.h>

struct chime_chat {
	struct chime_room *room;
	PurpleConversation *conv;
	/* For cancellation */
	SoupMessage *msgs_msg, *members_msg;
	gboolean got_members, got_msgs;
	GHashTable *messages; /* While fetching */
	GHashTable *members;
};

static void chat_deliver_msg(struct chime_chat *chat, JsonNode *node, int msg_time)
{
	const gchar *str;
	if (!msg_time) {
		GTimeVal tv;
		if (!parse_string(node, "CreatedOn", &str) ||
		    !g_time_val_from_iso8601(str, &tv))
			msg_time = time(NULL);
		else
			msg_time = tv.tv_sec;
	}
	if (parse_string(node, "Content", &str)) {
		PurpleConnection *conn = chat->conv->account->gc;
		struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
		int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(chat->conv));
		serv_got_chat_in(conn, id, "someone", PURPLE_MESSAGE_RECV, str, msg_time);
	}

}
static void chat_msg_cb(gpointer _chat, JsonNode *node)
{
	struct chime_chat *chat = _chat;
	const gchar *str;

	JsonObject *obj = json_node_get_object(node);
	JsonNode *record = json_object_get_member(obj, "record");
	if (!record)
		return;
	/* Still gathering messages. Add to the table, to avoid dupes */
	if (chat->messages) {
		const gchar *id;

		if (parse_string(record, "MessageId", &id))
			g_hash_table_insert(chat->messages, (gchar *)id, json_node_ref(node));
		return;
	}
	chat_deliver_msg(chat, record, time(NULL));
}

void chime_destroy_chat(struct chime_chat *chat)
{
	PurpleConnection *conn = chat->conv->account->gc;
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	struct chime_room *room = chat->room;
	int id = purple_conv_chat_get_id(PURPLE_CONV_CHAT(room->chat->conv));

	if (chat->msgs_msg) {
		soup_session_cancel_message(cxn->soup_sess, chat->msgs_msg, 1);
		chat->msgs_msg = NULL;
	}
	if (chat->members_msg) {
		soup_session_cancel_message(cxn->soup_sess, chat->members_msg, 1);
		chat->members_msg = NULL;
	}
	chime_jugg_unsubscribe(cxn, room->channel, chat_msg_cb, room);

	serv_got_chat_left(conn, id);
	g_hash_table_remove(cxn->live_chats, GUINT_TO_POINTER(room->id));

	if (chat->messages)
		g_hash_table_destroy(chat->messages);
	if (chat->members)
		g_hash_table_destroy(chat->members);

	g_free(chat);
	room->chat = NULL;
	printf("Destroyed chat %p\n", chat);
}


struct msg_sort {
	GTimeVal tm;
	JsonNode *node;
};

gint compare_ms(gconstpointer _a, gconstpointer _b)
{
	const struct msg_sort *a = _a;
	const struct msg_sort *b = _b;

	if (a->tm.tv_sec > b->tm.tv_sec)
		return 1;
	if (a->tm.tv_sec == b->tm.tv_sec &&
	    a->tm.tv_usec > b->tm.tv_usec)
		return 1;
	return 0;
}

int insert_queued_msg(gpointer _id, gpointer _node, gpointer _list)
{
	const gchar *str;
	GList **l = _list;

	if (parse_string(_node, "CreatedOn", &str)) {
		struct msg_sort *ms = g_new0(struct msg_sort, 1);
		if (!g_time_val_from_iso8601(str, &ms->tm)) {
			g_free(ms);
			return TRUE;
		}
		ms->node = json_node_ref(_node);
		printf("Add %p to list at %p\n", ms, l);
		*l = g_list_insert_sorted(*l, ms, compare_ms);
	}
	return TRUE;
}

void chime_complete_chat_setup(struct chime_connection *cxn, struct chime_chat *chat)
{
	GList *l = NULL;
	printf("List at %p\n", &l);
	/* Sort messages by time */
	g_hash_table_foreach_remove(chat->messages, insert_queued_msg, &l);
	g_hash_table_destroy(chat->messages);
	chat->messages = NULL;

	while (l) {
		struct msg_sort *ms = l->data;
		JsonNode *node = ms->node;
		chat_deliver_msg(chat, node, ms->tm.tv_sec);
		g_free(ms);
		l = g_list_remove(l, ms);

		/* Last message, note down the received time */
		if (!l) {
			const gchar *tm;
			if (parse_string(node, "CreatedOn", &tm)){
				gchar *last_msgs_key = g_strdup_printf("last-room-%s", chat->room->id);
				purple_account_set_string(cxn->prpl_conn->account,
							  last_msgs_key, tm);
				g_free(last_msgs_key);
			}
		}
		json_node_unref(node);
	}
}

static void one_msg_cb(JsonArray *array, guint index_,
		       JsonNode *node, gpointer _hash)
{
	struct chime_chat *chat;
	const char *id;

	if (parse_string(node, "MessageId", &id))
		g_hash_table_insert(_hash, (gpointer)id, json_node_ref(node));
}


void fetch_chat_messages(struct chime_connection *cxn, struct chime_chat *chat, const gchar *next_token);
static void fetch_msgs_cb(struct chime_connection *cxn, SoupMessage *msg, JsonNode *node, gpointer _chat)
{
	struct chime_chat *chat = _chat;
	const gchar *next_token;

	JsonObject *obj = json_node_get_object(node);
	JsonNode *msgs_node = json_object_get_member(obj, "Messages");
	JsonArray *msgs_array = json_node_get_array(msgs_node);

	chat->msgs_msg = NULL;
	json_array_foreach_element(msgs_array, one_msg_cb, chat->messages);

	if (parse_string(node, "NextToken", &next_token))
		fetch_chat_messages(cxn, _chat, next_token);
	else {
		//if (chat->members_done)
			chime_complete_chat_setup(cxn, chat);
	}
}

void fetch_chat_messages(struct chime_connection *cxn, struct chime_chat *chat, const gchar *next_token)
{
	struct chime_room *room = chat->room;
	SoupURI *uri = soup_uri_new_printf(cxn->messaging_url, "/rooms/%s/messages", room->id);
	const gchar *opts[4];
	int i = 0;
	gchar *last_msgs_key = g_strdup_printf("last-room-%s", room->id);
	const gchar *after = purple_account_get_string(cxn->prpl_conn->account, last_msgs_key, NULL);
	g_free(last_msgs_key);
	if (after && after[0]) {
		opts[i++] = "after";
		opts[i++] = after;
	}
	if (next_token) {
		opts[i++] = "next-token";
		opts[i++] = next_token;
	}
	while (i < 4)
		opts[i++] = NULL;

	soup_uri_set_query_from_fields(uri, "max-results", "50", opts[0], opts[1], opts[2], opts[3], NULL);
	chat->msgs_msg = chime_queue_http_request(cxn, NULL, uri, fetch_msgs_cb, chat, TRUE);
}

void chime_purple_join_chat(PurpleConnection *conn, GHashTable *data)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	struct chime_room *room;
	struct chime_chat *chat;
	const gchar *roomid = g_hash_table_lookup(data, "RoomId");

	printf("join_chat %p %s %s\n", data, roomid, (gchar *)g_hash_table_lookup(data, "Name"));
	room = g_hash_table_lookup(cxn->rooms_by_id, roomid);
	if (!room || room->chat)
		return;

	chat = g_new0(struct chime_chat, 1);
	room->chat = chat;
	chat->room = room;

	int chat_id = ++cxn->chat_id;
	chat->conv = serv_got_joined_chat(conn, chat_id, g_hash_table_lookup(data, "Name"));
	g_hash_table_insert(cxn->live_chats, GUINT_TO_POINTER(chat_id), chat);

	chat->messages = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)json_node_unref);
	chime_jugg_subscribe(cxn, room->channel, chat_msg_cb, chat);

	fetch_chat_messages(cxn, chat, NULL);
}

void chime_purple_chat_leave(PurpleConnection *conn, int id)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);
	struct chime_chat *chat = g_hash_table_lookup(cxn->live_chats, GUINT_TO_POINTER(id));

	chime_destroy_chat(chat);
}
