#include "bicmag/notice.h"

GQuark
bicmag_notice_error_quark(void)
{
    return g_quark_from_static_string("bicmag-notice-error-quark");
}

static GDateTime*
bicmag_notice_parse_date(const gchar*value)
{
    g_autofree gchar*normalized = NULL;
    gchar**parts;
    gint year, month, day;

    if (value == NULL || *value == '\0'){
        return NULL;
    }

    normalized = g_strdup(value);
    for (gchar*cursor = normalized; *cursor != '\0'; ++cursor) {
        if (*cursor == '.'){
            *cursor = '-';
        }
    }
    if (strlen(normalized) != 10 || normalized[4] != '-' ||
        normalized[7] != '-'){
        return NULL;
    }
    parts = g_strsplit(normalized, "-", -1);
    if (g_strv_length(parts) != 3) {
        g_strfreev(parts);
        return NULL;
    }
    for (guint i = 0; i < 3; ++i) {
        for (const gchar*cursor = parts[i]; *cursor != '\0'; ++cursor) {
            if (!g_ascii_isdigit(*cursor)) {
                g_strfreev(parts);
                return NULL;
            }
        }
    }
    year = (gint)g_ascii_strtoll(parts[0], NULL, 10);
    month = (gint)g_ascii_strtoll(parts[1], NULL, 10);
    day = (gint)g_ascii_strtoll(parts[2], NULL, 10);
    g_strfreev(parts);
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31){
        return NULL;
    }
    return g_date_time_new_local(year, month, day, 0, 0, 0);
}

BicMagNotice*
bicmag_notice_new(void)
{
    return g_new0(BicMagNotice, 1);
}

BicMagNotice*
bicmag_notice_copy(const BicMagNotice*notice)
{
    g_autoptr(BicMagNotice) copy = NULL;

    g_return_val_if_fail(notice != NULL, NULL);
    copy = bicmag_notice_new();
    copy->id = g_strdup(notice->id);
    copy->title = g_strdup(notice->title);
    copy->ministry = g_strdup(notice->ministry);
    copy->receipt_date = g_strdup(notice->receipt_date);
    copy->deadline_date = g_strdup(notice->deadline_date);
    copy->status = g_strdup(notice->status);
    copy->detail_url = g_strdup(notice->detail_url);
    return g_steal_pointer(&copy);
}

void
bicmag_notice_free(BicMagNotice*notice)
{
    if (notice == NULL){
        return;
    }
    g_free(notice->id);
    g_free(notice->title);
    g_free(notice->ministry);
    g_free(notice->receipt_date);
    g_free(notice->deadline_date);
    g_free(notice->status);
    g_free(notice->detail_url);
    g_free(notice);
}

gboolean
bicmag_notice_is_eligible(const gchar*receipt_date,
                          const gchar*deadline_date,
                          const gchar*status,
                          GDateTime*now,
                          GError**error)
{
    g_autoptr(GDateTime) receipt = NULL;
    g_autoptr(GDateTime) deadline = NULL;
    g_autoptr(GDateTime) today = NULL;

    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (now == NULL) {
        g_set_error(error, BICMAG_NOTICE_ERROR, 1, "now is required");
        return FALSE;
    }
    receipt = bicmag_notice_parse_date(receipt_date);
    deadline = bicmag_notice_parse_date(deadline_date);
    today = g_date_time_new_local(g_date_time_get_year(now),
                                  g_date_time_get_month(now),
                                  g_date_time_get_day_of_month(now),
                                  0, 0, 0);
    if (deadline_date != NULL && *deadline_date != '\0' && deadline == NULL) {
        g_set_error(error, BICMAG_NOTICE_ERROR, 2,
                    "invalid deadline date: %s", deadline_date);
        return FALSE;
    }
    if (receipt_date != NULL && *receipt_date != '\0' && receipt == NULL) {
        g_set_error(error, BICMAG_NOTICE_ERROR, 3,
                    "invalid receipt date: %s", receipt_date);
        return FALSE;
    }
    if (deadline != NULL){
        return g_date_time_compare(today, deadline) <= 0;
    }
    if (receipt == NULL || status == NULL){
        return FALSE;
    }
    if (g_strcmp0(status, "접수예정") == 0){
        return g_date_time_compare(receipt, today) >= 0;
    }
    if (g_strcmp0(status, "접수중") == 0){
        return g_date_time_compare(receipt, today) <= 0;
    }
    return FALSE;
}
