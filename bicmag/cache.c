#include "bicmag/cache.h"

#include <gio/gio.h>

static void
bicmag_cache_set_error(GError **error, sqlite3 *database, const gchar *context)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", context,
                database != NULL ? sqlite3_errmsg(database) : "database error");
}

static gboolean
bicmag_cache_exec(BicMagCache *cache, const gchar *sql, GError **error)
{
    char *message = NULL;
    if (sqlite3_exec(cache->database, sql, NULL, NULL, &message) != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", sql,
                    message != NULL ? message : sqlite3_errmsg(cache->database));
        sqlite3_free(message);
        return FALSE;
    }
    return TRUE;
}

BicMagCache *
bicmag_cache_open(const gchar *path, GError **error)
{
    g_autoptr(BicMagCache) cache = NULL;
    sqlite3 *database = NULL;
    const gchar *schema =
        "PRAGMA foreign_keys = ON;"
        "CREATE TABLE IF NOT EXISTS notices ("
        "id TEXT PRIMARY KEY, title TEXT, ministry TEXT, receipt_date TEXT,"
        "deadline_date TEXT, status TEXT, detail_url TEXT, eligible INTEGER,"
        "synced_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS attachments (id TEXT PRIMARY KEY, notice_id TEXT NOT NULL REFERENCES notices(id) ON DELETE CASCADE, name TEXT, download_url TEXT, local_path TEXT, sha256 TEXT, synced_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS notice_pdf_hashes (notice_id TEXT NOT NULL, sha256 TEXT NOT NULL, UNIQUE(notice_id, sha256));"
        "CREATE VIRTUAL TABLE IF NOT EXISTS notice_fts USING fts5("
        "notice_id UNINDEXED, title, ministry, content);";

    if (path == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "cache path is required");
        return NULL;
    }
    if (sqlite3_open_v2(path, &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "open cache: %s",
                    sqlite3_errmsg(database));
        sqlite3_close(database);
        return NULL;
    }
    cache = g_new0(BicMagCache, 1);
    cache->database = database;
    if (!bicmag_cache_exec(cache, schema, error))
        return NULL;
    return g_steal_pointer(&cache);
}

gboolean
bicmag_cache_upsert_attachment(BicMagCache *cache, const gchar *notice_id,
                               const BicMagAttachment *attachment,
                               const gchar *local_path, const gchar *sha256,
                               gint64 synced_at, GError **error)
{
    if (cache == NULL || notice_id == NULL || attachment == NULL || attachment->id == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "cache, notice id and attachment id are required"); return FALSE;
    }
    sqlite3_stmt *statement = NULL;
    const gchar *sql = "INSERT INTO attachments(id,notice_id,name,download_url,local_path,sha256,synced_at) VALUES(?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET notice_id=excluded.notice_id,name=excluded.name,download_url=excluded.download_url,local_path=excluded.local_path,sha256=excluded.sha256,synced_at=excluded.synced_at;";
    if (sqlite3_prepare_v2(cache->database, sql, -1, &statement, NULL) != SQLITE_OK) { bicmag_cache_set_error(error, cache->database, "prepare attachment upsert"); return FALSE; }
    sqlite3_bind_text(statement,1,attachment->id,-1,SQLITE_TRANSIENT); sqlite3_bind_text(statement,2,notice_id,-1,SQLITE_TRANSIENT); sqlite3_bind_text(statement,3,attachment->name,-1,SQLITE_TRANSIENT); sqlite3_bind_text(statement,4,attachment->download_url,-1,SQLITE_TRANSIENT); sqlite3_bind_text(statement,5,local_path,-1,SQLITE_TRANSIENT); sqlite3_bind_text(statement,6,sha256,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(statement,7,synced_at);
    gboolean ok = sqlite3_step(statement) == SQLITE_DONE; if (!ok) bicmag_cache_set_error(error, cache->database, "upsert attachment"); sqlite3_finalize(statement); return ok;
}

