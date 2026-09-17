#include "bicmag/index.h"
#include "bicmag/pdf.h"
#include <gio/gio.h>

gboolean
bicmag_index_pdf_notice(BicMagCache*cache, const gchar*notice_id,
                        const gchar*pdf_path, GError**error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (cache == NULL || notice_id == NULL || pdf_path == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "cache, notice id and PDF path are required");
        return FALSE;
    }
    if (!bicmag_pdf_has_signature(pdf_path, error)){return FALSE;}
    g_autofree gchar*text = NULL;
    if (!bicmag_pdf_extract_text(pdf_path, &text, error)){return FALSE;}
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, (const guchar*)text, strlen(text));
    const gchar*digest = g_checksum_get_string(checksum);
    sqlite3_stmt*statement = NULL;
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
    const gchar*old = (const gchar*)sqlite3_column_text(statement, 0);
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
