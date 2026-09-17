#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

static void
bicmag_mcp_handler(SoupServer*server, SoupServerMessage*message,
                   const gchar*path, GHashTable*query, gpointer user_data)
{
    (void)server;
    (void)query;
    (void)user_data;
    if (g_strcmp0(path, "/mcp") != 0) {
        soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
        return;
    }
    if (g_strcmp0(soup_server_message_get_method(message), SOUP_METHOD_POST) != 0) {
        soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
        return;
    }

    SoupMessageBody*request = soup_server_message_get_request_body(message);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    if (request == NULL || !json_parser_load_from_data(
            parser, request->data, request->length, &error)) {
        soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, NULL);
        return;
    }
    JsonNode*request_root = json_parser_get_root(parser);
    if (request_root == NULL || !JSON_NODE_HOLDS_OBJECT(request_root)) {
        soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, NULL);
        return;
    }
    JsonObject*object = json_node_get_object(request_root);
    const gchar*method = json_object_get_string_member_with_default(
        object, "method", "");
    JsonNode*id = json_object_get_member(object, "id");
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
        json_builder_end_array(builder);
        json_builder_end_object(builder);
    }
    else {
        json_builder_end_object(builder);
        soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
        return;
    }
    json_builder_end_object(builder);
    g_autoptr(JsonGenerator) generator = json_generator_new();
    JsonNode*root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gsize length = 0;
    g_autofree gchar*body = json_generator_to_data(generator, &length);
    json_node_free(root);
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY,
                                     body, length);
}

int
main(int argc, char**argv)
{
    guint port = argc > 1 ? (guint)g_ascii_strtoull(argv[1], NULL, 10) : 0;

    g_autoptr(GError) error = NULL;
    g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
    soup_server_add_handler(server, NULL, bicmag_mcp_handler, NULL, NULL);
    if (!soup_server_listen_local(server, port, SOUP_SERVER_LISTEN_IPV4_ONLY,
                                  &error)) {
        g_printerr("bicmag: %s\n", error->message);
        return 1;
    }
    GSList*uris = soup_server_get_uris(server);
    g_autofree gchar*endpoint = uris != NULL ? g_uri_to_string(uris->data) : NULL;
    if (endpoint != NULL && g_str_has_suffix(endpoint, "/")){
        endpoint[strlen(endpoint) - 1] = '\0';
    }
    g_print("bicmag MCP listening on %s/mcp\n", endpoint != NULL ? endpoint : "http://127.0.0.1:0");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
