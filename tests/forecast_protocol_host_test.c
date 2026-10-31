/* Transport-independent forecast transactions. Flash completion is controlled
 * explicitly so acceptance cannot accidentally masquerade as durability. */
#include "forecast_protocol.h"
#include "forecast.h"
#include "storage_forecast.h"
#include "file_store.h"
#include "clock_service.h"
#include "display.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MinGW's default assert opens a modal dialog on Windows. Fail in the host
 * process instead so automation always receives the exact failed check. */
#undef assert
#define assert(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
    exit(EXIT_FAILURE); } } while (0)

#define CITY_SZ "\xE6\xB7\xB1\xE5\x9C\xB3"
#define CITY_ZJ "\xE6\xB9\x9B\xE6\xB1\x9F"

static char s_output[4096];
static size_t s_output_length;
static uint8_t s_fail_send;
static uint32_t s_tick;
static uint32_t s_requests;
static uint32_t s_page_changes;
static uint32_t s_pending_token;
static uint8_t s_complete;
static int32_t s_accept_result;
static int32_t s_completion_result;
static forecast_batch_t s_pending;
static clock_view_model_t s_clock;
static storage_forecast_snapshot_t s_storage;

static uint8_t send_text(const uint8_t *p_data, uint16_t length)
{
    if (s_fail_send != 0U) { s_fail_send = 0U; return 0U; }
    assert(s_output_length + length < sizeof(s_output));
    memcpy(s_output + s_output_length, p_data, length);
    s_output_length += length;
    s_output[s_output_length] = '\0';
    return 1U;
}

void clock_service_get_model(clock_view_model_t *p_model) { *p_model = s_clock; }
void display_forecast_committed(void) { s_page_changes++; }
void storage_forecast_get_snapshot(storage_forecast_snapshot_t *p_snapshot)
{ *p_snapshot = s_storage; }
int32_t storage_forecast_request(const forecast_batch_t *p_batch, uint32_t token)
{
    assert(p_batch != NULL && token != 0U);
    assert(forecast_validate(p_batch, NULL));
    if (s_accept_result != 0) { return s_accept_result; }
    assert(s_pending_token == 0U);
    s_pending = *p_batch;
    s_pending_token = token;
    s_storage.busy = 1U;
    s_requests++;
    return FILE_STORE_OK;
}
uint8_t storage_forecast_take_result(uint32_t token, int32_t *p_result)
{
    assert(token != 0U && p_result != NULL);
    assert(s_pending_token == token);
    if (s_complete == 0U) { return 0U; }
    if (s_completion_result == 0) { assert(forecast_publish(&s_pending)); }
    *p_result = s_completion_result;
    s_storage.result = s_completion_result;
    s_storage.busy = 0U;
    s_pending_token = 0U;
    s_complete = 0U;
    return 1U;
}

static void clear_output(void) { s_output_length = 0U; s_output[0] = '\0'; }
static void command(const char *p_line)
{
    clear_output();
    assert(forecast_protocol_command(p_line, s_tick) != 0U);
}
static void expect(const char *p_text)
{
    if (strstr(s_output, p_text) == NULL) {
        fprintf(stderr, "Expected [%s], got [%s]\n", p_text, s_output);
        assert(0);
    }
}
static void reset(void)
{
    assert(s_pending_token == 0U);
    forecast_init();
    memset(&s_storage, 0, sizeof(s_storage));
    s_storage.ready = 1U;
    forecast_protocol_init(send_text);
    memset(&s_clock, 0, sizeof(s_clock));
    s_clock.calendar_state = CLOCK_DATA_READY;
    s_clock.calendar.year = 2026U;
    s_clock.calendar.month = 10U;
    s_clock.calendar.day = 29U;
    s_clock.calendar.weekday = 4U;
    s_tick = 100U;
    s_requests = 0U;
    s_page_changes = 0U;
    s_complete = 0U;
    s_accept_result = 0;
    s_completion_result = 0;
    s_fail_send = 0U;
    clear_output();
}
static void begin(uint32_t id, uint8_t count)
{
    char text[80];
    (void)snprintf(text, sizeof(text), "FORECAST BEGIN %lu," CITY_SZ ",%u",
        (unsigned long)id, (unsigned int)count);
    command(text);
}
static void day(uint32_t id, const char *p_date, uint8_t condition)
{
    char text[96];
    (void)snprintf(text, sizeof(text), "FORECAST DAY %lu,%s,%u,180,270",
        (unsigned long)id, p_date, (unsigned int)condition);
    command(text);
}
static void finish(int32_t result)
{
    s_completion_result = result;
    s_complete = 1U;
    clear_output();
    forecast_protocol_poll(s_tick);
}

