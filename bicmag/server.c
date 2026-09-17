/*
 * Copyright 2026 BicMag contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * BicMag의 실행 진입점이자 로컬 HTTP 기반 MCP 서버 구현.
 *
 * 실행 흐름:
 *   1. 작업 디렉터리의 SQLite 캐시를 열고 NTIS 공고/첨부파일을 수집한다.
 *   2. 후속 수집 타이머와 HTTP 요청 처리기를 GLib 메인 컨텍스트에 등록한다.
 *   3. IPv4 루프백 주소에서 POST /mcp 요청을 받아 로컬 캐시를 조회한다.
 *
 * 처리하는 JSON-RPC 메서드는 initialize, tools/list, tools/call이다.
 * 도구 호출은 캐시만 읽으며, NTIS 원격 요청은 별도의 수집 단계에서 수행한다.
 * 현재 구현은 단일 JSON 객체 요청과 JSON 응답을 처리하는 간단한 구조로,
 * 배치 요청, 스트리밍 응답, 세션 관리 및 알림 전용 처리를 구현하지 않는다.
 *
 * g_autofree와 g_autoptr 변수는 해당 스코프를 벗어날 때 자동 정리된다.
 * 반면 파서/배열에서 빌려 온 포인터는 소유자가 살아 있는 동안에만 사용하며,
 * 소유권을 받는 JSON 빌더에 넘길 때는 필요한 노드를 복사한다.
 */
#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include "bicmag/cache.h"
#include "bicmag/collector.h"

/*
 * 공고 마감일을 사람이 읽는 D-day 문자열로 바꾼다.
 * deadline_date는 보통 YYYY.MM.DD 형식이며 YYYY-MM-DD도 그대로 처리된다.
 * today는 호출자가 만든 기준 날짜의 자정이다. 입력 포인터는 소유하지 않는다.
 * 반환값은 새로 할당한 문자열이므로 호출자가 g_free()로 해제해야 한다.
 * 날짜가 없으면 "마감일 없음", 파싱할 수 없으면 "D-?"를 반환한다.
 */
static gchar *
bicmag_mcp_deadline_label(const gchar *deadline_date, GDateTime *today)
{
    g_autofree gchar *iso_date = NULL;
    g_autofree gchar *iso_datetime = NULL;

    g_autoptr(GDateTime) deadline = NULL;
    gint64 days;

    if (deadline_date == NULL || *deadline_date == '\0') {
        return g_strdup("마감일 없음");
    }
    /* 원본 공고 데이터는 보존하고 복사본의 구분자만 ISO 8601 형태로 바꾼다. */
    iso_date = g_strdup(deadline_date);
    for (gchar *cursor = iso_date; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            *cursor = '-';
        }
    }
    /* 마감 시각은 한국 표준시(UTC+09:00)의 자정으로 해석한다. */
    iso_datetime = g_strdup_printf("%sT00:00:00+09:00", iso_date);
    deadline = g_date_time_new_from_iso8601(iso_datetime, NULL);
    if (deadline == NULL) {
        return g_strdup("D-?");
    }
    /*
     * 마이크로초 단위 시간 차이를 24시간으로 나누며 소수 부분은 0 방향으로 버린다.
     * 오늘이면 D-0, 미래이면 D-N, 과거이면 D+N으로 표시한다.
     * 호출부의 today는 시스템 로컬 시간대이므로 한국 시간대가 아닌 환경에서는
     * 두 자정의 기준이 달라 달력상의 날짜 차이와 표시값이 다를 수 있다.
     */
    days = g_date_time_difference(deadline, today) / G_TIME_SPAN_DAY;
    return days >= 0 ? g_strdup_printf("D-%" G_GINT64_FORMAT, days) :
           g_strdup_printf("D+%" G_GINT64_FORMAT, -days);
}

/*
 * 수집 타이머에 함께 전달할 상태. 두 포인터의 소유자는 main()이다.
 * 이 구조체와 그 대상은 메인 루프가 실행되는 동안 계속 유효해야 한다.
 */
typedef struct {
    BicMagCache *cache;
    BicMagNtis *client;
} BicMagCollectorContext;

/*
 * 타이머 만료 시 NTIS를 동기 수집하고 다음 실행을 한 번 예약한다.
 * 메인 루프에서 직접 실행되므로 수집 중에는 같은 루프의 HTTP 처리가 지연된다.
 * 수집 실패는 경고로 남기고, 성공 여부와 관계없이 다음 수집을 예약한다.
 */
