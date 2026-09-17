#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include "bicmag/cache.h"

static gchar *
bicmag_mcp_deadline_label(const gchar *deadline_date, GDateTime *today)
{
    g_autofree gchar *iso_date = NULL;
    g_autofree gchar *iso_datetime = NULL;

    g_autoptr(GDateTime) deadline = NULL;
    gint64 days;

    if (deadline_date == NULL || *deadline_date == '\0') {
        return g_strdup("마감일 없음");
    }
    iso_date = g_strdup(deadline_date);
    for (gchar *cursor = iso_date; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            *cursor = '-';
        }
    }
    iso_datetime = g_strdup_printf("%sT00:00:00+09:00", iso_date);
    deadline = g_date_time_new_from_iso8601(iso_datetime, NULL);
    if (deadline == NULL) {
        return g_strdup("D-?");
    }
    days = g_date_time_difference(deadline, today) / G_TIME_SPAN_DAY;
    return days >= 0 ? g_strdup_printf("D-%" G_GINT64_FORMAT, days) :
           g_strdup_printf("D+%" G_GINT64_FORMAT, -days);
}

static void
bicmag_mcp_error(SoupServerMessage *message, JsonNode *id, gint code,
                 const gchar *text)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder); json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0"); json_builder_set_member_name(builder, "id");
    json_builder_add_value(builder,
                           id != NULL ? json_node_copy(id) : json_node_new(JSON_NODE_NULL));
    json_builder_set_member_name(builder, "error"); json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "code"); json_builder_add_int_value(builder, code);
    json_builder_set_member_name(builder, "message"); json_builder_add_string_value(builder, text);
    json_builder_end_object(builder); json_builder_end_object(builder);
    g_autoptr(JsonGenerator) generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder); json_generator_set_root(generator, root);
    gsize length = 0; g_autofree gchar *body = json_generator_to_data(generator, &length);
    json_node_free(root); soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, body, length);
}

