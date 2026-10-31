#include "forecast_protocol.h"
#include "forecast.h"
#include "storage_forecast.h"
#include "file_store.h"
#include "clock_service.h"
#include "display.h"
#include "FreeRTOS.h"
#include <stdio.h>
#include <string.h>

#define FORECAST_COLLECTION_TICKS ((uint32_t)configTICK_RATE_HZ * 60UL)
#define FORECAST_LINE_MAX 127U
#define FORECAST_TEXT_SIZE 192U
#define FORECAST_RESULT_TIMEOUT (-1100)

/* Session scratch belongs to the command task, not its small task stack. */
static forecast_protocol_send_t s_send;
static forecast_batch_t s_candidate;
static forecast_batch_t s_saved;
static clock_view_model_t s_clock;
static char s_text[FORECAST_TEXT_SIZE];
static uint32_t s_activity;
static uint32_t s_token;
static uint32_t s_clear_generation;
static uint32_t s_last_id;
static int32_t s_last_result;
static uint8_t s_collecting;
static uint8_t s_received;
static uint8_t s_pending;
static uint8_t s_time_ready;

static void s_reply(const char *p_text)
{
    if (s_send != NULL) {
        (void)s_send((const uint8_t *)p_text, (uint16_t)strlen(p_text));
    }
}

static void s_ack(const char *p_operation, uint32_t id)
{
    (void)snprintf(s_text, sizeof(s_text), "OK FORECAST %s %lu\r\n",
                   p_operation, (unsigned long)id);
    s_reply(s_text);
}

static uint8_t s_unsigned(const char *p_text, uint32_t *p_value)
{
    uint32_t value = 0U;
    uint32_t digit;
    if (p_text == NULL || *p_text == '\0') { return 0U; }
    while (*p_text != '\0') {
        if (*p_text < '0' || *p_text > '9') { return 0U; }
        digit = (uint32_t)(*p_text++ - '0');
        if (value > (0xFFFFFFFFUL - digit) / 10U) { return 0U; }
        value = value * 10U + digit;
    }
    *p_value = value;
    return 1U;
}

static uint8_t s_temperature(const char *p_text, int16_t *p_value)
{
    uint8_t negative = (uint8_t)(*p_text == '-');
    uint32_t magnitude;
    if (!s_unsigned(p_text + negative, &magnitude) || magnitude > 999U) {
        return 0U;
    }
    *p_value = (int16_t)(negative != 0U ? -(int32_t)magnitude : (int32_t)magnitude);
    return 1U;
}

static uint8_t s_date(const char *p_text, clock_calendar_t *p_date)
{
    uint8_t i;
    if (strlen(p_text) != 10U || p_text[4] != '-' || p_text[7] != '-') { return 0U; }
    for (i = 0U; i < 10U; i++) {
        if (i != 4U && i != 7U && (p_text[i] < '0' || p_text[i] > '9')) { return 0U; }
    }
    memset(p_date, 0, sizeof(*p_date));
    p_date->year = (uint16_t)((p_text[0]-'0')*1000 + (p_text[1]-'0')*100 +
                              (p_text[2]-'0')*10 + p_text[3]-'0');
    p_date->month = (uint8_t)((p_text[5]-'0')*10 + p_text[6]-'0');
    p_date->day = (uint8_t)((p_text[8]-'0')*10 + p_text[9]-'0');
    p_date->weekday = clock_calendar_weekday(p_date->year, p_date->month, p_date->day);
    return clock_calendar_valid(p_date);
}

static uint32_t s_date_key(const clock_calendar_t *p_date)
{
    return (uint32_t)p_date->year * 10000UL + (uint32_t)p_date->month * 100UL + p_date->day;
}

static uint8_t s_fields(char *p_text, char **p_fields, uint8_t count)
{
    uint8_t i;
    char *p_comma;
    for (i = 0U; i < count; i++) {
        p_fields[i] = p_text;
        p_comma = strchr(p_text, ',');
        if ((i + 1U < count) != (p_comma != NULL)) { return 0U; }
        if (p_comma != NULL) { *p_comma = '\0'; p_text = p_comma + 1; }
        if (*p_fields[i] == '\0') { return 0U; }
    }
    return 1U;
}