static gboolean
bicmag_collector_timeout(gpointer user_data)
{
    BicMagCollectorContext *context = user_data;

    g_autoptr(GError) error = NULL;
    /* 난수 상한은 제외되므로 대기 시간은 7,200~9,000초(2시간~2시간 30분)다. */
    guint delay = 7200 + g_random_int_range(0, 1801);

    if (!bicmag_collector_sync(context->cache, context->client,
        BICMAG_NTIS_LIST_URI,
        "bicmag-attachments", &error)) {
        g_warning("NTIS scheduled sync failed: %s", error->message);
    }
    /*
     * 수집이 끝난 시점부터 새 대기 시간을 적용한다. 기존 타이머는 아래 반환값으로
     * 제거하므로 반복 소스가 중복 유지되지 않고 매번 새로운 간격을 뽑는다.
     */
    g_timeout_add_seconds(delay, bicmag_collector_timeout, context);
    return G_SOURCE_REMOVE;
}

/*
 * JSON-RPC 오류 응답을 만든다:
 * {"jsonrpc":"2.0", "id":요청 식별자 또는 null,
 *  "error":{"code":오류 코드, "message":설명}}
 *
 * message와 id는 빌린 포인터다. id를 복사해 빌더에 넘기므로 원본은 유지된다.
 * 이 함수는 HTTP 상태를 항상 200으로 설정한다. 호출부에서 먼저 지정한 400/500도
 * 여기에서 덮어쓰며, 클라이언트는 JSON 본문의 error로 RPC 실패를 판별해야 한다.
 */
static void
bicmag_mcp_error(SoupServerMessage *message, JsonNode *id, gint code,
    const gchar *text)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    json_builder_set_member_name(builder, "id");
    json_builder_add_value(builder,
        id != NULL ? json_node_copy(id) : json_node_new(JSON_NODE_NULL));
    json_builder_set_member_name(builder, "error");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "code");
    json_builder_add_int_value(builder, code);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, text);
    json_builder_end_object(builder); json_builder_end_object(builder);
    /*
     * 빌더에서 얻은 루트는 호출자가 직렬화 후 json_node_free()로 해제한다.
     * 직렬화된 body는 SOUP_MEMORY_COPY로 응답에 복사되어 자동 해제해도 안전하다.
     */
    g_autoptr(JsonGenerator) generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gsize length = 0;
    g_autofree gchar *body = json_generator_to_data(generator, &length);
    json_node_free(root);
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "application/json",
        SOUP_MEMORY_COPY, body, length);
}

/*
 * libsoup가 호출하는 HTTP 요청 처리기. user_data는 main()의 공유 캐시다.
 * 경로/HTTP 메서드 확인 -> JSON 파싱 -> RPC 분기 -> JSON 직렬화 순으로 처리한다.
 * 캐시 조회는 동기 방식이며, 결과를 하나의 텍스트 콘텐츠로 묶어 돌려준다.
 */