static void test_time_gate_and_atomic_publish(void)
{
    forecast_batch_t stored;
    reset();
    assert(forecast_protocol_command("WEATHER unchanged", s_tick) == 0U);
    begin(42U, 2U); expect("ERR FORECAST TIME");
    forecast_protocol_time_result(0U);
    begin(42U, 2U); expect("ERR FORECAST TIME");
    forecast_protocol_time_result(1U);
    begin(42U, 2U); expect("OK FORECAST BEGIN 42");
    assert(forecast_protocol_busy());
    day(42U, "2026-10-30", 3U); expect("OK FORECAST DAY 42");
    day(42U, "2026-10-29", 2U); expect("OK FORECAST DAY 42");
    command("FORECAST COMMIT 42");
    assert(s_requests == 1U && s_page_changes == 0U);
    assert(strstr(s_output, "OK FORECAST COMMIT") == NULL);
    assert(!forecast_get_batch(&stored));
    forecast_protocol_poll(s_tick);
    assert(s_page_changes == 0U && !forecast_get_batch(&stored));
    begin(43U, 1U); expect("ERR BUSY");
    command("FORECAST CANCEL 42"); expect("ERR BUSY");
    finish(0); expect("OK FORECAST COMMIT 42");
    assert(!forecast_protocol_busy() && s_page_changes == 1U);
    assert(forecast_get_batch(&stored) && stored.id == 42U && stored.count == 2U);
    assert(stored.days[0].date.day == 29U && stored.days[1].date.day == 30U);
    begin(43U, 1U); expect("ERR FORECAST TIME");
}

static void test_duplicate_commit_and_lost_ack(void)
{
    uint32_t requests;
    reset();
    forecast_protocol_time_result(1U);
    begin(50U, 1U);
    day(50U, "2026-10-29", 2U);
    command("FORECAST COMMIT 50");
    s_fail_send = 1U;
    finish(0);
    assert(s_output_length == 0U && s_page_changes == 1U);
    requests = s_requests;
    command("FORECAST STATUS"); expect("FORECAST SAVED"); expect("50");
    command("FORECAST COMMIT 50"); expect("OK FORECAST COMMIT 50");
    assert(s_requests == requests && s_page_changes == 1U);
    /* Reconnect and move RTC beyond the original day: result replay still works. */
    forecast_protocol_reset_session();
    s_clock.calendar.day = 31U;
    begin(50U, 1U); expect("OK FORECAST BEGIN 50");
    day(50U, "2026-10-29", 2U); expect("OK FORECAST DAY 50");
    command("FORECAST COMMIT 50"); expect("OK FORECAST COMMIT 50");
    assert(s_requests == requests && s_page_changes == 1U);
    begin(50U, 1U);
    day(50U, "2026-10-29", 3U);
    command("FORECAST COMMIT 50"); expect("ERR FORECAST CONFLICT");
    assert(s_requests == requests);
}

static void test_validation_and_collection(void)
{
    static const char *const s_invalid_begin[] = {
        "FORECAST BEGIN 0," CITY_SZ ",1", "FORECAST BEGIN 1," CITY_SZ ",0",
        "FORECAST BEGIN 1," CITY_SZ ",8", "FORECAST BEGIN 4294967296," CITY_SZ ",1",
        "FORECAST BEGIN 1,Shanghai,1", "FORECAST BEGIN 1," CITY_SZ ",1,extra"
    };
    static const char *const s_invalid_day[] = {
        "FORECAST DAY 61,2026-02-29,2,180,270",
        "FORECAST DAY 61,2026-10-29,14,180,270",
        "FORECAST DAY 61,2026-10-29,2,280,270",
        "FORECAST DAY 61,2026-10-29,2,-1000,270",
        "FORECAST DAY 61,2026-10-29,2,180,1000",
        "FORECAST DAY 61,2026-10-29,2,180,270,extra"
    };
    size_t i;
    reset();
    forecast_protocol_time_result(1U);
    for (i = 0U; i < sizeof(s_invalid_begin) / sizeof(s_invalid_begin[0]); i++) {
        command(s_invalid_begin[i]); expect("ERR FORECAST");
        assert(!forecast_protocol_busy());
    }
    begin(61U, 2U); expect("OK FORECAST BEGIN 61");
    for (i = 0U; i < sizeof(s_invalid_day) / sizeof(s_invalid_day[0]); i++) {
        command(s_invalid_day[i]); expect("ERR FORECAST");
    }
    day(99U, "2026-10-29", 2U); expect("ERR FORECAST");
    day(61U, "2026-10-29", 2U); expect("OK FORECAST DAY 61");
    day(61U, "2026-10-29", 2U); expect("OK FORECAST DAY 61");
    day(61U, "2026-10-29", 3U); expect("ERR FORECAST CONFLICT");
    command("FORECAST COMMIT 61"); expect("ERR FORECAST");
    assert(s_requests == 0U);
    day(61U, "2026-10-31", 2U);
    command("FORECAST COMMIT 61"); expect("ERR FORECAST");
    assert(s_requests == 0U);
    command("FORECAST CANCEL 61"); expect("OK FORECAST CANCEL 61");
    assert(!forecast_protocol_busy());
    begin(62U, 1U); expect("ERR FORECAST TIME");
}

