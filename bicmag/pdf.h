#ifndef BICMAG_PDF_H
#define BICMAG_PDF_H

#include <glib.h>

G_BEGIN_DECLS

#define BICMAG_PDF_ERROR (bicmag_pdf_error_quark())

typedef enum {
    BICMAG_PDF_ERROR_INVALID_ARGUMENT,
    BICMAG_PDF_ERROR_OPEN,
    BICMAG_PDF_ERROR_PARSE,
    BICMAG_PDF_ERROR_EXTRACT,
} BicMagPdfError;

GQuark bicmag_pdf_error_quark(void);

/* Extracts text as a newly allocated UTF-8 string. Free text_out with g_free(). */
gboolean bicmag_pdf_extract_text(const gchar*path,
                                 gchar**text_out,
                                 GError**error);
gboolean bicmag_pdf_has_signature(const gchar*path, GError**error);

G_END_DECLS

#endif