static void
bicmag_mcp_handler(SoupServer *server, SoupServerMessage *message,
    const gchar *path, GHashTable *query, gpointer user_data)
{
    /* 콜백 시그니처에 필요한 인자 중 서버 객체와 URL 쿼리 문자열은 사용하지 않는다. */
    (void)server;
    (void)query;
    BicMagCache *cache = user_data;
    /* 기본 처리기로 등록되어 모든 경로가 들어올 수 있으므로 정확한 경로를 확인한다. */
    if (g_strcmp0(path, "/mcp") != 0) {
        soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
        return;
    }
    /* /mcp는 POST만 받으며, GET 등을 통한 조회나 스트리밍 연결은 제공하지 않는다. */
    if (g_strcmp0(soup_server_message_get_method(message),
        SOUP_METHOD_POST) != 0) {
        soup_server_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED,
            NULL);
        return;
    }

    /*
     * 요청 본문은 message 소유다. 길이를 명시해 JSON 파서에 전달한다.
     * 본문 부재, 파싱 실패, 객체가 아닌 최상위 값은 RPC 오류 본문 없이 HTTP 400이다.
     * 배열 형태의 JSON-RPC 배치 요청도 이 단계에서 거절한다.
     */
    SoupMessageBody *request = soup_server_message_get_request_body(message);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    if (request == NULL || !json_parser_load_from_data(
            parser, request->data, request->length, &error)) {
        soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, NULL);
        return;
    }
    JsonNode *request_root = json_parser_get_root(parser);
    if (request_root == NULL || !JSON_NODE_HOLDS_OBJECT(request_root)) {
        soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST, NULL);
        return;
    }
    /*
     * object, method, id는 파서의 트리에서 빌린 값이며 파서보다 오래 보관하지 않는다.
     * method 누락 시 빈 문자열로 분기한다. jsonrpc 버전이나 각 멤버의 자료형을
     * 여기에서 모두 검증하는 것은 아니며, 객체/문자열 접근자의 기대 형식을 전제한다.
     */
    JsonObject *object = json_node_get_object(request_root);
    const gchar *method = json_object_get_string_member_with_default(
        object, "method", "");
    JsonNode *id = json_object_get_member(object, "id");
    /*
     * 성공 응답의 공통 외피를 연다. 이후 메서드별 분기에서 result 값을 채운다.
     * id가 없는 요청도 알림으로 구분하지 않고 id:null인 응답을 생성한다.
     */
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    json_builder_set_member_name(builder, "id");
    json_builder_add_value(builder, id != NULL ? json_node_copy(id) :
        json_node_new(JSON_NODE_NULL));
    json_builder_set_member_name(builder, "result");
    /* 초기화 응답: 요청 버전과 협상하지 않고 아래 프로토콜/서버 버전을 반환한다. */
    if (g_strcmp0(method, "initialize") == 0) {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "protocolVersion");
        json_builder_add_string_value(builder, "2025-11-25");
        /* 현재 capabilities는 빈 객체이며 별도의 기능 플래그를 광고하지 않는다. */
        json_builder_set_member_name(builder, "capabilities");
        json_builder_begin_object(builder);
        json_builder_end_object(builder);
        json_builder_set_member_name(builder, "serverInfo");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, "BicMag");
        json_builder_set_member_name(builder, "version");
        json_builder_add_string_value(builder, "0.1.0");
        json_builder_end_object(builder);
        json_builder_end_object(builder);
    }
    /* 도구 목록은 정적으로 정의하며 inputSchema에 각 도구의 입력 형태를 기술한다. */
    else if (g_strcmp0(method, "tools/list") == 0) {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "tools");
        json_builder_begin_array(builder);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        /* ntis_search: 필수 문자열 query로 로컬 공고 전문 검색을 수행한다. */
        json_builder_add_string_value(builder, "ntis_search");
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder,
            "Search locally cached NTIS notices");
        json_builder_set_member_name(builder, "inputSchema");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "object");
        json_builder_set_member_name(builder, "properties");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "query");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "string");
        json_builder_end_object(builder);
        json_builder_end_object(builder);
        json_builder_set_member_name(builder, "required");
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "query");
        json_builder_end_array(builder); json_builder_end_object(builder);
        json_builder_end_object(builder);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        /* ntis_list: 필수 인자 없이 캐시에 저장된 조회 대상 공고를 나열한다. */
        json_builder_add_string_value(builder, "ntis_list");
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder,
            "List locally cached NTIS notices");
        json_builder_set_member_name(builder, "inputSchema");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "object");
        json_builder_end_object(builder);
        json_builder_end_object(builder);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        /* ntis_attachments: 필수 문자열 notice_id에 연결된 첨부파일 정보를 나열한다. */
        json_builder_add_string_value(builder, "ntis_attachments");
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder,
            "List locally cached NTIS attachments");
        json_builder_set_member_name(builder, "inputSchema");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "object");
        json_builder_set_member_name(builder, "properties");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "notice_id");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "string");
        json_builder_end_object(builder);
        json_builder_end_object(builder);
        json_builder_set_member_name(builder, "required");
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "notice_id");
        json_builder_end_array(builder);
        json_builder_end_object(builder);
        json_builder_end_object(builder);
        json_builder_end_array(builder);
        json_builder_end_object(builder);
    }else if (g_strcmp0(method, "tools/call") == 0) {
        /*
         * params.name으로 도구를 고르고 params.arguments에서 인자를 읽는다.
         * 인자가 없을 때는 아래 기본값을 사용하지만, 전달된 JSON 값의 자료형까지
         * 스키마로 자동 검증하지는 않는다. 문자열의 공백도 제거하지 않는다.
         */
        JsonObject *params = json_object_get_object_member(object, "params");
        const gchar *name =
            params ? json_object_get_string_member_with_default(params, "name",
                "") : "";
        JsonObject *arguments = params ? json_object_get_object_member(params,
                "arguments") : NULL;
        const gchar *query_text =
            arguments ? json_object_get_string_member_with_default(arguments,
                "query",
                "") : "";
        const gchar *notice_id = arguments ?
            json_object_get_string_member_with_default(arguments,
                "notice_id", "") :
            "";
        /*
         * 도구에 따라 두 결과 배열 중 하나만 채운다. 캐시 API의 배열은 요소 해제
         * 함수를 가지고 있으므로 배열을 정리할 때 공고/첨부파일 객체도 정리된다.
         */
        g_autoptr(GError) search_error = NULL;
        g_autoptr(GPtrArray) notices = NULL;
        g_autoptr(GPtrArray) attachments = NULL;
        if (g_strcmp0(name, "ntis_attachments") == 0 && cache != NULL &&
            *notice_id != '\0') {
            /* 메타데이터 조회만 수행하며 이 요청에서 파일을 다운로드하지 않는다. */
            attachments = bicmag_cache_list_attachments(cache, notice_id,
                    &search_error);
        }else if (g_strcmp0(name, "ntis_list") == 0) {
            /* 캐시 계층이 eligible 공고만 선택하고 마감일 등을 기준으로 정렬한다. */
            notices = bicmag_cache_list_notices(cache, &search_error);
        }else if (g_strcmp0(name, "ntis_search") == 0 && cache != NULL &&
            *query_text != '\0') {
            /* query는 캐시 계층의 SQLite FTS MATCH 검색식으로 전달된다. */
            notices = bicmag_cache_search_notices(cache, query_text,
                    &search_error);
        }else {
            /* 알 수 없는 도구 또는 빈 필수 인자는 RPC -32602로 응답한다. */
            json_builder_end_object(builder);
            soup_server_message_set_status(message, SOUP_STATUS_BAD_REQUEST,
                NULL);
            bicmag_mcp_error(message, id, -32602,
                "Invalid tools/call arguments");
            return;
        }
        /*
         * NULL 결과는 조회 실패이고 길이 0인 배열은 정상적인 빈 결과다.
         * 실패 시 RPC -32603과 가능한 경우 캐시 계층의 상세 오류를 반환한다.
         */
        if ((g_strcmp0(name,
            "ntis_attachments") == 0 ? attachments == NULL : notices == NULL)) {
            soup_server_message_set_status(message,
                SOUP_STATUS_INTERNAL_SERVER_ERROR,
                NULL);
            bicmag_mcp_error(message, id, -32603,
                search_error ? search_error->message :
                "Local search failed"); return;
        }
        /*
         * MCP 도구 결과 형태: {"content":[{"type":"text", "text":"..."}]}.
         * 행마다 탭으로 열을 구분해 하나의 문자열에 누적한다. 결과가 없더라도
         * text가 빈 문자열인 콘텐츠 항목 하나를 반환한다.
         */
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "content");
        json_builder_begin_array(builder); json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "text");
        json_builder_set_member_name(builder, "text");
        g_autoptr(GString) text = g_string_new(NULL);
        /* 요청 처리 시점의 로컬 날짜를 자정으로 맞춰 모든 공고에 같은 기준을 쓴다. */
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        g_autoptr(GDateTime) today =
            g_date_time_new_local(g_date_time_get_year(now),
                g_date_time_get_month(now),
                g_date_time_get_day_of_month(now),
                0, 0, 0);
        if (g_strcmp0(name, "ntis_attachments") == 0) {
            /*
             * 열 순서: 첨부파일 ID, 파일명, 로컬 경로(없으면 미다운로드), 다운로드 URL.
             * 로컬 경로는 캐시에 기록된 값이며 여기서 실제 파일 존재를 검사하지 않는다.
             */
            for (guint i = 0; i < attachments->len; i++) {
                BicMagAttachment *attachment = g_ptr_array_index(attachments,
                        i);
                g_string_append_printf(text, "%s\t%s\t%s\t%s\n", attachment->id,
                    attachment->name, attachment->local_path != NULL ?
                    attachment->local_path : "미다운로드",
                    attachment->download_url);
            }
        }else {
            /*
             * 열 순서: 공고 ID, 마감일(없으면 대체 문구), D-day, 제목.
             * notice는 배열에서 빌린 포인터이고 dday는 반복마다 자동 해제된다.
             */
            for (guint i = 0; i < notices->len;
                i++) {
                BicMagNotice *notice = g_ptr_array_index(notices, i);
                g_autofree gchar *dday =
                    bicmag_mcp_deadline_label(notice->deadline_date, today);
                g_string_append_printf(text, "%s\t%s\t%s\t%s\n", notice->id,
                    notice->deadline_date != NULL &&
                    *notice->deadline_date != '\0' ?
                    notice->deadline_date : "마감일 없음", dday,
                    notice->title);
            }
        }
        /* JSON 직렬화 시 탭/개행은 이스케이프된다. 열 값 자체의 탭은 따로 치환하지 않는다. */
        json_builder_add_string_value(builder, text->str);
        json_builder_end_object(builder);
        json_builder_end_array(builder); json_builder_end_object(builder);
    }else {
        json_builder_end_object(builder);
        /* 지원하지 않는 메서드(빈 이름 포함)는 JSON-RPC의 메서드 없음 오류다. */
        bicmag_mcp_error(message, id, -32601, "Method not found");
        return;
    }
    /* 메서드별 result와 공통 외피가 완성되면 JSON 바이트열로 직렬화한다. */
    json_builder_end_object(builder);
    g_autoptr(JsonGenerator) generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gsize length = 0;
    g_autofree gchar *body = json_generator_to_data(generator, &length);
    json_node_free(root);
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    /* 응답이 body를 복사하므로 함수 종료 시 지역 버퍼를 자동 해제할 수 있다. */
    soup_server_message_set_response(message, "application/json",
        SOUP_MEMORY_COPY,
        body, length);
}

