#ifndef BICMAG_ATTACHMENT_H
#define BICMAG_ATTACHMENT_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    gchar *id;
    gchar *notice_id;
    gchar *name;
    gchar *download_url;
    gchar *local_path;
    gchar *sha256;
} BicMagAttachment;

BicMagAttachment *bicmag_attachment_new(void);
void bicmag_attachment_free(BicMagAttachment *attachment);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(BicMagAttachment, bicmag_attachment_free)

GPtrArray *bicmag_ntis_parse_attachments(const gchar *html,
                                          const gchar *base_uri,
                                          GError **error);

G_END_DECLS

#endif
