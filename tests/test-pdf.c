#include "bicmag/pdf.h"

#include <glib.h>
#include <glib/gstdio.h>

static void
test_invalid_arguments(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = (gchar *)0x1;

    g_assert_false(bicmag_pdf_extract_text(NULL, &text, &error));
    g_assert_null(text);
    g_assert_error(error, BICMAG_PDF_ERROR,
                   BICMAG_PDF_ERROR_INVALID_ARGUMENT);
}

static void
test_missing_file(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    g_assert_false(bicmag_pdf_extract_text("does-not-exist.pdf", &text,
                                           &error));
    g_assert_null(text);
    g_assert_error(error, BICMAG_PDF_ERROR, BICMAG_PDF_ERROR_OPEN);
}

static void
test_corrupt_file(void)
{
    g_autofree gchar *path =
        g_build_filename(g_get_tmp_dir(), "bicmag-corrupt.pdf", NULL);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    g_assert_true(g_file_set_contents(path, "not a pdf", -1, &error));
    g_assert_false(bicmag_pdf_extract_text(path, &text, &error));
    g_assert_null(text);
    g_assert_nonnull(error);
    g_remove(path);
}

static void
test_text_extraction(void)
{
    g_autofree gchar *fixture =
        g_test_build_filename(G_TEST_BUILT, "sample.pdf", NULL);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;

    g_assert_true(bicmag_pdf_extract_text(fixture, &text, &error));
    g_assert_no_error(error);
    g_assert_nonnull(text);
    g_assert_nonnull(g_strstr_len(text, -1, "BicMag PDF"));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/pdf/invalid-arguments", test_invalid_arguments);
    g_test_add_func("/pdf/missing-file", test_missing_file);
    g_test_add_func("/pdf/corrupt-file", test_corrupt_file);
    g_test_add_func("/pdf/text-extraction", test_text_extraction);
    return g_test_run();
}
