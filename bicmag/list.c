#include "bicmag/list.h"

GQuark
bicmag_ntis_list_error_quark(void)
{
    return g_quark_from_static_string("bicmag-ntis-list-error-quark");
}

static gchar *
clean_text(const gchar *value)
{
    g_autofree gchar *decoded = g_strdup(value ? value : "");
    const gchar *entities[][2] = {{"&amp;", "&"}, {"&lt;", "<"},
                                  {"&gt;", ">"}, {"&quot;", "\""},
                                  {"&#39;", "'"}};
    for (guint i = 0; i < G_N_ELEMENTS(entities); i++) {
        g_autofree gchar *replacement = g_regex_escape_string(entities[i][0], -1);
        g_autoptr(GRegex) entity = g_regex_new(replacement, 0, 0, NULL);
        g_autofree gchar *next = g_regex_replace_literal(entity, decoded, -1, 0,
                                                         entities[i][1], 0, NULL);
        g_free(g_steal_pointer(&decoded));
        decoded = g_steal_pointer(&next);
    }
    g_autoptr(GString) out = g_string_new(NULL);
    gboolean space = FALSE;
    for (const gchar *p = decoded; *p; p++) {
        if (g_ascii_isspace(*p)) { space = out->len > 0; continue; }
        if (space) { g_string_append_c(out, ' '); space = FALSE; }
        g_string_append_c(out, *p);
    }
    return g_string_free(g_steal_pointer(&out), FALSE);
}

static gchar *
cell_text(const gchar *row, const gchar *label)
{
    g_autofree gchar *pattern = g_strdup_printf(
        "data-title\\s*=\\s*['\"]%s['\"][^>]*>(.*?)</", label);
    g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
    g_autoptr(GMatchInfo) match = NULL;
    if (!g_regex_match(regex, row, 0, &match)) return NULL;
    g_autofree gchar *raw = g_match_info_fetch(match, 1);
    g_autoptr(GRegex) tags = g_regex_new("<[^>]+>", G_REGEX_DOTALL, 0, NULL);
    g_autofree gchar *plain = g_regex_replace(tags, raw, -1, 0, "", 0, NULL);
    return clean_text(plain);
}

static gchar *
attribute(const gchar *row, const gchar *name)
{
    g_autofree gchar *pattern = g_strdup_printf("%s\\s*=\\s*['\"]([^'\"]+)", name);
    g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
    g_autoptr(GMatchInfo) match = NULL;
    if (!g_regex_match(regex, row, 0, &match)) return NULL;
    return g_match_info_fetch(match, 1);
}

GPtrArray *
bicmag_ntis_parse_notices(const gchar *html, const gchar *base_uri, GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (html == NULL || base_uri == NULL || *html == '\0') {
        g_set_error(error, BICMAG_NTIS_LIST_ERROR, 1, "HTML and base URI are required");
        return NULL;
    }
    g_autoptr(GPtrArray) notices = g_ptr_array_new_with_free_func((GDestroyNotify)bicmag_notice_free);
    g_autoptr(GRegex) rows = g_regex_new("<tr\\b[^>]*>(.*?)</tr>", G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
    g_autoptr(GMatchInfo) info = NULL;
    g_regex_match(rows, html, 0, &info);
    while (g_match_info_matches(info)) {
        g_autofree gchar *row = g_match_info_fetch(info, 1);
        g_autofree gchar *id = attribute(row, "value");
        g_autofree gchar *title = cell_text(row, "공고명");
        if (id == NULL || title == NULL || *title == '\0') { g_match_info_next(info, NULL); continue; }
        g_autoptr(BicMagNotice) notice = bicmag_notice_new();
        notice->id = g_steal_pointer(&id);
        notice->title = g_steal_pointer(&title);
        notice->ministry = cell_text(row, "부처명");
        notice->receipt_date = cell_text(row, "접수일");
        notice->deadline_date = cell_text(row, "마감일");
        notice->status = cell_text(row, "현황");
        g_autofree gchar *href = attribute(row, "href");
        if (href != NULL && !g_str_has_prefix(href, "javascript:")) {
            g_autoptr(GUri) base = g_uri_parse(base_uri, G_URI_FLAGS_NONE, NULL);
            g_autoptr(GUri) resolved = base ? g_uri_parse_relative(base, href, G_URI_FLAGS_NONE, NULL) : NULL;
            if (resolved) notice->detail_url = g_uri_to_string(resolved);
        }
        g_ptr_array_add(notices, g_steal_pointer(&notice));
        g_match_info_next(info, NULL);
    }
    return g_steal_pointer(&notices);
}