GPtrArray *
bicmag_cache_list_attachments(BicMagCache *cache, const gchar *notice_id,
                               GError **error)
{
    g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func((GDestroyNotify)bicmag_attachment_free);
    if (cache == NULL || notice_id == NULL) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "cache and notice id are required"); return NULL; }
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(cache->database, "SELECT id,name,download_url,local_path,sha256 FROM attachments WHERE notice_id = ? ORDER BY id;", -1, &statement, NULL) != SQLITE_OK) { bicmag_cache_set_error(error, cache->database, "prepare attachment list"); return NULL; }
    sqlite3_bind_text(statement, 1, notice_id, -1, SQLITE_TRANSIENT);
    int code;
    while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
        g_autoptr(BicMagAttachment) item = bicmag_attachment_new();
        item->id = g_strdup((const gchar *)sqlite3_column_text(statement, 0)); item->notice_id = g_strdup(notice_id); item->name = g_strdup((const gchar *)sqlite3_column_text(statement, 1)); item->download_url = g_strdup((const gchar *)sqlite3_column_text(statement, 2)); item->local_path = g_strdup((const gchar *)sqlite3_column_text(statement, 3)); item->sha256 = g_strdup((const gchar *)sqlite3_column_text(statement, 4));
        g_ptr_array_add(result, g_steal_pointer(&item));
    }
    if (code != SQLITE_DONE) { bicmag_cache_set_error(error, cache->database, "list attachments"); sqlite3_finalize(statement); return NULL; }
    sqlite3_finalize(statement); return g_steal_pointer(&result);
}

void
bicmag_cache_close(BicMagCache *cache)
{
    if (cache == NULL)
        return;
    if (cache->database != NULL)
        sqlite3_close(cache->database);
    g_free(cache);
}

gboolean
bicmag_cache_upsert_notice(BicMagCache *cache,
                            const BicMagNotice *notice,
                            gboolean eligible,
                            gint64 synced_at,
                            GError **error)
{
    sqlite3_stmt *statement = NULL;
    gchar *existing_content = NULL;
    const gchar *sql =
        "INSERT INTO notices(id,title,ministry,receipt_date,deadline_date,status,detail_url,eligible,synced_at)"
        " VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET title=excluded.title,"
        "ministry=excluded.ministry,receipt_date=excluded.receipt_date,deadline_date=excluded.deadline_date,"
        "status=excluded.status,detail_url=excluded.detail_url,eligible=excluded.eligible,synced_at=excluded.synced_at";
    if (cache == NULL || notice == NULL || notice->id == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "cache and notice id are required");
        return FALSE;
    }
    if (!bicmag_cache_exec(cache, "BEGIN IMMEDIATE;", error))
        return FALSE;
    if (sqlite3_prepare_v2(cache->database, sql, -1, &statement, NULL) != SQLITE_OK) {
        bicmag_cache_set_error(error, cache->database, "prepare notice upsert");
        bicmag_cache_exec(cache, "ROLLBACK;", NULL);
        return FALSE;
    }
    sqlite3_bind_text(statement, 1, notice->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, notice->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, notice->ministry, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, notice->receipt_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, notice->deadline_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, notice->status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 7, notice->detail_url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 8, eligible ? 1 : 0);
    sqlite3_bind_int64(statement, 9, synced_at);
    if (sqlite3_step(statement) != SQLITE_DONE) {
        bicmag_cache_set_error(error, cache->database, "upsert notice");
        sqlite3_finalize(statement);
        bicmag_cache_exec(cache, "ROLLBACK;", NULL);
        return FALSE;
    }
    sqlite3_finalize(statement);
    if (sqlite3_prepare_v2(cache->database, "SELECT content FROM notice_fts WHERE notice_id = ? LIMIT 1;", -1, &statement, NULL) != SQLITE_OK) {
        bicmag_cache_set_error(error, cache->database, "lookup notice index");
        bicmag_cache_exec(cache, "ROLLBACK;", NULL);
        return FALSE;
    }
    sqlite3_bind_text(statement, 1, notice->id, -1, SQLITE_TRANSIENT);
    int lookup_result = sqlite3_step(statement);
    if (lookup_result == SQLITE_ROW)
        existing_content = g_strdup((const gchar *)sqlite3_column_text(statement, 0));
    else if (lookup_result != SQLITE_DONE) {
        bicmag_cache_set_error(error, cache->database, "lookup notice index");
        sqlite3_finalize(statement); bicmag_cache_exec(cache, "ROLLBACK;", NULL); return FALSE;
    }
    sqlite3_finalize(statement);
    if (sqlite3_prepare_v2(cache->database,
            "DELETE FROM notice_fts WHERE notice_id = ?;",
            -1, &statement, NULL) != SQLITE_OK) {
        bicmag_cache_set_error(error, cache->database, "prepare notice index");
        bicmag_cache_exec(cache, "ROLLBACK;", NULL);
        return FALSE;
    }
    sqlite3_bind_text(statement, 1, notice->id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_DONE) {
        bicmag_cache_set_error(error, cache->database, "update notice index");
        sqlite3_finalize(statement);
        bicmag_cache_exec(cache, "ROLLBACK;", NULL);
        return FALSE;
    }
    sqlite3_finalize(statement);
    if (sqlite3_prepare_v2(cache->database,
            "INSERT INTO notice_fts(notice_id,title,ministry,content) VALUES(?,?,?,?);",
            -1, &statement, NULL) != SQLITE_OK) {
        bicmag_cache_set_error(error, cache->database, "prepare notice index insert");
        bicmag_cache_exec(cache, "ROLLBACK;", NULL);
        return FALSE;
    }
    sqlite3_bind_text(statement, 1, notice->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, notice->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, notice->ministry, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, existing_content != NULL ? existing_content : notice->title, -1, SQLITE_TRANSIENT);
    g_free(existing_content);
    if (sqlite3_step(statement) != SQLITE_DONE) {
        bicmag_cache_set_error(error, cache->database, "update notice index");
        sqlite3_finalize(statement);
        bicmag_cache_exec(cache, "ROLLBACK;", NULL);
        return FALSE;
    }
    sqlite3_finalize(statement);
    return bicmag_cache_exec(cache, "COMMIT;", error);
}

