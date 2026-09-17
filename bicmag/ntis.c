#include "bicmag/ntis.h"
#include <gio/gio.h>
#include <libsoup/soup-form.h>
#include <string.h>

BicMagNtis *bicmag_ntis_new(void)
{
    BicMagNtis *client = g_new0(BicMagNtis, 1);
    client->session = soup_session_new();
    g_autoptr(SoupCookieJar) cookie_jar = soup_cookie_jar_new();
    soup_session_add_feature(client->session,
                             SOUP_SESSION_FEATURE(cookie_jar));
    g_autofree gchar *agent = g_strdup_printf("BicMag/0.1 (%s; %s)",
#ifdef G_OS_WIN32
        "Windows",
#elif defined(__APPLE__)
        "macOS",
#elif defined(__linux__)
        "Linux",
#else
        "Unknown",
#endif
        G_STRINGIFY(GLIB_MAJOR_VERSION) "." G_STRINGIFY(GLIB_MINOR_VERSION));
    g_object_set(client->session, "user-agent", agent, "timeout", 60u, NULL);
    return client;
}

void bicmag_ntis_free(BicMagNtis *client)
{
    if (client == NULL) return;
    g_clear_object(&client->session);
    g_free(client);
}

static gchar *
bicmag_ntis_send_message(BicMagNtis *client, SoupMessage *message,
                          GError **error)
{
    g_autoptr(GBytes) body = soup_session_send_and_read(client->session, message, NULL, error);
    if (body == NULL) return NULL;
    if (soup_message_get_status(message) < 200 || soup_message_get_status(message) >= 300) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "NTIS HTTP status: %u",
                    soup_message_get_status(message));
        return NULL;
    }
    gsize size = 0;
    const gchar *data = g_bytes_get_data(body, &size);
    return g_strndup(data, size);
}

gchar *
bicmag_ntis_get_html(BicMagNtis *client, const gchar *uri, GError **error)
{
    g_return_val_if_fail(client != NULL && client->session != NULL, NULL);
    g_return_val_if_fail(uri != NULL, NULL);
    g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, uri);
    if (message == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid NTIS URI");
        return NULL;
    }
    return bicmag_ntis_send_message(client, message, error);
}

gchar *
bicmag_ntis_post_form(BicMagNtis *client, const gchar *uri,
                      GHashTable *form, GError **error)
{
    g_return_val_if_fail(client != NULL && client->session != NULL, NULL);
    g_return_val_if_fail(uri != NULL && form != NULL, NULL);
    g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_POST, uri);
    if (message == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid NTIS URI");
        return NULL;
    }
    g_autofree gchar *encoded = soup_form_encode_hash(form);
    g_autoptr(GBytes) body = g_bytes_new(encoded, strlen(encoded));
    soup_message_set_request_body_from_bytes(message,
        "application/x-www-form-urlencoded", body);
    return bicmag_ntis_send_message(client, message, error);
}
