#include "time_protocol.h"
#include "clock_service.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TIME2_LINE_LENGTH 29U
#define TIME_REPLY_BYTES 128U

static time_protocol_send_t s_send;
static uint32_t s_next_token;
static uint32_t s_pending_token;
static uint32_t s_receive_tick;
static uint8_t s_pending_query;
static uint8_t s_sync_success;
static char s_reply[TIME_REPLY_BYTES];

static void s_send_text(const char *p_text)
{
    if (s_send != NULL) { (void)s_send((const uint8_t *)p_text, (uint16_t)strlen(p_text)); }
}

static uint16_t s_number(const char *p_text, uint8_t length)
{
    uint16_t value = 0U;
    uint8_t i;
    for (i = 0U; i < length; i++) { value = (uint16_t)(value * 10U + p_text[i] - '0'); }
    return value;
}

static uint8_t s_parse(const char *p_line, clock_timestamp_t *p_timestamp)
{
    size_t i;
    if (strlen(p_line) != TIME2_LINE_LENGTH || strncmp(p_line, "TIME2 ", 6U) != 0) {
        return 0U;
    }
    for (i = 6U; i < TIME2_LINE_LENGTH; i++) {
        if (i == 10U || i == 13U) { if (p_line[i] != '-') { return 0U; } }
        else if (i == 16U) { if (p_line[i] != 'T') { return 0U; } }
        else if (i == 19U || i == 22U) { if (p_line[i] != ':') { return 0U; } }
        else if (i == 25U) { if (p_line[i] != '.') { return 0U; } }
        else if (p_line[i] < '0' || p_line[i] > '9') { return 0U; }
    }
    memset(p_timestamp, 0, sizeof(*p_timestamp));
    p_timestamp->calendar.year = s_number(p_line + 6U, 4U);
    p_timestamp->calendar.month = (uint8_t)s_number(p_line + 11U, 2U);
    p_timestamp->calendar.day = (uint8_t)s_number(p_line + 14U, 2U);
    p_timestamp->calendar.hour = (uint8_t)s_number(p_line + 17U, 2U);
    p_timestamp->calendar.minute = (uint8_t)s_number(p_line + 20U, 2U);
    p_timestamp->calendar.second = (uint8_t)s_number(p_line + 23U, 2U);
    p_timestamp->calendar.weekday = clock_calendar_weekday(p_timestamp->calendar.year,
        p_timestamp->calendar.month, p_timestamp->calendar.day);
    p_timestamp->millisecond = s_number(p_line + 26U, 3U);
    return clock_calendar_valid(&p_timestamp->calendar);
}

void time_protocol_init(time_protocol_send_t send)
{
    s_send = send;
    s_next_token = 0U;
    s_pending_token = 0U;
    s_receive_tick = 0U;
    s_pending_query = 0U;
    s_sync_success = 0U;
}

uint8_t time_protocol_command(const char *p_line, uint32_t receive_tick_ms)
{
    clock_timestamp_t timestamp;
    uint8_t query;
    int32_t result;
    if (p_line == NULL) { return 0U; }
    query = (uint8_t)(strcmp(p_line, "TIME?") == 0);
    if (query == 0U && strncmp(p_line, "TIME2", 5U) != 0) { return 0U; }
    if (s_pending_token != 0U) { s_send_text("ERR BUSY\r\n"); return 1U; }
    s_sync_success = 0U;
    if (query == 0U && s_parse(p_line, &timestamp) == 0U) {
        s_send_text("ERR TIME2\r\n");
        return 1U;
    }
    s_next_token++;
    if (s_next_token == 0U) { s_next_token++; }
    result = query != 0U ? clock_service_request_time_query(s_next_token) :
        clock_service_request_timestamp(&timestamp, receive_tick_ms, s_next_token);
    if (result != 0) {
        s_send_text(result == -1 ? "ERR BUSY\r\n" :
            (result == -3 ? "ERR TIME2\r\n" : "ERR RTC\r\n"));
        return 1U;
    }
    s_pending_token = s_next_token;
    s_pending_query = query;
    s_receive_tick = receive_tick_ms;
    return 1U;
}

void time_protocol_poll(void)
{
    clock_timestamp_t timestamp;
    const clock_calendar_t *p_calendar;
    int32_t result;
    int32_t length;
    if (s_pending_token == 0U || clock_service_take_timestamp_result(s_pending_token,
        &result, &timestamp) == 0U) { return; }
    s_pending_token = 0U;
    if (result != 0) {
        s_send_text(result == -4 ? "ERR TIME2 TIMING\r\n" :
            (result == -5 ? "ERR TIME UNSYNC\r\n" : "ERR RTC\r\n"));
        return;
    }
    p_calendar = &timestamp.calendar;
    if (s_pending_query == 0U) { s_sync_success = 1U; }
    length = snprintf(s_reply, sizeof(s_reply),
        "OK %s %04u-%02u-%02uT%02u:%02u:%02u.%03u RX %lu SAMPLE %lu SPAN %u\r\n",
        s_pending_query != 0U ? "TIME?" : "TIME2",
        (unsigned int)p_calendar->year, (unsigned int)p_calendar->month,
        (unsigned int)p_calendar->day, (unsigned int)p_calendar->hour,
        (unsigned int)p_calendar->minute, (unsigned int)p_calendar->second,
        (unsigned int)timestamp.millisecond, (unsigned long)s_receive_tick,
        (unsigned long)timestamp.tick_ms, (unsigned int)timestamp.read_span_ms);
    if (length > 0 && (size_t)length < sizeof(s_reply)) { s_send_text(s_reply); }
}

uint8_t time_protocol_busy(void)
{
    return (uint8_t)(s_pending_token != 0U);
}

uint8_t time_protocol_take_sync_success(void)
{
    uint8_t success = s_sync_success;
    s_sync_success = 0U;
    return success;
}
