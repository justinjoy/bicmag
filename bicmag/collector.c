#include "bicmag/collector.h"

#include "bicmag/attachment.h"
#include "bicmag/index.h"
#include "bicmag/list.h"
#include "bicmag/notice.h"

#include <gio/gio.h>

static gchar *
bicmag_collector_safe_filename(const gchar *name)
{
    g_autofree gchar *base = g_path_get_basename(name != NULL ? name : "attachment");

    for (gchar *cursor = base; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\' || *cursor < 0x20) {
            *cursor = '_';
        }
    }
    return g_strdup(*base != '\0' ? base : "attachment");
}

static gboolean
bicmag_collector_sync_attachment(BicMagCache *cache,
                                 BicMagNtis *client,
                                 BicMagAttachment *attachment,
                                 const gchar *notice_id,
                                 const gchar *directory,
                                 gint64 synced_at,
                                 GError **error)
{
    g_autoptr(GHashTable) form = g_hash_table_new(g_str_hash, g_str_equal);
    g_autofree gchar *filename = bicmag_collector_safe_filename(attachment->name);
    g_autofree gchar *path = g_build_filename(directory, filename, NULL);
    g_autofree gchar *saved_path = NULL;
    g_autofree gchar *sha256 = NULL;

    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        attachment->local_path = g_strdup(path);
        return bicmag_cache_upsert_attachment(cache, notice_id, attachment, path, NULL,
                                              synced_at, error);
    }
    g_hash_table_insert(form, "wfUid", attachment->id);
    g_hash_table_insert(form, "roTextUid", attachment->notice_id);
    if (!bicmag_ntis_download_post_form_with_digest(client, attachment->download_url, form,
                                                    directory, filename, &saved_path, NULL,
                                                    &sha256, error)) {
        return FALSE;
    }
    attachment->local_path = g_steal_pointer(&saved_path);
    attachment->sha256 = g_steal_pointer(&sha256);
    return bicmag_cache_upsert_attachment(cache, notice_id, attachment,
                                          attachment->local_path, attachment->sha256,
                                          synced_at, error);
}

static void
bicmag_collector_append_index_text(GString *content, const gchar *text)
{
    if (text == NULL || *text == '\0') {
        return;
    }
    if (content->len > 0) {
        g_string_append_c(content, '\n');
    }
    g_string_append(content, text);
}

gboolean
bicmag_collector_sync(BicMagCache *cache,
                      BicMagNtis *client,
                      const gchar *list_uri,
                      const gchar *attachment_directory,
                      GError **error)
{
    g_autofree gchar *html = NULL;

    g_autoptr(GPtrArray) notices = NULL;
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    gint64 synced_at = g_get_real_time() / G_TIME_SPAN_SECOND;

    g_return_val_if_fail(cache != NULL && client != NULL, FALSE);
    if (list_uri == NULL || attachment_directory == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "list URI and attachment directory are required");
        return FALSE;
    }
    html = bicmag_ntis_get_html(client, list_uri, error);
    if (html == NULL) {
        return FALSE;
    }
    notices = bicmag_ntis_parse_notices(html, list_uri, error);
    if (notices == NULL) {
        return FALSE;
    }
    g_autofree gchar *today = g_date_time_format(now, "%Y.%m.%d");
    if (!bicmag_cache_remove_expired(cache, today, error)) {
        return FALSE;
    }
    for (guint i = 0; i < notices->len; ++i) {
        BicMagNotice *notice = g_ptr_array_index(notices, i);
        g_autoptr(GError) eligibility_error = NULL;
        gboolean eligible = bicmag_notice_is_eligible(notice->receipt_date,
                                                      notice->deadline_date,
                                                      notice->status, now,
                                                      &eligibility_error);
        if (eligibility_error != NULL) {
            g_propagate_error(error, g_steal_pointer(&eligibility_error));
            return FALSE;
        }
        if (!bicmag_cache_upsert_notice(cache, notice, eligible, synced_at, error)) {
            return FALSE;
        }
        if (!eligible || notice->detail_url == NULL) {
            continue;
        }
        g_autoptr(GHashTable) detail_form = g_hash_table_new(g_str_hash, g_str_equal);
        g_hash_table_insert(detail_form, "roRndUid", notice->id);
        g_hash_table_insert(detail_form, "flag", "rndList");
        g_autofree gchar *detail = bicmag_ntis_post_form(client, notice->detail_url,
                                                         detail_form, error);
        if (detail == NULL) {
            return FALSE;
        }
        g_autoptr(GPtrArray) attachments = bicmag_ntis_parse_attachments(
            detail, notice->detail_url, error);
        if (attachments == NULL) {
            return FALSE;
        }
        g_autofree gchar *notice_directory = g_build_filename(attachment_directory,
                                                              notice->id, NULL);
        g_autoptr(GString) indexed_content = g_string_new(NULL);
        for (guint j = 0; j < attachments->len; ++j) {
            BicMagAttachment *attachment = g_ptr_array_index(attachments, j);
            if (!bicmag_collector_sync_attachment(cache, client, attachment, notice->id,
                                                  notice_directory, synced_at, error)) {
                return FALSE;
            }
            bicmag_collector_append_index_text(indexed_content, attachment->name);
            g_autofree gchar *attachment_text = NULL;
            g_autoptr(GError) extraction_error = NULL;
            if (bicmag_index_extract_attachment_text(attachment->local_path,
                                                     &attachment_text,
                                                     &extraction_error)) {
                bicmag_collector_append_index_text(indexed_content, attachment_text);
            } else if (!g_error_matches(extraction_error, G_IO_ERROR,
                                        G_IO_ERROR_NOT_SUPPORTED)) {
                g_warning("cannot index attachment %s: %s",
                          attachment->local_path,
                          extraction_error != NULL ? extraction_error->message :
                          "unknown extraction error");
            }
        }
        if (!bicmag_cache_replace_notice_content(cache, notice->id,
                                                 indexed_content->str, error)) {
            return FALSE;
        }
    }
    return TRUE;
}
