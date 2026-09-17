#include "bicmag/pdf.h"

#include <podofo/podofo.h>

#include <exception>
#include <string>
#include <vector>
#include <fstream>

GQuark
bicmag_pdf_error_quark(void)
{
    return g_quark_from_static_string("bicmag-pdf-error-quark");
}

static void
bicmag_pdf_set_error(GError **error,
                     BicMagPdfError code,
                     const gchar *message)
{
    if (error != nullptr && *error == nullptr){
        g_set_error(error, BICMAG_PDF_ERROR, code, "%s", message);
    }
}

extern "C" gboolean
bicmag_pdf_has_signature(const gchar *path, GError **error)
{
    if (path == nullptr) { bicmag_pdf_set_error(error, BICMAG_PDF_ERROR_INVALID_ARGUMENT,
                                                "path is required"); return FALSE; }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { bicmag_pdf_set_error(error, BICMAG_PDF_ERROR_OPEN, "cannot open PDF file");
                   return FALSE; }
    char header[5] = {};
    stream.read(header, sizeof header);
    if (stream.gcount() != 5 || std::string(header, 5) != "%PDF-") {
        bicmag_pdf_set_error(error, BICMAG_PDF_ERROR_PARSE, "file has no PDF signature");
        return FALSE;
    }
    return TRUE;
}

extern "C" gboolean
bicmag_pdf_extract_text(const gchar *path, gchar **text_out, GError **error)
{
    if (text_out != nullptr){
        *text_out = nullptr;
    }

    if (path == nullptr || text_out == nullptr) {
        bicmag_pdf_set_error(error, BICMAG_PDF_ERROR_INVALID_ARGUMENT,
                             "path and text_out are required");
        return FALSE;
    }

    try {
        PoDoFo::PdfMemDocument document;
        document.Load(path);

        std::string text;
        const auto &pages = document.GetPages();
        for (unsigned i = 0; i < pages.GetCount(); ++i) {
            std::vector<PoDoFo::PdfTextEntry> entries;
            pages.GetPageAt(i).ExtractTextTo(entries);
            for (const auto &entry : entries) {
                text.append(entry.Text);
            }
            if (i + 1 < pages.GetCount()){
                text.push_back('\n');
            }
        }

        *text_out = g_strdup(text.c_str());
        return TRUE;
    } catch (const PoDoFo::PdfError &exception) {
        const auto code = exception.GetCode();
        const auto category = code == PoDoFo::PdfErrorCode::FileNotFound ||
                              code == PoDoFo::PdfErrorCode::IOError
                                  ? BICMAG_PDF_ERROR_OPEN
                                  : BICMAG_PDF_ERROR_PARSE;
        bicmag_pdf_set_error(error, category, exception.what());
    } catch (const std::exception &exception) {
        bicmag_pdf_set_error(error, BICMAG_PDF_ERROR_EXTRACT,
                             exception.what());
    } catch (...) {
        bicmag_pdf_set_error(error, BICMAG_PDF_ERROR_EXTRACT,
                             "unknown PDF extraction error");
    }

    return FALSE;
}
