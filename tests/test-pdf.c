#include "bicmag/pdf.h"
#include "bicmag/notice.h"
#include "bicmag/cache.h"
#include "bicmag/ntis.h"
#include "bicmag/list.h"
#include "bicmag/attachment.h"
#include "bicmag/index.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup-server-message.h>

typedef struct { SoupServer*server; GMainLoop*loop; GMutex mutex; GCond cond; gchar*uri;
} TestServer;
static void test_server_cb(SoupServer*server, SoupServerMessage*msg, const gchar*path,
                           GHashTable*query, gpointer data) {
    (void)server; (void)query; (void)data;
    if (g_str_has_suffix(path, "error")) { soup_server_message_set_response(msg, "text/plain",
                                                                            SOUP_MEMORY_STATIC,
                                                                            "missing", 7);
                                           soup_server_message_set_status(msg, 404, "Not Found");
                                           return; }
    const gchar*body = "fixture-download";
    soup_server_message_set_status(msg, 200, NULL);
    soup_server_message_set_response(msg, "application/octet-stream", SOUP_MEMORY_STATIC, body,
                                     strlen(body));
}
static gpointer test_server_thread(gpointer data) {
    TestServer*t = data; g_autoptr(GError) error = NULL;
    GMainContext*context = g_main_context_new(); g_main_context_push_thread_default(context);

    t->server = soup_server_new(NULL, NULL);
    soup_server_add_handler(t->server, NULL, test_server_cb, NULL, NULL);
    g_assert_true(soup_server_listen_local(t->server, 0, 0, &error));
    GSList*uris = soup_server_get_uris(t->server);
    g_mutex_lock(&t->mutex); t->uri = g_uri_to_string(uris->data);
    t->loop = g_main_loop_new(g_main_context_get_thread_default(), FALSE); g_cond_signal(&t->cond);
    g_mutex_unlock(&t->mutex);
    g_main_loop_run(t->loop); g_main_context_pop_thread_default(context);
    g_main_context_unref(context); return NULL;
}

static void
test_invalid_arguments(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar*text = (gchar*)0x1;

    g_assert_false(bicmag_pdf_extract_text(NULL, &text, &error));
    g_assert_null(text);
    g_assert_error(error, BICMAG_PDF_ERROR,
                   BICMAG_PDF_ERROR_INVALID_ARGUMENT);
}

static void
test_missing_file(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar*text = NULL;

    g_assert_false(bicmag_pdf_extract_text("does-not-exist.pdf", &text,
                                           &error));
    g_assert_null(text);
    g_assert_error(error, BICMAG_PDF_ERROR, BICMAG_PDF_ERROR_OPEN);
}

static void
test_corrupt_file(void)
{
    g_autofree gchar*path =
        g_build_filename(g_get_tmp_dir(), "bicmag-corrupt.pdf", NULL);

    g_autoptr(GError) error = NULL;
    g_autofree gchar*text = NULL;

    g_assert_true(g_file_set_contents(path, "not a pdf", -1, &error));
    g_assert_false(bicmag_pdf_extract_text(path, &text, &error));
    g_assert_null(text);
    g_assert_nonnull(error);
    g_remove(path);
}

static void
test_text_extraction(void)
{
    g_autofree gchar*fixture =
        g_test_build_filename(G_TEST_BUILT, "sample.pdf", NULL);

    g_autoptr(GError) error = NULL;
    g_autofree gchar*text = NULL;

    g_assert_true(bicmag_pdf_extract_text(fixture, &text, &error));
    g_assert_no_error(error);
    g_assert_nonnull(text);
    g_assert_nonnull(g_strstr_len(text, -1, "BicMag PDF"));
}

