#include "time_protocol.h"
#include "clock_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char s_output[256];
static uint32_t s_token;
static uint32_t s_receive_tick;
static uint32_t s_requests;
static int32_t s_accept_result;
static int32_t s_complete_result;
static uint8_t s_done;
static uint8_t s_query;
static clock_timestamp_t s_timestamp;

static uint8_t s_send(const uint8_t *p_data, uint16_t length)
{
    assert(length < sizeof(s_output));
    memcpy(s_output, p_data, length);
    s_output[length] = '\0';
    return 1U;
}

int32_t clock_service_request_timestamp(const clock_timestamp_t *p_timestamp,
                                        uint32_t receive_tick_ms, uint32_t token)
{
    s_requests++;
    s_timestamp = *p_timestamp;
    s_receive_tick = receive_tick_ms;
    s_token = token;
    s_query = 0U;
    return s_accept_result;
}

int32_t clock_service_request_time_query(uint32_t token)
{
    s_requests++;
    s_token = token;
    s_query = 1U;
    return s_accept_result;
}

uint8_t clock_service_take_timestamp_result(uint32_t token, int32_t *p_result,
                                           clock_timestamp_t *p_timestamp)
{
    assert(token == s_token);
    if (s_done == 0U) { return 0U; }
    s_done = 0U;
    *p_result = s_complete_result;
    *p_timestamp = s_timestamp;
    return 1U;
}

int main(void)
{
    static const char *const s_invalid[] = {
        "TIME2", "TIME2 ", "TIME2 2026-02-29T12:00:00.000",
        "TIME2 2028-02-29T12:00:00", "TIME2 2028-02-29T12:00:00.00",
        "TIME2 2028-02-29T12:00:00.000Z", "TIME2 2028-02-29T12:00:00.000 ",
        "TIME2 2028-02-29T24:00:00.000", "TIME2 2028-02-29T12:00:60.000",
        "TIME2 2028-02-29 12:00:00.000", "TIME2 2028-02-29T12:00:00.0a0"
    };
    size_t i;
    time_protocol_init(s_send);
    assert(time_protocol_command(NULL, 0U) == 0U);
    assert(time_protocol_command("TIME 2026-11-02T00:00:00", 0U) == 0U);
    for (i = 0U; i < sizeof(s_invalid) / sizeof(s_invalid[0]); i++) {
        assert(time_protocol_command(s_invalid[i], 0U) == 1U);
        assert(strcmp(s_output, "ERR TIME2\r\n") == 0 && s_requests == 0U);
    }
    s_output[0] = '\0';
    assert(time_protocol_command("TIME2 2028-02-29T12:00:00.999", UINT32_MAX - 20U) == 1U);
    assert(time_protocol_busy() != 0U && s_requests == 1U && s_output[0] == '\0');
    assert(s_timestamp.calendar.weekday == 2U && s_timestamp.millisecond == 999U);
    assert(s_receive_tick == UINT32_MAX - 20U && s_token != 0U);
    time_protocol_poll();
    assert(s_output[0] == '\0');
    assert(time_protocol_command("TIME?", 22U) == 1U);
    assert(strcmp(s_output, "ERR BUSY\r\n") == 0 && s_requests == 1U);
    s_done = 1U;
    s_timestamp.tick_ms = 30U;
    s_timestamp.read_span_ms = 1U;
    time_protocol_poll();
    assert(strcmp(s_output,
        "OK TIME2 2028-02-29T12:00:00.999 RX 4294967275 SAMPLE 30 SPAN 1\r\n") == 0);
    assert(time_protocol_busy() == 0U && time_protocol_take_sync_success() == 1U);
    assert(time_protocol_take_sync_success() == 0U);
    assert(time_protocol_command("TIME?", 40U) == 1U && s_query == 1U);
    s_done = 1U;
    time_protocol_poll();
    assert(strncmp(s_output, "OK TIME? ", 9U) == 0);
    assert(time_protocol_take_sync_success() == 0U);
    assert(time_protocol_command("TIME2 2028-02-29T12:00:00.000", 0U) == 1U);
    s_done = 1U;
    s_complete_result = -4;
    time_protocol_poll();
    assert(strcmp(s_output, "ERR TIME2 TIMING\r\n") == 0);
    assert(time_protocol_take_sync_success() == 0U);
    assert(time_protocol_command("TIME?", 0U) == 1U);
    s_done = 1U;
    s_complete_result = -5;
    time_protocol_poll();
    assert(strcmp(s_output, "ERR TIME UNSYNC\r\n") == 0);
    assert(time_protocol_take_sync_success() == 0U);
    s_accept_result = -1;
    assert(time_protocol_command("TIME?", 0U) == 1U);
    assert(strcmp(s_output, "ERR BUSY\r\n") == 0 && time_protocol_busy() == 0U);
    s_accept_result = -2;
    assert(time_protocol_command("TIME?", 0U) == 1U);
    assert(strcmp(s_output, "ERR RTC\r\n") == 0);
    puts("PASS: TIME2 strict syntax, leap date, busy/async completion, timestamp response, query and timing failure");
    return 0;
}