static void s_drop_collection(void)
{
    s_collecting = 0U;
    s_received = 0U;
}

static void s_status(void)
{
    storage_forecast_snapshot_t snapshot;
    uint8_t present = (uint8_t)forecast_get_batch(&s_saved);
    storage_forecast_get_snapshot(&snapshot);
    if (present != 0U) {
        (void)snprintf(s_text, sizeof(s_text),
            "FORECAST SAVED %lu CITY %s COUNT %u FIRST %04u-%02u-%02u LAST %04u-%02u-%02u\r\n",
            (unsigned long)s_saved.id,
            s_saved.city == WEATHER_CITY_SHENZHEN ? "\xE6\xB7\xB1\xE5\x9C\xB3" : "\xE6\xB9\x9B\xE6\xB1\x9F",
            (unsigned int)s_saved.count, (unsigned int)s_saved.days[0].date.year,
            (unsigned int)s_saved.days[0].date.month, (unsigned int)s_saved.days[0].date.day,
            (unsigned int)s_saved.days[s_saved.count-1U].date.year,
            (unsigned int)s_saved.days[s_saved.count-1U].date.month,
            (unsigned int)s_saved.days[s_saved.count-1U].date.day);
        s_reply(s_text);
    } else { s_reply("FORECAST SAVED 0\r\n"); }
    (void)snprintf(s_text, sizeof(s_text),
        "FORECAST STATE %s ID %lu RECEIVED %u TIME %u READY %u RESULT %ld LAST %lu %ld\r\n",
        s_pending != 0U ? "SAVING" : (s_collecting != 0U ? "COLLECTING" : "IDLE"),
        (unsigned long)((s_pending != 0U || s_collecting != 0U) ? s_candidate.id : 0U),
        (unsigned int)s_received, (unsigned int)s_time_ready, (unsigned int)snapshot.ready,
        (long)snapshot.result, (unsigned long)s_last_id, (long)s_last_result);
    s_reply(s_text);
}

void forecast_protocol_init(forecast_protocol_send_t send)
{
    storage_forecast_snapshot_t snapshot;
    s_send = send;
    s_token = 0U;
    s_pending = 0U;
    s_time_ready = 0U;
    s_last_id = 0U;
    s_last_result = 0;
    s_drop_collection();
    storage_forecast_get_snapshot(&snapshot);
    s_clear_generation = snapshot.clear_generation;
}

void forecast_protocol_time_result(uint8_t success)
{
    s_time_ready = (uint8_t)(success != 0U);
}

void forecast_protocol_reset_session(void)
{
    s_time_ready = 0U;
    if (s_pending == 0U) { s_drop_collection(); }
}

uint8_t forecast_protocol_busy(void)
{
    return (uint8_t)(s_collecting != 0U || s_pending != 0U);
}

void forecast_protocol_poll(uint32_t now_tick)
{
    storage_forecast_snapshot_t snapshot;
    int32_t result;
    storage_forecast_get_snapshot(&snapshot);
    if (snapshot.clear_generation != s_clear_generation) {
        s_clear_generation = snapshot.clear_generation;
        forecast_protocol_reset_session();
    }
    if (s_pending != 0U && storage_forecast_take_result(s_token, &result) != 0U) {
        s_pending = 0U;
        s_last_id = s_candidate.id;
        s_last_result = result;
        if (result == FILE_STORE_OK) {
            display_forecast_committed();
            s_ack("COMMIT", s_candidate.id);
            s_drop_collection();
        } else {
            /* Accepted IO is never abandoned. A failed batch may retry COMMIT
             * without another TIME; a different batch still needs one. */
            s_activity = now_tick;
            (void)snprintf(s_text, sizeof(s_text), "ERR FORECAST STORAGE %lu %ld\r\n",
                (unsigned long)s_candidate.id, (long)result);
            s_reply(s_text);
        }
    }
    if (s_pending == 0U && s_collecting != 0U &&
        (uint32_t)(now_tick - s_activity) >= FORECAST_COLLECTION_TICKS) {
        s_last_id = s_candidate.id;
        s_last_result = FORECAST_RESULT_TIMEOUT;
        s_drop_collection();
        (void)snprintf(s_text, sizeof(s_text), "ERR FORECAST TIMEOUT %lu\r\n",
                       (unsigned long)s_last_id);
        s_reply(s_text);
    }
}