/*
 * 사용 예: bicmag 8080. 첫 번째 인자는 10진수 포트이며 생략 시 0을 사용한다.
 * 포트 0은 운영체제가 사용 가능한 포트를 선택하게 한다. 현재 인자의 변환 종료
 * 위치/범위를 별도로 검사하지 않으므로 이 코드는 엄격한 포트 검증기가 아니다.
 * 초기화 또는 리스닝 실패 시 1, 메인 루프가 정상 반환하면 0을 반환한다.
 */
int
main(int argc, char **argv)
{
    guint port = argc > 1 ? (guint)g_ascii_strtoull(argv[1], NULL, 10) : 0;

    g_autoptr(GError) error = NULL;
    g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
    /* 상대 경로는 실행 시 작업 디렉터리 기준이며 캐시 열기 실패는 시작 불가다. */
    g_autoptr(BicMagCache) cache = bicmag_cache_open("bicmag.db", &error);
    if (cache == NULL) {
        g_printerr("bicmag: %s\n", error->message); return 1;
    }
    g_autoptr(BicMagNtis) client = bicmag_ntis_new();
    /*
     * HTTP 리스닝 전에 최초 수집을 완료한다. 수집에는 원격 공고 조회와 대상 공고의
     * 첨부파일 다운로드가 포함되므로 시작까지 시간이 걸릴 수 있다.
     * 실패해도 로그만 출력하고 기존 캐시로 서버를 시작한다.
     */
    if (!bicmag_collector_sync(cache, client, BICMAG_NTIS_LIST_URI,
        "bicmag-attachments", &error)) {
        g_printerr("bicmag: NTIS sync failed: %s\n", error->message);
    }
    /*
     * 첫 주기 수집도 2시간~2시간 30분 뒤에 실행한다.
     * 타이머는 스택의 collector_context 주소를 보관한다. main()이 루프 실행 동안
     * 반환하지 않으므로 컨텍스트와 자동 정리 대상 cache/client는 계속 유효하다.
     */
    BicMagCollectorContext collector_context = { cache, client };
    g_timeout_add_seconds(7200 + g_random_int_range(0, 1801),
        bicmag_collector_timeout,
        &collector_context);
    /* NULL 경로는 기본 처리기를 뜻한다. 캐시는 빌려 주며 별도 해제 콜백은 없다. */
    soup_server_add_handler(server, NULL, bicmag_mcp_handler, cache, NULL);
    /* IPv4 루프백에만 바인딩하므로 외부 네트워크 인터페이스에는 직접 공개하지 않는다. */
    if (!soup_server_listen_local(server, port, SOUP_SERVER_LISTEN_IPV4_ONLY,
        &error)) {
        g_printerr("bicmag: %s\n", error->message);
        return 1;
    }
    /*
     * 실제 리스닝 URI로 접속 주소를 출력한다. 특히 포트 0을 사용했을 때 필요하다.
     * URI 끝의 슬래시를 제거한 뒤 /mcp를 붙여 슬래시 중복을 피한다.
     * 반환된 목록과 GUri 참조는 호출자가 정리할 대상이지만 현재 코드에는 해제가 없다.
     */
    GSList *uris = soup_server_get_uris(server);
    g_autofree gchar *endpoint = uris !=
        NULL ? g_uri_to_string(uris->data) : NULL;
    if (endpoint != NULL && g_str_has_suffix(endpoint, "/")){
        endpoint[strlen(endpoint) - 1] = '\0';
    }
    g_print("bicmag MCP listening on %s/mcp\n",
        endpoint != NULL ? endpoint : "http://127.0.0.1:0");
    /*
     * 기본 메인 컨텍스트에서 HTTP 이벤트와 수집 타이머를 함께 처리한다.
     * 이 파일에는 g_main_loop_quit() 호출이 없어 보통 프로세스 종료까지 머문다.
     * 루프가 반환하면 main()의 자동 정리 변수가 스코프를 벗어나며 해제된다.
     */
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
