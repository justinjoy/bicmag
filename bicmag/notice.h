#ifndef BICMAG_NOTICE_H
#define BICMAG_NOTICE_H

#include <glib.h>

G_BEGIN_DECLS

#define BICMAG_NOTICE_ERROR (bicmag_notice_error_quark())

GQuark bicmag_notice_error_quark(void);

typedef struct {
    gchar *id;
    gchar *title;
    gchar *ministry;
    gchar *receipt_date;
    gchar *deadline_date;
    gchar *status;
    gchar *detail_url;
} BicMagNotice;

BicMagNotice *bicmag_notice_new(void);
BicMagNotice *bicmag_notice_copy(const BicMagNotice *notice);
void bicmag_notice_free(BicMagNotice *notice);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(BicMagNotice, bicmag_notice_free)

/* Dates use NTIS's YYYY.MM.DD representation and are inclusive. */
gboolean bicmag_notice_is_eligible(const gchar *receipt_date,
                                   const gchar *deadline_date,
                                   const gchar *status,
                                   GDateTime *now,
                                   GError **error);

G_END_DECLS

#endif
