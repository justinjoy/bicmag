#ifndef BICMAG_LIST_H
#define BICMAG_LIST_H

#include "bicmag/notice.h"

G_BEGIN_DECLS

#define BICMAG_NTIS_LIST_ERROR (bicmag_ntis_list_error_quark())
GQuark bicmag_ntis_list_error_quark(void);

/* Parses NTIS's table rows identified by data-title attributes. */
GPtrArray *bicmag_ntis_parse_notices(const gchar *html,
                                     const gchar *base_uri,
                                     GError **error);

G_END_DECLS

#endif
