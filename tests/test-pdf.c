#include "bicmag/pdf.h"
#include "bicmag/notice.h"
#include "bicmag/cache.h"
#include "bicmag/ntis.h"
#include "bicmag/list.h"

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

static void
test_notice_date_rules(void)
{
    g_autoptr(GDateTime) now = g_date_time_new_local(2026, 9, 17, 12, 0, 0);
    g_autoptr(GError) error = NULL;

    g_assert_true(bicmag_notice_is_eligible("2026.09.20", "2026.10.01",
                                            "접수예정", now, &error));
    g_assert_true(bicmag_notice_is_eligible("2026.09.01", "2026.09.17",
                                            "마감", now, &error));
    g_assert_false(bicmag_notice_is_eligible("2026.09.01", "2026.09.16",
                                             "접수중", now, &error));
    g_assert_true(bicmag_notice_is_eligible("2026.09.20", NULL, "접수예정",
                                            now, &error));
    g_assert_false(bicmag_notice_is_eligible("2026.09.01", NULL, "접수예정",
                                             now, &error));
    g_assert_true(bicmag_notice_is_eligible("2026.09.01", NULL, "접수중",
                                            now, &error));
    g_assert_false(bicmag_notice_is_eligible("2026.09.20", NULL, "접수중",
                                             now, &error));
    g_assert_false(bicmag_notice_is_eligible("2026.09.20", NULL, "마감",
                                             now, &error));
    g_assert_false(bicmag_notice_is_eligible("2026.09.17junk", NULL, "접수중",
                                             now, &error));
    g_assert_error(error, BICMAG_NOTICE_ERROR, 3);
    g_clear_error(&error);
    g_assert_no_error(error);
}

static void
test_local_notice_cache(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(BicMagCache) cache = bicmag_cache_open(":memory:", &error);
    g_autoptr(BicMagNotice) notice = bicmag_notice_new();
    g_autoptr(BicMagNotice) second_notice = bicmag_notice_new();
    g_autoptr(GPtrArray) results = NULL;

    g_assert_no_error(error);
    g_assert_nonnull(cache);
    notice->id = g_strdup("notice-1");
    notice->title = g_strdup("태양광 연구개발 공고");
    notice->ministry = g_strdup("과학기술정보통신부");
    notice->status = g_strdup("접수중");
    g_assert_true(bicmag_cache_upsert_notice(cache, notice, TRUE, 1, &error));
    g_assert_no_error(error);
    second_notice->id = g_strdup("notice-2");
    second_notice->title = g_strdup("해양 연구개발 공고");
    second_notice->ministry = g_strdup("해양수산부");
    second_notice->status = g_strdup("접수중");
    g_assert_true(bicmag_cache_upsert_notice(cache, second_notice, TRUE, 1,
                                             &error));
    g_assert_no_error(error);
    results = bicmag_cache_search_notices(cache, "태양광", &error);
    g_assert_no_error(error);
    g_assert_nonnull(results);
    g_assert_cmpuint(results->len, ==, 1);
    g_assert_cmpstr(((BicMagNotice *)g_ptr_array_index(results, 0))->id, ==,
                    "notice-1");
}

static void
test_ntis_client(void)
{
    g_autoptr(BicMagNtis) client = bicmag_ntis_new();
    g_assert_nonnull(client);
    g_assert_nonnull(client->session);
}

static void
test_ntis_list_parser(void)
{
    g_autofree gchar *html = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_get_contents("../tests/data/ntis-list.html", &html, NULL, &error));
    g_autoptr(GPtrArray) notices = bicmag_ntis_parse_notices(html,
        "https://www.ntis.go.kr/rndgate/eg/un/ra/mng.do", &error);
    g_assert_no_error(error);
    g_assert_cmpuint(notices->len, ==, 2);
    g_assert_cmpstr(((BicMagNotice *)notices->pdata[0])->id, ==, "77109");
    g_assert_cmpstr(((BicMagNotice *)notices->pdata[0])->ministry, ==, "국방부");
    g_assert_cmpstr(((BicMagNotice *)notices->pdata[1])->title, ==,
                    "핵심인재 & 글로벌 브릿지");
    g_assert_true(g_str_has_prefix(((BicMagNotice *)notices->pdata[0])->detail_url,
                                   "https://www.ntis.go.kr/"));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/pdf/invalid-arguments", test_invalid_arguments);
    g_test_add_func("/pdf/missing-file", test_missing_file);
    g_test_add_func("/pdf/corrupt-file", test_corrupt_file);
    g_test_add_func("/pdf/text-extraction", test_text_extraction);
    g_test_add_func("/notice/date-rules", test_notice_date_rules);
    g_test_add_func("/cache/local-search", test_local_notice_cache);
    g_test_add_func("/ntis/client", test_ntis_client);
    g_test_add_func("/ntis/list-parser", test_ntis_list_parser);
    return g_test_run();
}
