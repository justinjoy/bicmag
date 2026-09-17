#ifndef BICMAG_CACHE_H
#define BICMAG_CACHE_H

#include "bicmag/notice.h"

#include <sqlite3.h>

G_BEGIN_DECLS

typedef struct {
    sqlite3 *database;
} BicMagCache;

BicMagCache *bicmag_cache_open(const gchar *path, GError **error);
void bicmag_cache_close(BicMagCache *cache);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(BicMagCache, bicmag_cache_close)

gboolean bicmag_cache_upsert_notice(BicMagCache *cache,
                                    const BicMagNotice *notice,
                                    gboolean eligible,
                                    gint64 synced_at,
                                    GError **error);

GPtrArray *bicmag_cache_search_notices(BicMagCache *cache,
                                       const gchar *query,
                                       GError **error);

G_END_DECLS

#endif
