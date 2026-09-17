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
                                              G_STRINGIFY(GLIB_MAJOR_VERSION) "." G_STRINGIFY(
                                                  GLIB_MINOR_VERSION));
    g_object_set(client->session, "user-agent", agent, "timeout", 60u, NULL);
    return client;
}

void bicmag_ntis_free(BicMagNtis *client)
{
    if (client == NULL){return;}
    g_clear_object(&client->session);
    g_free(client);
}

static gchar *
bicmag_ntis_send_message(BicMagNtis *client, SoupMessage *message,
                         GError **error)
{
    g_autoptr(GBytes) body = soup_session_send_and_read(client->session, message, NULL, error);
    if (body == NULL){return NULL;}
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

static gboolean
bicmag_ntis_download_message(BicMagNtis *client, SoupMessage *message,
                             const gchar *directory, const gchar *filename,
                             gchar **saved_path, guint64 *bytes_written,
                             gchar **sha256, GError **error)
{
    if (bytes_written != NULL){*bytes_written = 0;}
    if (sha256 != NULL){*sha256 = NULL;}
    g_return_val_if_fail(client != NULL && client->session != NULL, FALSE);
    g_return_val_if_fail(directory != NULL && filename != NULL, FALSE);
    if (saved_path != NULL){*saved_path = NULL;}
    if (*filename == '\0' || g_path_is_absolute(filename) ||
        g_strcmp0(filename, ".") == 0 || g_strcmp0(filename, "..") == 0 ||
        g_strstr_len(filename, -1, "/") != NULL ||
        g_strstr_len(filename, -1, "\\") != NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Unsafe attachment filename");
        return FALSE;
    }
    if (g_mkdir_with_parents(directory, 0755) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Cannot create download directory: %s", directory);
        return FALSE;
    }
    g_autoptr(GError) local_error = NULL;
    g_autoptr(GInputStream) input = soup_session_send(client->session, message, NULL, &local_error);
    if (input == NULL) { g_propagate_error(error, g_steal_pointer(&local_error)); return FALSE; }
    guint status = soup_message_get_status(message);
    if (status < 200 || status >= 300) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "NTIS download HTTP status: %u", status);
        return FALSE;
    }
    g_autofree gchar *final_name = g_build_filename(directory, filename, NULL);
    g_autofree gchar *temp_name = g_strdup_printf("%s.part", final_name);
    g_autoptr(GFile) temp = g_file_new_for_path(temp_name);
    g_autoptr(GFile) final = g_file_new_for_path(final_name);
    g_autoptr(GFileOutputStream) output = g_file_replace(temp, NULL, FALSE, G_FILE_CREATE_NONE,
                                                         NULL, error);
    if (output == NULL){return FALSE;}
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    guint64 total = 0; guint8 buffer[8192]; gssize n;
    while ((n = g_input_stream_read(G_INPUT_STREAM(input), buffer, sizeof buffer, NULL,
                                    error)) > 0) {
        if (!g_output_stream_write_all(G_OUTPUT_STREAM(output), buffer, n, NULL, NULL,
                                       error)) { g_file_delete(temp, NULL, NULL); return FALSE; }
        g_checksum_update(checksum, buffer, n); total += (guint64)n;
    }
    if (n < 0) { g_file_delete(temp, NULL, NULL); return FALSE; }
    if (!g_output_stream_close(G_OUTPUT_STREAM(output), NULL, error)) {
        g_file_delete(temp, NULL, NULL); return FALSE;
    }
    if (!g_file_move(temp, final, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error)) {
        g_file_delete(temp, NULL, NULL); return FALSE;
    }
    if (saved_path != NULL){*saved_path = g_steal_pointer(&final_name);}
    if (bytes_written != NULL){*bytes_written = total;}
    if (sha256 != NULL){*sha256 = g_strdup(g_checksum_get_string(checksum));}
    return TRUE;
}

gboolean
bicmag_ntis_download_file(BicMagNtis *client, const gchar *uri,
                          const gchar *directory, const gchar *filename,
                          gchar **saved_path, GError **error)
{
    g_return_val_if_fail(client != NULL && uri != NULL, FALSE);
    g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, uri);
    if (message == NULL) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                       "Invalid download URI"); return FALSE; }
    return bicmag_ntis_download_message(client, message, directory, filename, saved_path, NULL,
                                        NULL, error);
}

gboolean
bicmag_ntis_download_post_form(BicMagNtis *client, const gchar *uri,
                               GHashTable *form, const gchar *directory,
                               const gchar *filename, gchar **saved_path,
                               GError **error)
{
    g_return_val_if_fail(client != NULL && uri != NULL && form != NULL, FALSE);
    g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_POST, uri);
    if (message == NULL) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                       "Invalid download URI"); return FALSE; }
    g_autofree gchar *encoded = soup_form_encode_hash(form);
    g_autoptr(GBytes) body = g_bytes_new(encoded, strlen(encoded));
    soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", body);
    return bicmag_ntis_download_message(client, message, directory, filename, saved_path, NULL,
                                        NULL, error);
}

gboolean
bicmag_ntis_download_file_with_digest(BicMagNtis *client, const gchar *uri,
                                      const gchar *directory, const gchar *filename,
                                      gchar **saved_path, guint64 *bytes_written,
                                      gchar **sha256, GError **error)
{
    g_return_val_if_fail(client != NULL && uri != NULL, FALSE);
    g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, uri);
    if (message == NULL) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                       "Invalid download URI"); return FALSE; }
    return bicmag_ntis_download_message(client, message, directory, filename,
                                        saved_path, bytes_written, sha256, error);
}

gboolean
bicmag_ntis_download_post_form_with_digest(BicMagNtis *client, const gchar *uri,
                                           GHashTable *form, const gchar *directory,
                                           const gchar *filename, gchar **saved_path,
                                           guint64 *bytes_written, gchar **sha256,
                                           GError **error)
{
    g_return_val_if_fail(client != NULL && uri != NULL && form != NULL, FALSE);
    g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_POST, uri);
    if (message == NULL) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                       "Invalid download URI"); return FALSE; }
    g_autofree gchar *encoded = soup_form_encode_hash(form);
    g_autoptr(GBytes) body = g_bytes_new(encoded, strlen(encoded));
    soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", body);
    return bicmag_ntis_download_message(client, message, directory, filename,
                                        saved_path, bytes_written, sha256, error);
}
