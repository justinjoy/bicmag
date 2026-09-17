#ifndef BICMAG_NTIS_H
#define BICMAG_NTIS_H
#include <glib.h>
#include <libsoup/soup.h>
G_BEGIN_DECLS
typedef struct { SoupSession *session; } BicMagNtis;
BicMagNtis *bicmag_ntis_new(void);
void bicmag_ntis_free(BicMagNtis *client);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(BicMagNtis, bicmag_ntis_free)
gchar *bicmag_ntis_get_html(BicMagNtis *client, const gchar *uri, GError **error);
gchar *bicmag_ntis_post_form(BicMagNtis *client, const gchar *uri,
                             GHashTable *form, GError **error);
gboolean bicmag_ntis_download_file(BicMagNtis *client, const gchar *uri,
                                   const gchar *directory,
                                   const gchar *filename,
                                   gchar **saved_path, GError **error);
gboolean bicmag_ntis_download_post_form(BicMagNtis *client, const gchar *uri,
                                        GHashTable *form,
                                        const gchar *directory,
                                        const gchar *filename,
                                        gchar **saved_path, GError **error);
G_END_DECLS
#endif
