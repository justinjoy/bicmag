#include "bicmag/attachment.h"
#include <string.h>

BicMagAttachment *bicmag_attachment_new(void) { return g_new0(BicMagAttachment, 1); }
void bicmag_attachment_free(BicMagAttachment *a) { if (!a) return; g_free(a->id); g_free(a->notice_id); g_free(a->name); g_free(a->download_url); g_free(a); }

GPtrArray *bicmag_ntis_parse_attachments(const gchar *html, const gchar *base_uri, GError **error)
{
    if (html == NULL || base_uri == NULL) { g_set_error(error, G_URI_ERROR, G_URI_ERROR_FAILED, "HTML and base URI are required"); return NULL; }
    g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func((GDestroyNotify) bicmag_attachment_free);
    g_autoptr(GRegex) re = g_regex_new("<a[^>]*href=\\\"([^\\\"]+)\\\"[^>]*onclick=\\\"([^\\\"]*fn_fileDownload[^\\\"]*)\\\"[^>]*>(.*?)</a>", G_REGEX_CASELESS|G_REGEX_DOTALL, 0, NULL);
    g_autoptr(GMatchInfo) mi = NULL; g_regex_match(re, html, 0, &mi);
    while (g_match_info_matches(mi)) {
        g_autofree gchar *href = g_match_info_fetch(mi, 1), *onclick = g_match_info_fetch(mi, 2), *name = g_match_info_fetch(mi, 3);
        g_autoptr(GRegex) args = g_regex_new("fn_fileDownload\\s*\\(\\s*['\\\"]([^'\\\"]+)['\\\"]\\s*,\\s*['\\\"]([^'\\\"]+)['\\\"]", 0, 0, NULL);
        g_autoptr(GMatchInfo) ai = NULL;
        if (g_regex_match(args, onclick, 0, &ai)) {
            g_autoptr(BicMagAttachment) a = bicmag_attachment_new(); a->id = g_match_info_fetch(ai,1); a->notice_id = g_match_info_fetch(ai,2); g_strstrip(name); a->name = g_strdup(name);
            g_autoptr(GUri) base = g_uri_parse(base_uri, 0, NULL); g_autoptr(GUri) uri = base ? g_uri_parse_relative(base, href, 0, NULL) : NULL;
            if (uri) a->download_url = g_uri_to_string(uri); if (a->name && a->download_url) g_ptr_array_add(out, g_steal_pointer(&a));
        }
        g_match_info_next(mi, NULL);
    }
    return g_steal_pointer(&out);
}
