#ifndef BICMAG_INDEX_H
#define BICMAG_INDEX_H

#include "bicmag/cache.h"

G_BEGIN_DECLS

gboolean bicmag_index_pdf_notice(BicMagCache *cache, const gchar *notice_id,
                                 const gchar *pdf_path, GError **error);

gboolean bicmag_index_extract_attachment_text(const gchar *path,
                                              gchar **text_out,
                                              GError **error);

G_END_DECLS

#endif
