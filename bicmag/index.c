#include "bicmag/index.h"
#include "bicmag/pdf.h"
#include <gio/gio.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <string.h>

static gchar *
bicmag_index_find_extractor(const gchar *name)
{
    g_autofree gchar *program = NULL;

    if (g_strcmp0(name, "hwp5html") == 0) {
        const gchar *configured = g_getenv("BICMAG_HWP5HTML");
        if (configured != NULL && g_file_test(configured, G_FILE_TEST_IS_EXECUTABLE)) {
            program = g_strdup(configured);
        } else {
            g_autofree gchar *data_dir = g_build_filename(g_get_user_data_dir(),
                                                           "bicmag", "hwp-tools",
                                                           "bin", "hwp5html", NULL);
            if (g_file_test(data_dir, G_FILE_TEST_IS_EXECUTABLE)) {
                program = g_steal_pointer(&data_dir);
            }
        }
    }
    if (program == NULL) {
        program = g_find_program_in_path(name);
    }
    return g_steal_pointer(&program);
}

static gboolean
bicmag_index_run_extractor(const gchar *const *argv,
                           gchar **stdout_out,
                           GError **error)
{
    g_autofree gchar *program = bicmag_index_find_extractor(argv[0]);
    g_auto(GStrv) resolved_argv = NULL;
    g_autoptr(GSubprocess) process = NULL;
    g_autofree gchar *stdout_text = NULL;
    g_autofree gchar *stderr_text = NULL;

    if (program == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "required extractor is not installed: %s", argv[0]);
        return FALSE;
    }
    resolved_argv = g_strdupv((gchar **)argv);
    g_free(resolved_argv[0]);
    resolved_argv[0] = g_steal_pointer(&program);
    process = g_subprocess_newv((const gchar *const *)resolved_argv,
                                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                error);
    if (process == NULL ||
        !g_subprocess_communicate_utf8(process, NULL, NULL, &stdout_text,
                                       &stderr_text, error)) {
        return FALSE;
    }
    if (!g_subprocess_get_successful(process)) {
        gboolean timed_out = g_strcmp0(argv[0], "timeout") == 0 &&
                             g_subprocess_get_if_exited(process) &&
                             g_subprocess_get_exit_status(process) == 124;
        g_set_error(error, G_IO_ERROR,
                    timed_out ? G_IO_ERROR_TIMED_OUT : G_IO_ERROR_FAILED,
                    "%s failed: %s", argv[0],
                    timed_out ? "attachment extraction timed out" :
                    stderr_text != NULL && *stderr_text != '\0' ? stderr_text :
                    "unknown extractor error");
        return FALSE;
    }
    *stdout_out = g_steal_pointer(&stdout_text);
    return TRUE;
}

static void
bicmag_index_append_html_text(xmlNode *node, GString *text)
{
    for (xmlNode *current = node; current != NULL; current = current->next) {
        if (current->type == XML_TEXT_NODE || current->type == XML_CDATA_SECTION_NODE) {
            xmlChar *value = xmlNodeGetContent(current);
            if (value != NULL) {
                g_string_append(text, (const gchar *)value);
                xmlFree(value);
            }
        } else if (current->type == XML_ELEMENT_NODE) {
            bicmag_index_append_html_text(current->children, text);
            if (text->len > 0 && !g_ascii_isspace(text->str[text->len - 1])) {
                g_string_append_c(text, ' ');
            }
        }
    }
}