static void
bicmag_mcp_handler(SoupServer *server, SoupServerMessage *message,
                   const gchar *path, GHashTable *query, gpointer user_data)
{
    (void)server;
    (void)query;
    BicMagCache *cache = user_data;
    if (g_strcmp0(path, "/mcp") != 0) {
        soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
        return;
    }
    if (g_strcmp0(soup_server_message_get_method(message), SOUP_METHOD_POST) != 0) {
        soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
        return;
    }

    SoupMessageBody *request = soup_server_message_get_request_body(message);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    if (request == NULL || !json_parser_load_from_data(
            parser, request->data, request->length, &error)) {
        soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, NULL);
        return;
    }
    JsonNode *request_root = json_parser_get_root(parser);
    if (request_root == NULL || !JSON_NODE_HOLDS_OBJECT(request_root)) {
        soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, NULL);
        return;
    }
    JsonObject *object = json_node_get_object(request_root);
    const gchar *method = json_object_get_string_member_with_default(
        object, "method", "");
    JsonNode *id = json_object_get_member(object, "id");
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    json_builder_set_member_name(builder, "id");
    json_builder_add_value(builder, id != NULL ? json_node_copy(id) :
                           json_node_new(JSON_NODE_NULL));
    json_builder_set_member_name(builder, "result");
    if (g_strcmp0(method, "initialize") == 0) {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "protocolVersion");
        json_builder_add_string_value(builder, "2025-11-25");
        json_builder_set_member_name(builder, "capabilities");
        json_builder_begin_object(builder);
        json_builder_end_object(builder);
        json_builder_set_member_name(builder, "serverInfo");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, "BicMag");
        json_builder_set_member_name(builder, "version");
        json_builder_add_string_value(builder, "0.1.0");
        json_builder_end_object(builder);
        json_builder_end_object(builder);
    }
    else if (g_strcmp0(method, "tools/list") == 0) {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "tools");
        json_builder_begin_array(builder);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, "ntis_search");
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder, "Search locally cached NTIS notices");
        json_builder_set_member_name(builder, "inputSchema"); json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "object");
        json_builder_set_member_name(builder, "properties"); json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "query"); json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "string"); json_builder_end_object(builder);
        json_builder_end_object(builder); json_builder_set_member_name(builder, "required");
        json_builder_begin_array(builder); json_builder_add_string_value(builder, "query");
        json_builder_end_array(builder); json_builder_end_object(builder);
        json_builder_end_object(builder);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, "ntis_list");
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder, "List locally cached NTIS notices");
        json_builder_set_member_name(builder, "inputSchema");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "object");
        json_builder_end_object(builder);
        json_builder_end_object(builder);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, "ntis_attachments");
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder, "List locally cached NTIS attachments");
        json_builder_set_member_name(builder, "inputSchema");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "object");
        json_builder_set_member_name(builder, "properties");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "notice_id");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "string");
        json_builder_end_object(builder);
        json_builder_end_object(builder);
        json_builder_set_member_name(builder, "required");
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "notice_id");
        json_builder_end_array(builder);
        json_builder_end_object(builder);
        json_builder_end_object(builder);
        json_builder_end_array(builder);
        json_builder_end_object(builder);
    }
    else if (g_strcmp0(method, "tools/call") == 0) {
        JsonObject *params = json_object_get_object_member(object, "params");
        const gchar *name = params ? json_object_get_string_member_with_default(params, "name",
                                                                                "") : "";
        JsonObject *arguments = params ? json_object_get_object_member(params, "arguments") : NULL;
        const gchar *query_text = arguments ? json_object_get_string_member_with_default(arguments,
                                                                                         "query",
                                                                                         "") : "";
        const gchar *notice_id = arguments ?
                                 json_object_get_string_member_with_default(arguments,
                                                                            "notice_id", "") :
                                 "";
        g_autoptr(GError) search_error = NULL;
        g_autoptr(GPtrArray) notices = NULL;
        g_autoptr(GPtrArray) attachments = NULL;
        if (g_strcmp0(name, "ntis_attachments") == 0 && cache != NULL &&
            *notice_id != '\0') {
            attachments = bicmag_cache_list_attachments(cache, notice_id, &search_error);
        }
        else if (g_strcmp0(name, "ntis_list") == 0) {
            notices = bicmag_cache_list_notices(cache, &search_error);
        }
        else if (g_strcmp0(name, "ntis_search") == 0 && cache != NULL &&
                 *query_text != '\0') {
            notices = bicmag_cache_search_notices(cache, query_text, &search_error);
        }
        else {
            json_builder_end_object(builder);
            soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, NULL);
            bicmag_mcp_error(message, id, -32602, "Invalid tools/call arguments");
            return;
        }
        if ((g_strcmp0(name, "ntis_attachments") == 0 ? attachments == NULL : notices == NULL)) {
            soup_server_message_set_status(message,
                                           SOUP_STATUS_INTERNAL_SERVER_ERROR,
                                           NULL);
            bicmag_mcp_error(message, id, -32603,
                             search_error ? search_error->message :
                             "Local search failed"); return;
        }
        json_builder_begin_object(builder); json_builder_set_member_name(builder, "content");
        json_builder_begin_array(builder); json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "text");
        json_builder_set_member_name(builder, "text");
        g_autoptr(GString) text = g_string_new(NULL);
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        g_autoptr(GDateTime) today = g_date_time_new_local(g_date_time_get_year(now),
                                                           g_date_time_get_month(now),
                                                           g_date_time_get_day_of_month(now),
                                                           0, 0, 0);
        if (g_strcmp0(name, "ntis_attachments") == 0) {
            for (guint i = 0; i < attachments->len; i++) {
                BicMagAttachment *attachment = g_ptr_array_index(attachments, i);
                g_string_append_printf(text, "%s\t%s\t%s\t%s\n", attachment->id,
                                       attachment->name, attachment->local_path != NULL ?
                                       attachment->local_path : "미다운로드",
                                       attachment->download_url);
            }
        }
        else {
            for (guint i = 0; i < notices->len;
                 i++) { BicMagNotice *notice = g_ptr_array_index(notices, i);
                        g_autofree gchar *dday =
                            bicmag_mcp_deadline_label(notice->deadline_date, today);
                        g_string_append_printf(text, "%s\t%s\t%s\t%s\n", notice->id,
                                               notice->deadline_date != NULL &&
                                               *notice->deadline_date != '\0' ?
                                               notice->deadline_date : "마감일 없음", dday,
                                               notice->title);}
        }
        json_builder_add_string_value(builder, text->str); json_builder_end_object(builder);
        json_builder_end_array(builder); json_builder_end_object(builder);
    }
    else {
        json_builder_end_object(builder);
        bicmag_mcp_error(message, id, -32601, "Method not found");
        return;
    }
    json_builder_end_object(builder);
    g_autoptr(JsonGenerator) generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gsize length = 0;
    g_autofree gchar *body = json_generator_to_data(generator, &length);
    json_node_free(root);
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY,
                                     body, length);
}

int
main(int argc, char **argv)
{
    guint port = argc > 1 ? (guint)g_ascii_strtoull(argv[1], NULL, 10) : 0;

    g_autoptr(GError) error = NULL;
    g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
    g_autoptr(BicMagCache) cache = bicmag_cache_open("bicmag.db", &error);
    if (cache == NULL) { g_printerr("bicmag: %s\n", error->message); return 1; }
    soup_server_add_handler(server, NULL, bicmag_mcp_handler, cache, NULL);
    if (!soup_server_listen_local(server, port, SOUP_SERVER_LISTEN_IPV4_ONLY,
                                  &error)) {
        g_printerr("bicmag: %s\n", error->message);
        return 1;
    }
    GSList *uris = soup_server_get_uris(server);
    g_autofree gchar *endpoint = uris != NULL ? g_uri_to_string(uris->data) : NULL;
    if (endpoint != NULL && g_str_has_suffix(endpoint, "/")){
        endpoint[strlen(endpoint) - 1] = '\0';
    }
    g_print("bicmag MCP listening on %s/mcp\n", endpoint != NULL ? endpoint : "http://127.0.0.1:0");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