static void
test_pdf_signature(void)
{
    g_autofree gchar*fixture = g_test_build_filename(G_TEST_BUILT, "sample.pdf", NULL);
    g_autofree gchar*path = g_build_filename(g_get_tmp_dir(), "bicmag-not-pdf.bin", NULL);

    g_autoptr(GError) error = NULL;
    g_assert_true(bicmag_pdf_has_signature(fixture, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(path, "hello", -1, &error));
    g_assert_false(bicmag_pdf_has_signature(path, &error));
    g_assert_error(error, BICMAG_PDF_ERROR, BICMAG_PDF_ERROR_PARSE);
    g_remove(path);
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
    g_autoptr(BicMagAttachment) attachment = bicmag_attachment_new();
    attachment->id = g_strdup("file-1"); attachment->name = g_strdup("doc.pdf");
    attachment->download_url = g_strdup("https://example.test/doc.pdf");
    g_assert_true(bicmag_cache_upsert_attachment(cache, "notice-1", attachment, "/tmp/doc.pdf",
                                                 "abc", 2, &error));
    g_assert_no_error(error);
    g_assert_false(bicmag_cache_upsert_attachment(cache, "missing", attachment, "/tmp/doc.pdf",
                                                  "abc", 2, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_autoptr(GPtrArray) attachments = bicmag_cache_list_attachments(cache, "notice-1", &error);
    g_assert_no_error(error); g_assert_cmpuint(attachments->len, ==, 1);
    g_assert_cmpstr(((BicMagAttachment*)attachments->pdata[0])->name, ==, "doc.pdf");
    g_assert_cmpstr(((BicMagAttachment*)attachments->pdata[0])->local_path, ==, "/tmp/doc.pdf");
    g_assert_cmpstr(((BicMagAttachment*)attachments->pdata[0])->sha256, ==, "abc");
    g_assert_true(bicmag_cache_remove_expired(cache, "2026.09.17", &error));
    g_assert_no_error(error);
    g_assert_cmpstr(((BicMagNotice*)g_ptr_array_index(results, 0))->id, ==,
                    "notice-1");
    g_autofree gchar*fixture = g_test_build_filename(G_TEST_BUILT, "sample.pdf", NULL);
    g_assert_true(bicmag_index_pdf_notice(cache, "notice-1", fixture, &error));
    g_clear_pointer(&results, g_ptr_array_unref);
    results = bicmag_cache_search_notices(cache, "BicMag", &error);
    g_assert_no_error(error);
    g_assert_cmpuint(results->len, ==, 1);
    g_clear_error(&error);
    g_assert_false(bicmag_index_pdf_notice(cache, "missing", fixture, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
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
    g_autofree gchar*html = NULL;

    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_get_contents("../tests/data/ntis-list.html", &html, NULL, &error));
    g_autoptr(GPtrArray) notices = bicmag_ntis_parse_notices(html,
                                                             "https://www.ntis.go.kr/rndgate/eg/un/ra/mng.do",
                                                             &error);
    g_assert_no_error(error);
    g_assert_cmpuint(notices->len, ==, 2);
    g_assert_cmpstr(((BicMagNotice*)notices->pdata[0])->id, ==, "77109");
    g_assert_cmpstr(((BicMagNotice*)notices->pdata[0])->ministry, ==, "국방부");
    g_assert_cmpstr(((BicMagNotice*)notices->pdata[1])->title, ==,
                    "핵심인재 & 글로벌 브릿지");
    g_assert_true(g_str_has_prefix(((BicMagNotice*)notices->pdata[0])->detail_url,
                                   "https://www.ntis.go.kr/"));
}

static void
test_ntis_attachment_parser(void)
{
    g_autofree gchar*html = NULL;

    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_get_contents("../tests/data/ntis-detail.html", &html, NULL, &error));
    g_autoptr(GPtrArray) files = bicmag_ntis_parse_attachments(html,
                                                               "https://www.ntis.go.kr/rndgate/eg/un/ra/view.do",
                                                               &error);
    g_assert_no_error(error);
    g_assert_cmpuint(files->len, ==, 2);
    g_assert_cmpstr(((BicMagAttachment*)files->pdata[0])->id, ==, "1411335");
    g_assert_cmpstr(((BicMagAttachment*)files->pdata[0])->notice_id, ==,
                    "20260914100059666JKJ57LNH22");
    g_assert_cmpstr(((BicMagAttachment*)files->pdata[0])->name, ==, "공고문.pdf");
    g_assert_true(g_str_has_prefix(((BicMagAttachment*)files->pdata[0])->download_url,
                                   "https://www.ntis.go.kr/"));
}

static void
test_ntis_download_path_safety(void)
{
    g_autoptr(BicMagNtis) client = bicmag_ntis_new();
    g_autoptr(GError) error = NULL;
    g_autofree gchar*saved = NULL;
    g_assert_false(bicmag_ntis_download_file(client, "https://example.invalid/file",
                                             g_get_tmp_dir(), "../escape.pdf",
                                             &saved, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null(saved);
}

static void
test_ntis_digest_arguments(void)
{
    g_autoptr(BicMagNtis) client = bicmag_ntis_new();
    g_autoptr(GError) error = NULL;
    g_autofree gchar*saved = NULL;
    g_autofree gchar*digest = NULL;
    guint64 bytes = 0;
    g_assert_false(bicmag_ntis_download_file_with_digest(client,
                                                         "https://example.invalid/file",
                                                         g_get_tmp_dir(), "../x.pdf",
                                                         &saved, &bytes, &digest, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null(saved); g_assert_null(digest); g_assert_cmpuint(bytes, ==, 0);
}

static void
test_ntis_download_fixture(void)
{
    TestServer t = {0}; g_mutex_init(&t.mutex); g_cond_init(&t.cond);
    GThread*thread = g_thread_new("ntis-fixture", test_server_thread, &t);

    g_mutex_lock(&t.mutex);
    while (t.uri == NULL){ g_cond_wait(&t.cond, &t.mutex);} gchar*uri = g_strdup(t.uri);
    g_mutex_unlock(&t.mutex);
    g_autoptr(BicMagNtis) client = bicmag_ntis_new();
    g_autofree gchar*dir = g_dir_make_tmp("bicmag-test-XXXXXX", NULL);
    g_autofree gchar*saved = NULL; g_autofree gchar*digest = NULL; guint64 bytes = 0;
    g_autoptr(GError) error = NULL;
    gboolean ok = bicmag_ntis_download_file_with_digest(client, uri, dir, "fixture.bin", &saved,
                                                        &bytes, &digest, &error);
    g_test_message("download error: %s", error ? error->message : "none"); g_assert_true(ok);
    g_assert_no_error(error); g_assert_cmpuint(bytes, ==, strlen("fixture-download"));
    g_assert_cmpstr(digest, ==, "3d159e6b507feaf3432d6f40beb51808d5e7645078760aa74761a69720905cbc");
    g_autofree gchar*error_uri = g_strdup_printf("%s/error", uri);
    g_clear_error(&error);
    g_assert_false(bicmag_ntis_download_file(client, error_uri, dir, "error.bin", NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_main_loop_quit(t.loop); g_thread_join(thread); g_clear_object(&t.server);
    g_clear_pointer(&t.loop, g_main_loop_unref); g_free(t.uri); g_free(uri); g_remove(saved);
    g_rmdir(dir);
}

int
main(int argc, char**argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/pdf/invalid-arguments", test_invalid_arguments);
    g_test_add_func("/pdf/missing-file", test_missing_file);
    g_test_add_func("/pdf/corrupt-file", test_corrupt_file);
    g_test_add_func("/pdf/text-extraction", test_text_extraction);
    g_test_add_func("/pdf/signature", test_pdf_signature);
    g_test_add_func("/notice/date-rules", test_notice_date_rules);
    g_test_add_func("/cache/local-search", test_local_notice_cache);
    g_test_add_func("/ntis/client", test_ntis_client);
    g_test_add_func("/ntis/list-parser", test_ntis_list_parser);
    g_test_add_func("/ntis/attachment-parser", test_ntis_attachment_parser);
    g_test_add_func("/ntis/download-path-safety", test_ntis_download_path_safety);
    g_test_add_func("/ntis/digest-arguments", test_ntis_digest_arguments);
    g_test_add_func("/ntis/download-fixture", test_ntis_download_fixture);
    return g_test_run();
}