static xmlNode *
bicmag_index_find_html_body(xmlNode *node)
{
    for (xmlNode *current = node; current != NULL; current = current->next) {
        if (current->type == XML_ELEMENT_NODE &&
            xmlStrcasecmp(current->name, (const xmlChar *)"body") == 0) {
            return current;
        }
        xmlNode *found = bicmag_index_find_html_body(current->children);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static gboolean
bicmag_index_html_to_text(const gchar *html, gchar **text_out, GError **error)
{
    htmlDocPtr document = htmlReadMemory(html, (int)strlen(html), NULL, "UTF-8",
                                         HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
                                         HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    if (document == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "cannot parse HWP converter output");
        return FALSE;
    }
    g_autoptr(GString) text = g_string_new(NULL);
    xmlNode *body = bicmag_index_find_html_body(xmlDocGetRootElement(document));
    bicmag_index_append_html_text(body != NULL ? body->children :
                                  xmlDocGetRootElement(document), text);
    xmlFreeDoc(document);
    g_strstrip(text->str);
    *text_out = g_string_free(g_steal_pointer(&text), FALSE);
    return TRUE;
}

gboolean
bicmag_index_extract_attachment_text(const gchar *path,
                                     gchar **text_out,
                                     GError **error)
{
    g_autofree gchar *lower_path = NULL;

    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (text_out != NULL) {
        *text_out = NULL;
    }
    if (path == NULL || text_out == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "path and text output are required");
        return FALSE;
    }
    lower_path = g_ascii_strdown(path, -1);
    if (g_str_has_suffix(lower_path, ".pdf")) {
        return bicmag_pdf_extract_text(path, text_out, error);
    }
    if (g_str_has_suffix(lower_path, ".hwp")) {
        g_autofree gchar *hwp5html = bicmag_index_find_extractor("hwp5html");
        if (hwp5html == NULL) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "required extractor is not installed: hwp5html");
            return FALSE;
        }
        const gchar *argv[] = { "timeout", "30s", hwp5html, "--html", path, NULL };
        g_autofree gchar *html = NULL;
        if (!bicmag_index_run_extractor(argv, &html, error)) {
            return FALSE;
        }
        return bicmag_index_html_to_text(html, text_out, error);
    }
    if (g_str_has_suffix(lower_path, ".hwpx")) {
        const gchar *argv[] = { "bsdtar", "-xOf", path,
                                "Preview/PrvText.txt", NULL };
        return bicmag_index_run_extractor(argv, text_out, error);
    }
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "unsupported attachment format: %s", path);
    return FALSE;
}