static void test_midnight_and_failed_save(void)
{
    forecast_batch_t stored;
    reset();
    forecast_protocol_time_result(1U);
    begin(70U, 1U);
    day(70U, "2026-10-29", 2U);
    s_clock.calendar.day = 30U;
    command("FORECAST COMMIT 70"); expect("ERR FORECAST");
    assert(s_requests == 0U);
    command("FORECAST CANCEL 70");
    forecast_protocol_time_result(1U);
    begin(71U, 1U);
    day(71U, "2026-10-31", 2U);
    s_accept_result = FILE_STORE_ERROR_BUSY;
    command("FORECAST COMMIT 71"); expect("ERR BUSY");
    assert(s_requests == 0U);
    s_accept_result = 0;
    command("FORECAST COMMIT 71");
    finish(-28); expect("ERR FORECAST STORAGE 71");
    assert(!forecast_get_batch(&stored) && s_page_changes == 0U);
    command("FORECAST COMMIT 71");
    assert(s_requests == 2U);
    finish(0); expect("OK FORECAST COMMIT 71");
    assert(s_page_changes == 1U && forecast_get_batch(&stored));
    assert(stored.id == 71U);
    forecast_protocol_time_result(1U);
    begin(72U, 1U);
    day(72U, "2026-11-01", 3U);
    command("FORECAST COMMIT 72");
    /* Session reset must not abandon an already accepted storage request. */
    forecast_protocol_reset_session();
    assert(forecast_protocol_busy());
    finish(-5); expect("ERR FORECAST STORAGE 72");
    assert(forecast_get_batch(&stored) && stored.id == 71U && s_page_changes == 1U);
    command("FORECAST CANCEL 72");
}

static void test_timeout_and_format_reset(void)
{
    reset();
    forecast_protocol_time_result(1U);
    begin(80U, 1U);
    clear_output();
    s_tick += 59999U;
    forecast_protocol_poll(s_tick);
    assert(forecast_protocol_busy() && s_output_length == 0U);
    s_tick++;
    forecast_protocol_poll(s_tick);
    expect("ERR FORECAST TIMEOUT 80");
    assert(!forecast_protocol_busy());
    begin(81U, 1U); expect("ERR FORECAST TIME");
    forecast_protocol_time_result(1U);
    begin(81U, 1U);
    s_storage.clear_generation++;
    forecast_protocol_poll(s_tick);
    assert(!forecast_protocol_busy());
    begin(82U, 1U); expect("ERR FORECAST TIME");
    /* Monotonic receive timeout also works across the uint32_t tick wrap. */
    s_tick = 0xFFFFFF00UL;
    forecast_protocol_time_result(1U);
    begin(83U, 1U);
    s_tick += 60000U;
    clear_output();
    forecast_protocol_poll(s_tick);
    expect("ERR FORECAST TIMEOUT 83");
    assert(!forecast_protocol_busy());
}

static void test_seven_days_and_ready_gates(void)
{
    static const char *const s_dates[] = {
        "2026-10-29", "2026-10-30", "2026-10-31", "2026-11-01",
        "2026-11-02", "2026-11-03", "2026-11-04"
    };
    forecast_batch_t stored;
    uint8_t i;
    reset();
    forecast_protocol_time_result(1U);
    s_storage.ready = 0U;
    begin(90U, 7U); expect("ERR BUSY");
    assert(!forecast_protocol_busy());
    s_storage.ready = 1U;
    s_storage.busy = 1U;
    begin(90U, 7U); expect("ERR BUSY");
    s_storage.busy = 0U;
    begin(90U, 7U); expect("OK FORECAST BEGIN 90");
    for (i = 7U; i > 0U; i--) {
        day(90U, s_dates[i - 1U], (uint8_t)(i - 1U));
        expect("OK FORECAST DAY 90");
    }
    s_clock.calendar_state = CLOCK_DATA_ERROR;
    command("FORECAST COMMIT 90"); expect("ERR FORECAST TIME");
    assert(s_requests == 0U);
    s_clock.calendar_state = CLOCK_DATA_READY;
    command("FORECAST COMMIT 90");
    finish(0);
    assert(forecast_get_batch(&stored) && stored.count == 7U);
    assert(stored.days[0].date.month == 10U && stored.days[0].date.day == 29U);
    assert(stored.days[6].date.month == 11U && stored.days[6].date.day == 4U);
    /* A TIME failure revokes an otherwise unused successful calibration. */
    forecast_protocol_time_result(1U);
    forecast_protocol_time_result(0U);
    begin(91U, 1U); expect("ERR FORECAST TIME");
    forecast_protocol_time_result(1U);
    forecast_protocol_reset_session();
    begin(91U, 1U); expect("ERR FORECAST TIME");
}

int main(void)
{
    test_time_gate_and_atomic_publish();
    test_duplicate_commit_and_lost_ack();
    test_validation_and_collection();
    test_midnight_and_failed_save();
    test_timeout_and_format_reset();
    test_seven_days_and_ready_gates();
    puts("PASS: forecast TIME gate, explicit dates, atomic publication, replay, rejects, failures and expiry-free day selection");
    return 0;
}
