#ifndef BICMAG_COLLECTOR_H
#define BICMAG_COLLECTOR_H

#include "bicmag/cache.h"
#include "bicmag/ntis.h"

G_BEGIN_DECLS

#define BICMAG_NTIS_LIST_URI "https://www.ntis.go.kr/rndgate/eg/un/ra/mng.do"

gboolean bicmag_collector_sync(BicMagCache *cache,
                               BicMagNtis *client,
                               const gchar *list_uri,
                               const gchar *attachment_directory,
                               GError **error);

G_END_DECLS

#endif