GPtrArray *
bicmag_cache_search_notices(BicMagCache *cache, const gchar *query, GError **error)
{
    g_autoptr(GPtrArray) results =
        g_ptr_array_new_with_free_func((GDestroyNotify)bicmag_notice_free);
    sqlite3_stmt *statement = NULL;
    const gchar *sql = "SELECT n.id,n.title,n.ministry,n.receipt_date,n.deadline_date,n.status,n.detail_url "
                       "FROM notice_fts f JOIN notices n ON n.id=f.notice_id "
                       "WHERE n.eligible = 1 AND notice_fts MATCH ? "
                       "ORDER BY rank";
    if (cache == NULL || query == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "cache and query are required");
        return NULL;
    }
    if (sqlite3_prepare_v2(cache->database, sql, -1, &statement, NULL) != SQLITE_OK) {
        bicmag_cache_set_error(error, cache->database, "prepare notice search");
        return NULL;
    }
    sqlite3_bind_text(statement, 1, query, -1, SQLITE_TRANSIENT);
    int result_code;
    while ((result_code = sqlite3_step(statement)) == SQLITE_ROW) {
        g_autoptr(BicMagNotice) notice = bicmag_notice_new();
        notice->id = g_strdup((const gchar *)sqlite3_column_text(statement, 0));
        notice->title = g_strdup((const gchar *)sqlite3_column_text(statement, 1));
        notice->ministry = g_strdup((const gchar *)sqlite3_column_text(statement, 2));
        notice->receipt_date = g_strdup((const gchar *)sqlite3_column_text(statement, 3));
        notice->deadline_date = g_strdup((const gchar *)sqlite3_column_text(statement, 4));
        notice->status = g_strdup((const gchar *)sqlite3_column_text(statement, 5));
        notice->detail_url = g_strdup((const gchar *)sqlite3_column_text(statement, 6));
        g_ptr_array_add(results, g_steal_pointer(&notice));
    }
    if (result_code != SQLITE_DONE) {
        bicmag_cache_set_error(error, cache->database, "search notices");
        sqlite3_finalize(statement);
        return NULL;
    }
    sqlite3_finalize(statement);
    return g_steal_pointer(&results);
}