uint8_t forecast_protocol_command(const char *p_line, uint32_t now_tick)
{
    char copy[FORECAST_LINE_MAX + 1U];
    char *fields[5];
    forecast_day_t day;
    uint32_t id;
    uint32_t number;
    uint8_t i;
    uint8_t j;
    uint8_t present;
    weather_city_t city;
    int32_t result;
    storage_forecast_snapshot_t snapshot;
    if (p_line == NULL || strncmp(p_line, "FORECAST", 8U) != 0 ||
        (p_line[8] != '\0' && p_line[8] != ' ')) { return 0U; }
    forecast_protocol_poll(now_tick);
    if (strlen(p_line) > FORECAST_LINE_MAX) { s_reply("ERR FORECAST ARGUMENT\r\n"); return 1U; }
    if (strcmp(p_line, "FORECAST STATUS") == 0) { s_status(); return 1U; }
    present = (uint8_t)forecast_get_batch(&s_saved);
    if (strncmp(p_line, "FORECAST BEGIN ", 15U) == 0) {
        strcpy(copy, p_line + 15U);
        if (!s_fields(copy, fields, 3U) || !s_unsigned(fields[0], &id) || id == 0U ||
            !s_unsigned(fields[2], &number) || number == 0U || number > FORECAST_MAX_DAYS) {
            s_reply("ERR FORECAST ARGUMENT\r\n"); return 1U;
        }
        if (strcmp(fields[1], "\xE6\xB7\xB1\xE5\x9C\xB3") == 0) { city = WEATHER_CITY_SHENZHEN; }
        else if (strcmp(fields[1], "\xE6\xB9\x9B\xE6\xB1\x9F") == 0) { city = WEATHER_CITY_ZHANJIANG; }
        else { s_reply("ERR FORECAST ARGUMENT\r\n"); return 1U; }
        if (s_pending != 0U) { s_reply("ERR BUSY\r\n"); return 1U; }
        if (s_collecting != 0U) {
            if (s_candidate.id != id || s_candidate.city != city || s_candidate.count != number) {
                s_reply("ERR BUSY\r\n"); return 1U;
            }
            s_activity = now_tick; s_ack("BEGIN", id); return 1U;
        }
        if (present != 0U && s_saved.id == id) {
            if (s_saved.city != city || s_saved.count != number) {
                s_reply("ERR FORECAST CONFLICT\r\n"); return 1U;
            }
        } else if (s_time_ready == 0U) { s_reply("ERR FORECAST TIME\r\n"); return 1U; }
        storage_forecast_get_snapshot(&snapshot);
        if (snapshot.ready == 0U || snapshot.busy != 0U) {
            s_reply("ERR BUSY\r\n"); return 1U;
        }
        memset(&s_candidate, 0, sizeof(s_candidate));
        s_candidate.id = id; s_candidate.city = city; s_candidate.count = (uint8_t)number;
        s_time_ready = 0U; s_collecting = 1U; s_received = 0U; s_activity = now_tick;
        s_ack("BEGIN", id); return 1U;
    }
    if (strncmp(p_line, "FORECAST DAY ", 13U) == 0) {
        strcpy(copy, p_line + 13U);
        memset(&day, 0, sizeof(day));
        if (!s_fields(copy, fields, 5U) || !s_unsigned(fields[0], &id) || id == 0U ||
            !s_date(fields[1], &day.date) || !s_unsigned(fields[2], &number) || number > 13U ||
            !s_temperature(fields[3], &day.minimum_tenths) ||
            !s_temperature(fields[4], &day.maximum_tenths) || day.minimum_tenths > day.maximum_tenths) {
            s_reply("ERR FORECAST ARGUMENT\r\n"); return 1U;
        }
        day.condition = (uint8_t)number;
        if (s_pending != 0U) { s_reply("ERR BUSY\r\n"); return 1U; }
        if (s_collecting == 0U || s_candidate.id != id) {
            s_reply("ERR FORECAST SEQUENCE\r\n"); return 1U;
        }
        for (i = 0U; i < s_received; i++) {
            if (s_date_key(&s_candidate.days[i].date) == s_date_key(&day.date)) {
                if (s_candidate.days[i].condition != day.condition ||
                    s_candidate.days[i].minimum_tenths != day.minimum_tenths ||
                    s_candidate.days[i].maximum_tenths != day.maximum_tenths) {
                    s_reply("ERR FORECAST CONFLICT\r\n"); return 1U;
                }
                s_activity = now_tick; s_ack("DAY", id); return 1U;
            }
        }
        if (s_received >= s_candidate.count) { s_reply("ERR FORECAST SEQUENCE\r\n"); return 1U; }
        for (i = 0U; i < s_received; i++) {
            if (s_date_key(&day.date) < s_date_key(&s_candidate.days[i].date)) { break; }
        }
        for (j = s_received; j > i; j--) { s_candidate.days[j] = s_candidate.days[j-1U]; }
        s_candidate.days[i] = day; s_received++; s_activity = now_tick;
        s_ack("DAY", id); return 1U;
    }
    if (strncmp(p_line, "FORECAST COMMIT ", 16U) == 0 ||
        strncmp(p_line, "FORECAST CANCEL ", 16U) == 0) {
        if (!s_unsigned(p_line + 16U, &id) || id == 0U) {
            s_reply("ERR FORECAST ARGUMENT\r\n"); return 1U;
        }
        if (s_pending != 0U) { s_reply("ERR BUSY\r\n"); return 1U; }
        if (strncmp(p_line, "FORECAST CANCEL ", 16U) == 0) {
            if (s_collecting == 0U || s_candidate.id != id) {
                s_reply("ERR FORECAST SEQUENCE\r\n"); return 1U;
            }
            s_drop_collection(); s_ack("CANCEL", id); return 1U;
        }
        if (s_collecting == 0U) {
            if (present != 0U && s_saved.id == id) { s_ack("COMMIT", id); }
            else { s_reply("ERR FORECAST SEQUENCE\r\n"); }
            return 1U;
        }
        if (s_candidate.id != id || s_received != s_candidate.count ||
            !forecast_validate(&s_candidate, NULL)) {
            s_reply("ERR FORECAST SEQUENCE\r\n"); return 1U;
        }
        if (present != 0U && s_saved.id == id) {
            if (!forecast_equal(&s_candidate, &s_saved)) { s_reply("ERR FORECAST CONFLICT\r\n"); return 1U; }
            s_drop_collection(); s_ack("COMMIT", id); return 1U;
        }
        clock_service_get_model(&s_clock);
        if (s_clock.calendar_state != CLOCK_DATA_READY ||
            !clock_calendar_valid(&s_clock.calendar)) { s_reply("ERR FORECAST TIME\r\n"); return 1U; }
        if (!forecast_validate(&s_candidate, &s_clock.calendar)) {
            s_reply("ERR FORECAST DATE\r\n"); return 1U;
        }
        s_token++; if (s_token == 0U) { s_token++; }
        result = storage_forecast_request(&s_candidate, s_token);
        if (result == FILE_STORE_OK) { s_pending = 1U; s_activity = now_tick; }
        else if (result == FILE_STORE_ERROR_BUSY) { s_reply("ERR BUSY\r\n"); }
        else {
            s_last_id = id; s_last_result = result;
            (void)snprintf(s_text, sizeof(s_text), "ERR FORECAST STORAGE %lu %ld\r\n",
                (unsigned long)id, (long)result);
            s_reply(s_text);
        }
        return 1U;
    }
    s_reply("ERR FORECAST ARGUMENT\r\n");
    return 1U;
}