gboolean
bicmag_index_pdf_notice(BicMagCache *cache, const gchar *notice_id,
                        const gchar *pdf_path, GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (cache == NULL || notice_id == NULL || pdf_path == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "cache, notice id and PDF path are required");
        return FALSE;
    }
    if (!bicmag_pdf_has_signature(pdf_path, error)){return FALSE;}
    g_autofree gchar *text = NULL;
    if (!bicmag_pdf_extract_text(pdf_path, &text, error)){return FALSE;}
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, (const guchar *)text, strlen(text));
    const gchar *digest = g_checksum_get_string(checksum);
    sqlite3_stmt *statement = NULL;
    if (sqlite3_exec(cache->database, "BEGIN IMMEDIATE;", NULL, NULL,
                     NULL) != SQLITE_OK) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                                       "begin PDF index transaction"); return FALSE;
    }
    if (sqlite3_prepare_v2(cache->database,
                           "SELECT 1 FROM notice_pdf_hashes WHERE notice_id = ? AND sha256 = ?;",
                           -1, &statement, NULL) != SQLITE_OK) { g_set_error(error, G_IO_ERROR,
                                                                             G_IO_ERROR_FAILED,
                                                                             "prepare PDF hash lookup: %s",
                                                                             sqlite3_errmsg(
                                                                                 cache->database));
                                                                 sqlite3_exec(cache->database,
                                                                              "ROLLBACK;", NULL,
                                                                              NULL, NULL);
                                                                 return FALSE; }
    sqlite3_bind_text(statement, 1, notice_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, digest, -1, SQLITE_TRANSIENT);
    int hash_result = sqlite3_step(statement); sqlite3_finalize(statement);
    if (hash_result == SQLITE_ROW) { sqlite3_exec(cache->database, "ROLLBACK;", NULL, NULL, NULL);
                                     return TRUE; }
    if (hash_result != SQLITE_DONE) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                                  "lookup PDF hash: %s",
                                                  sqlite3_errmsg(cache->database));
                                      sqlite3_exec(cache->database, "ROLLBACK;", NULL, NULL, NULL);
                                      return FALSE; }
    if (sqlite3_prepare_v2(cache->database,
                           "SELECT content FROM notice_fts WHERE notice_id = ? LIMIT 1;", -1,
                           &statement, NULL) != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "prepare PDF lookup: %s",
                    sqlite3_errmsg(cache->database));
        sqlite3_exec(cache->database, "ROLLBACK;", NULL, NULL, NULL); return FALSE;
    }
    sqlite3_bind_text(statement, 1, notice_id, -1, SQLITE_TRANSIENT);
    int lookup = sqlite3_step(statement);
    if (lookup == SQLITE_DONE) { sqlite3_finalize(statement);
                                 g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                             "notice is not present in the local FTS index");
                                 sqlite3_exec(cache->database, "ROLLBACK;", NULL, NULL, NULL);
                                 return FALSE; }
    if (lookup != SQLITE_ROW) { sqlite3_finalize(statement);
                                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                            "lookup PDF index: %s",
                                            sqlite3_errmsg(cache->database));
                                sqlite3_exec(cache->database, "ROLLBACK;", NULL, NULL, NULL);
                                return FALSE; }
    const gchar *old = (const gchar *)sqlite3_column_text(statement, 0);
    if (old != NULL && g_strcmp0(old, text) == 0) { sqlite3_finalize(statement);
                                                    sqlite3_exec(cache->database, "ROLLBACK;", NULL,
                                                                 NULL, NULL); return TRUE; }
    sqlite3_finalize(statement);
    if (sqlite3_prepare_v2(cache->database,
                           "UPDATE notice_fts SET content = CASE WHEN content IS NULL OR content = '' "
                           "THEN ? ELSE content || ' ' || ? END WHERE notice_id = ?;",
                           -1, &statement, NULL) != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "prepare PDF index: %s",
                    sqlite3_errmsg(cache->database));
        sqlite3_exec(cache->database, "ROLLBACK;", NULL, NULL, NULL); return FALSE;
    }
    sqlite3_bind_text(statement, 1, text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, notice_id, -1, SQLITE_TRANSIENT);
    gboolean ok = sqlite3_step(statement) == SQLITE_DONE;
    if (ok && sqlite3_changes(cache->database) != 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "notice is not present in the local FTS index");
        ok = FALSE;
    }
    if (!ok && (error == NULL || *error == NULL)){g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                                              "index PDF: %s",
                                                              sqlite3_errmsg(cache->database));}
    sqlite3_finalize(statement);
    if (!ok) { sqlite3_exec(cache->database, "ROLLBACK;", NULL, NULL, NULL); return FALSE; }
    if (ok) {
        if (sqlite3_prepare_v2(cache->database,
                               "INSERT OR IGNORE INTO notice_pdf_hashes(notice_id, sha256) VALUES(?, ?);",
                               -1, &statement, NULL) != SQLITE_OK) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "prepare PDF hash insert: %s",
                        sqlite3_errmsg(cache->database));
            sqlite3_exec(cache->database, "ROLLBACK;", NULL, NULL, NULL); return FALSE;
        }
        sqlite3_bind_text(statement, 1, notice_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, digest, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement) != SQLITE_DONE) { g_set_error(error, G_IO_ERROR,
                                                                  G_IO_ERROR_FAILED,
                                                                  "store PDF hash: %s",
                                                                  sqlite3_errmsg(cache->database));
                                                      sqlite3_finalize(statement);
                                                      sqlite3_exec(cache->database, "ROLLBACK;",
                                                                   NULL, NULL, NULL); return FALSE;}
        sqlite3_finalize(statement);
    }
    if (ok && sqlite3_exec(cache->database, "COMMIT;", NULL, NULL,
                           NULL) != SQLITE_OK) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                                             "commit PDF index");
                                                 sqlite3_exec(cache->database, "ROLLBACK;", NULL,
                                                              NULL, NULL); return FALSE; }
    return ok;
}
