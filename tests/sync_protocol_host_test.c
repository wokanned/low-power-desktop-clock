/* Real USART2 command handling, weather/page commit and clock service.
 * RTOS waits delimit owner-task iterations; only IO and storage are mocked. */
#include "debug.h"
#include "clock_service.h"
#include "clock_faults.h"
#include "display.h"
#include "epd_bringup.h"
#include "storage.h"
#include "storage_files.h"
#include "storage_forecast.h"
#include "stm32_clock.h"
#include "stm32_debug_uart.h"
#include "weather_feed.h"
#include "time_protocol.h"
#include "file_transfer.h"
#include "queue.h"
#include "task.h"
#include "cmsis_os.h"
#include <assert.h>
bool stm32_debug_uart_sleep_ready(void) { return true; }
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#undef assert
#define assert(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
    exit(EXIT_FAILURE); } } while (0)

#define TEST_HOUR_TICKS (3600UL * configTICK_RATE_HZ)
#define TEST_WEATHER "WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,2,235,180,270,65,2,35,2026-10-28T14:30\n"
#define TEST_TIME "TIME 2026-10-28T14:30:00\n"

static jmp_buf s_wait;
static TickType_t s_tick;
static uint32_t s_critical;
static uint8_t s_owner_role;
static uint8_t s_fail_send;
static uint8_t s_probe_weather;
static uint32_t s_tx_calls;
static uint32_t s_writes;
static uint32_t s_format_requests;
static uint32_t s_file_requests;
static int32_t s_write_result;
static const char *s_input = "";
static const char *s_last_input = "";
static char s_output[2048];
static size_t s_output_length;
static clock_calendar_t s_rtc = {2026U, 10U, 28U, 3U, 14U, 29U, 0U};
static storage_config_snapshot_t s_config;
static storage_forecast_snapshot_t s_forecast_storage;
static forecast_batch_t s_forecast_pending;
static uint32_t s_forecast_token;
static uint32_t s_forecast_requests;
static uint8_t s_forecast_done;
static int32_t s_forecast_result;
static uint32_t s_rx_errors;
static uint8_t s_hold_file;
static uint8_t s_file_complete;
static file_store_request_t s_file_pending;

static clock_view_model_t model(void)
{
    clock_view_model_t snapshot;
    clock_service_get_model(&snapshot);
    return snapshot;
}

static void expect_wifi(clock_data_state_t state, int32_t hours)
{
    clock_view_model_t snapshot = model();
    if (snapshot.wifi.state != state || snapshot.wifi.value != hours) {
        fprintf(stderr, "WIFI expected %u/%ld actual %u/%ld tick=%lu after [%s]\n",
            (unsigned int)state, (long)hours, (unsigned int)snapshot.wifi.state,
            (long)snapshot.wifi.value, (unsigned long)s_tick, s_last_input);
    }
    assert(snapshot.wifi.state == state && snapshot.wifi.value == hours);
}

void sync_test_enter(void) { s_critical++; }
void sync_test_exit(void)
{
    display_snapshot_t snapshot;
    assert(s_critical > 0U);
    s_critical--;
    if (s_critical == 0U && s_probe_weather != 0U) {
        s_probe_weather = 0U;
        display_get_snapshot(s_tick, &snapshot);
        assert(snapshot.page == DISPLAY_PAGE_CLOCK && snapshot.weather.available);
        assert(snapshot.clock.wifi.state == CLOCK_DATA_READY);
        assert(snapshot.clock.wifi.value == 0);
    }
}

TickType_t xTaskGetTickCount(void) { return s_tick; }
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{ return s_owner_role == 1U ? (void *)&s_rtc : (void *)&s_input; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task != NULL); }
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *p_woken)
{ assert(task != NULL); *p_woken = pdFALSE; }
void vTaskSuspend(TaskHandle_t task) { (void)task; assert(0); }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    assert(s_critical == 0U && clear == pdTRUE);
    assert(timeout > 0U && timeout <= (s_owner_role == 1U ? 1000U : 20U));
    longjmp(s_wait, 1);
    return 0U;
}
size_t xPortGetFreeHeapSize(void) { return 1000U; }

QueueHandle_t xQueueCreateStatic(uint32_t depth, uint32_t size, uint8_t *p_storage,
                                 StaticQueue_t *p_control)
{
    assert(depth == 8U && size == 64U && p_storage != NULL);
    memset(p_control, 0, sizeof(*p_control));
    return p_control;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *p_item, TickType_t wait)
{ (void)queue; (void)p_item; assert(wait == 0U); return pdPASS; }
BaseType_t xQueueReceive(QueueHandle_t queue, void *p_item, TickType_t wait)
{ (void)queue; (void)p_item; assert(wait == 0U); return pdFALSE; }
BaseType_t xQueueReset(QueueHandle_t queue) { queue->count = 0U; return pdPASS; }

bool stm32_debug_uart_rx_start(stm32_debug_uart_notify_t notify)
{ assert(notify != NULL); return true; }
bool stm32_debug_uart_rx_recover(void) { return true; }
void stm32_debug_uart_set_binary(uint8_t enabled) { assert(enabled == 0U); }
bool stm32_debug_uart_receive(uint8_t *p_byte, uint32_t *p_errors)
{
    *p_errors = s_rx_errors; s_rx_errors = 0U;
    if (*s_input == '\0') { return false; }
    *p_byte = (uint8_t)*s_input++;
    return true;
}
bool stm32_debug_uart_send(void *p_handle, const uint8_t *p_data, uint16_t length)
{
    (void)p_handle;
    assert(s_critical == 0U && s_output_length + length < sizeof(s_output));
    s_tx_calls++;
    if (s_fail_send != 0U) { s_fail_send = 0U; return false; }
    memcpy(s_output + s_output_length, p_data, length);
    s_output_length += length;
    s_output[s_output_length] = '\0';
    return true;
}

uint8_t stm32_clock_is_synchronized(void) { return 1U; }
int32_t stm32_clock_set_synchronized(uint8_t synchronized)
{ (void)synchronized; return 0; }

int32_t stm32_clock_read_calendar(clock_calendar_t *p_calendar)
{ assert(s_critical == 0U); *p_calendar = s_rtc; return 0; }
int32_t stm32_clock_write_calendar(const clock_calendar_t *p_calendar,
                                   clock_calendar_t *p_verified)
{
    assert(s_critical == 0U);
    s_writes++;
    if (s_write_result != 0) { return s_write_result; }
    s_rtc = *p_calendar;
    *p_verified = s_rtc;
    return 0;
}
int32_t stm32_clock_image_sizes(uint32_t *p_sram, uint32_t *p_flash)
{ assert(s_critical == 0U); *p_sram = 48000U; *p_flash = 98000U; return 0; }
int32_t stm32_clock_read_timestamp(uint32_t tick_ms, clock_timestamp_t *p_sample)
{
    int32_t result = stm32_clock_read_calendar(&p_sample->calendar);
    p_sample->millisecond = 0U;
    p_sample->read_span_ms = 1U;
    p_sample->tick_ms = tick_ms;
    return result;
}
int32_t stm32_clock_write_timestamp(const clock_timestamp_t *p_requested,
                                   uint32_t tick_ms, clock_timestamp_t *p_verified)
{
    clock_timestamp_t requested = *p_requested;
    int32_t result = stm32_clock_write_calendar(&requested.calendar, &p_verified->calendar);
    p_verified->millisecond = requested.millisecond;
    p_verified->tick_ms = tick_ms;
    p_verified->read_span_ms = 1U;
    return result;
}
void storage_get_config_snapshot(storage_config_snapshot_t *p_snapshot)
{ *p_snapshot = s_config; }
void storage_get_files_snapshot(file_store_snapshot_t *p_snapshot)
{
    memset(p_snapshot, 0, sizeof(*p_snapshot));
    p_snapshot->state = FILE_STORE_READY;
    p_snapshot->total_blocks = FILE_STORE_BLOCK_COUNT;
}
int32_t storage_request_format(clock_format_t format, uint32_t token)
{
    s_format_requests++;
    s_config.format = format;
    s_config.completed_token = token;
    s_config.completed_result = 0;
    return 0;
}
int32_t storage_file_request(const file_store_request_t *p_request)
{
    assert(p_request != NULL); s_file_requests++;
    if (s_hold_file == 0U) { return FILE_STORE_ERROR_BUSY; }
    assert(s_file_pending.token == 0U); s_file_pending = *p_request;
    return FILE_STORE_OK;
}
uint8_t storage_file_take_result(uint32_t token, file_store_response_t *p_response)
{
    assert(token == s_file_pending.token && token != 0U);
    if (s_file_complete == 0U) { return 0U; }
    memset(p_response, 0, sizeof(*p_response));
    p_response->token = token; p_response->operation = s_file_pending.operation;
    p_response->result = FILE_STORE_OK;
    s_file_pending.token = 0U; s_file_complete = 0U; s_hold_file = 0U;
    return 1U;
}
int32_t storage_maintenance_acquire(void) { return FILE_STORE_OK; }
void storage_maintenance_release(void) { }
int32_t storage_forecast_request(const forecast_batch_t *p_batch, uint32_t token)
{
    assert(s_forecast_token == 0U && token != 0U);
    assert(forecast_validate(p_batch, NULL));
    s_forecast_pending = *p_batch;
    s_forecast_token = token;
    s_forecast_requests++;
    s_forecast_storage.busy = 1U;
    return FILE_STORE_OK;
}
uint8_t storage_forecast_take_result(uint32_t token, int32_t *p_result)
{
    assert(token == s_forecast_token && token != 0U);
    if (s_forecast_done == 0U) { return 0U; }
    if (s_forecast_result == 0) { assert(forecast_publish(&s_forecast_pending)); }
    *p_result = s_forecast_result;
    s_forecast_token = 0U;
    s_forecast_done = 0U;
    s_forecast_storage.busy = 0U;
    return 1U;
}
void storage_forecast_get_snapshot(storage_forecast_snapshot_t *p_snapshot)
{ *p_snapshot = s_forecast_storage; }

osThreadId_t osThreadNew(osThreadFunc_t function, void *p_argument,
                         const osThreadAttr_t *p_attributes)
{ (void)function; (void)p_argument; (void)p_attributes; assert(0); return NULL; }
osThreadId_t osThreadGetId(void) { assert(0); return NULL; }
int32_t osThreadSuspend(osThreadId_t thread) { (void)thread; assert(0); return -1; }
int32_t osDelay(uint32_t ticks) { (void)ticks; assert(0); return -1; }
epd_gdey029t94_status_t epd_bringup_run_clock(void) { assert(0); return EPD_GDEY029T94_OK; }
epd_gdey029t94_status_t epd_bringup_stop(void) { return EPD_GDEY029T94_OK; }
epd_gdey029t94_status_t epd_bringup_get_last_status(void) { return EPD_GDEY029T94_OK; }
uint32_t epd_bringup_get_completed_frames(void) { return 0U; }

static void start_clock_owner(void)
{
    s_owner_role = 1U;
    if (setjmp(s_wait) == 0) { clock_service_run(); }
}

static void command(const char *p_input)
{
    s_owner_role = 2U;
    s_output_length = 0U;
    s_output[0] = '\0';
    s_input = p_input;
    s_last_input = p_input;
    if (setjmp(s_wait) == 0) { StartDebugTask(NULL); }
    assert(*s_input == '\0' && s_critical == 0U);
}

static void age_hour(void)
{
    s_tick += TEST_HOUR_TICKS;
    clock_service_poll();
    expect_wifi(CLOCK_DATA_READY, 1);
}

static void test_weather(void)
{
    weather_snapshot_t before, after;
    display_snapshot_t snapshot;
    uint32_t calls;
    command("PAGE CLOCK\nWEATHER invalid\nTIME 2026-02-29T14:30:00\nordinary traffic\n");
    expect_wifi(CLOCK_DATA_UNAVAILABLE, 0);
    assert(s_tx_calls == 0U && s_writes == 0U);

    s_probe_weather = 1U;
    command(TEST_WEATHER);
    assert(s_probe_weather == 0U && s_tx_calls == 0U);
    expect_wifi(CLOCK_DATA_READY, 0);
    weather_get_snapshot(s_tick, &before);
    s_tick += TEST_HOUR_TICKS - 1U;
    clock_service_poll();
    expect_wifi(CLOCK_DATA_READY, 0);
    s_tick++;
    clock_service_poll();
    expect_wifi(CLOCK_DATA_READY, 1);
    command(TEST_WEATHER); /* Identical packet resets age without a weather revision. */
    expect_wifi(CLOCK_DATA_READY, 0);
    weather_get_snapshot(s_tick, &after);
    assert(before.revision == after.revision);
    age_hour();
    command("WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,99,235,180,270,65,2,35,2026-10-28T14:30\n");
    expect_wifi(CLOCK_DATA_READY, 1);
    assert(s_tx_calls == 0U);

    command("CONSOLE ON\nPAGE CLOCK\nFORMAT 24\nFS STATUS\nFS LIST /\nordinary traffic\n");
    assert(Debug_IsConsoleActive() && s_format_requests == 1U && s_file_requests == 1U);
    expect_wifi(CLOCK_DATA_READY, 1);
    display_get_snapshot(s_tick, &snapshot);
    assert(snapshot.page == DISPLAY_PAGE_CLOCK);
    command("WEATHER invalid\n");
    assert(strcmp(s_output, "ERR WEATHER\r\n") == 0);
    expect_wifi(CLOCK_DATA_READY, 1);

    s_fail_send = 1U;
    calls = s_tx_calls;
    command(TEST_WEATHER);
    assert(s_tx_calls == calls + 1U && s_output_length == 0U);
    expect_wifi(CLOCK_DATA_READY, 0);
    age_hour();
    command(TEST_WEATHER);
    assert(strcmp(s_output, "OK WEATHER\r\n") == 0);
    expect_wifi(CLOCK_DATA_READY, 0);
}

static void test_time(void)
{
    clock_view_model_t snapshot;
    uint32_t writes;
    int32_t failure;
    age_hour();
    writes = s_writes;
    command("TIME 2026-02-29T14:30:00\n");
    assert(strcmp(s_output, "ERR TIME\r\n") == 0 && s_writes == writes);
    expect_wifi(CLOCK_DATA_READY, 1);
    command(TEST_TIME TEST_TIME);
    assert(strcmp(s_output, "ERR BUSY\r\n") == 0 && s_writes == writes);
    expect_wifi(CLOCK_DATA_READY, 1); /* Acceptance alone is not success. */
    clock_service_poll();
    snapshot = model();
    assert(snapshot.calendar_state == CLOCK_DATA_READY && snapshot.calendar.minute == 30U);
    expect_wifi(CLOCK_DATA_READY, 0);
    s_fail_send = 1U;
    command("");
    assert(s_output_length == 0U && s_writes == writes + 1U);
    expect_wifi(CLOCK_DATA_READY, 0); /* Lost OK TIME does not lose success. */

    for (failure = -1; failure >= -3; failure--) {
        if (failure == -1) { age_hour(); }
        s_write_result = failure;
        command(TEST_TIME);
        assert(s_output_length == 0U);
        clock_service_poll();
        command("");
        assert(strcmp(s_output, "ERR RTC\r\n") == 0);
        expect_wifi(CLOCK_DATA_READY, 1);
    }
    s_write_result = 0;
    command("CONSOLE OFF\n");
    assert(!Debug_IsConsoleActive());
    command(TEST_TIME);
    expect_wifi(CLOCK_DATA_READY, 1);
    clock_service_poll();
    command("");
    assert(s_output_length == 0U);
    expect_wifi(CLOCK_DATA_READY, 0);
    age_hour();
    s_write_result = -1;
    command(TEST_TIME);
    clock_service_poll();
    command("");
    assert(s_output_length == 0U);
    expect_wifi(CLOCK_DATA_READY, 1);

    clock_service_mark_unavailable();
    command(TEST_TIME); /* Owner unavailable is also silent and not a success. */
    expect_wifi(CLOCK_DATA_READY, 1);
    clock_service_init();
    Debug_Init();
    start_clock_owner();
    expect_wifi(CLOCK_DATA_UNAVAILABLE, 0);
}

static void test_forecast(void)
{
    static const char s_begin[] = "FORECAST BEGIN 900,\xE6\xB7\xB1\xE5\x9C\xB3,2\n";
    static const char s_days[] =
        "FORECAST DAY 900,2026-10-29,3,190,280\n"
        "FORECAST DAY 900,2026-10-28,2,180,270\n";
    display_snapshot_t snapshot;
    forecast_batch_t stored;
    uint32_t writes;
    s_write_result = 0;
    s_forecast_storage.ready = 1U;
    command("CONSOLE ON\n");
    assert(strcmp(s_output, "OK CONSOLE ON\r\n") == 0);
    command(s_begin);
    assert(strstr(s_output, "ERR FORECAST TIME") != NULL);
    command(TEST_TIME);
    assert(s_output_length == 0U);
    /* Before the RTC owner verifies the write there is no permit. */
    command(s_begin);
    assert(strstr(s_output, "ERR BUSY") != NULL ||
        strstr(s_output, "ERR FORECAST TIME") != NULL);
    clock_service_poll();
    command("");
    assert(strcmp(s_output, "OK TIME\r\n") == 0);
    writes = s_writes;
    command(s_begin);
    assert(strstr(s_output, "OK FORECAST BEGIN 900") != NULL);
    command("TIME 2026-10-28T14:31:00\nCONSOLE OFF\nFS LIST /\n");
    assert(Debug_IsConsoleActive() && s_writes == writes);
    assert(strstr(s_output, "ERR BUSY") != NULL);
    command(s_days);
    assert(strstr(s_output, "OK FORECAST DAY 900") != NULL);
    command("PAGE CLOCK\nFORECAST COMMIT 900\n");
    assert(s_forecast_requests == 1U && s_forecast_token != 0U);
    assert(strstr(s_output, "OK FORECAST COMMIT") == NULL);
    assert(!forecast_get_batch(&stored));
    display_get_snapshot(s_tick, &snapshot);
    assert(snapshot.page == DISPLAY_PAGE_CLOCK);
    command("CONSOLE OFF\n");
    assert(Debug_IsConsoleActive() && strstr(s_output, "ERR BUSY") != NULL);
    /* Storage completion is durable even if the final UART ACK fails. */
    s_forecast_done = 1U;
    s_forecast_result = 0;
    s_fail_send = 1U;
    command("");
    assert(s_output_length == 0U && s_forecast_token == 0U);
    assert(forecast_get_batch(&stored) && stored.id == 900U);
    display_get_snapshot(s_tick, &snapshot);
    assert(snapshot.page == DISPLAY_PAGE_WEATHER);
    command("PAGE CLOCK\nFORECAST COMMIT 900\n");
    assert(strstr(s_output, "OK FORECAST COMMIT 900") != NULL);
    assert(s_forecast_requests == 1U && s_writes == writes);
    display_get_snapshot(s_tick, &snapshot);
    assert(snapshot.page == DISPLAY_PAGE_CLOCK); /* Replay has no page side effect. */
    command("FORECAST BEGIN 901,\xE6\xB7\xB1\xE5\x9C\xB3,1\n");
    assert(strstr(s_output, "ERR FORECAST TIME") != NULL);
    command("FORECAST STATUS\n");
    assert(strstr(s_output, "FORECAST SAVED") != NULL && strstr(s_output, "900") != NULL);
    command("CONSOLE OFF\n");
    assert(!Debug_IsConsoleActive());
    command("FORECAST COMMIT 900\n");
    assert(s_output_length == 0U && s_forecast_requests == 1U);
}

static void test_forecast_time_session_boundary(void)
{
    static const char s_begin[] = "FORECAST BEGIN 850,\xE6\xB7\xB1\xE5\x9C\xB3,1\n";
    uint8_t completes_before_console;
    uint32_t writes = s_writes;
    s_write_result = 0;
    s_forecast_storage.ready = 1U;
    assert(!Debug_IsConsoleActive());
    for (completes_before_console = 0U; completes_before_console < 2U;
         completes_before_console++) {
        command(TEST_TIME);
        assert(s_output_length == 0U && !Debug_IsConsoleActive());
        if (completes_before_console != 0U) { clock_service_poll(); }
        /* ON is parsed before debugTask consumes any completed TIME result.
         * Exercise RTC completion on either side of that session boundary. */
        command("CONSOLE ON\n");
        assert(Debug_IsConsoleActive());
        assert(strcmp(s_output, "OK CONSOLE ON\r\n") == 0);
        if (completes_before_console == 0U) {
            clock_service_poll();
            command("");
            assert(s_output_length == 0U); /* The old result stays silent. */
        }
        command(s_begin);
        assert(strstr(s_output, "ERR FORECAST TIME") != NULL);
        assert(s_forecast_requests == 0U);
        command("CONSOLE OFF\n");
        assert(!Debug_IsConsoleActive());
    }
    assert(s_writes == writes + 2U);
    command("CONSOLE ON\n");
    command(TEST_TIME);
    assert(s_output_length == 0U);
    command(s_begin);
    assert(strstr(s_output, "ERR FORECAST TIME") != NULL ||
        strstr(s_output, "ERR BUSY") != NULL);
    clock_service_poll();
    command("");
    assert(strcmp(s_output, "OK TIME\r\n") == 0);
    assert(s_writes == writes + 3U);
    command(s_begin);
    assert(strstr(s_output, "OK FORECAST BEGIN 850") != NULL);
    command("FORECAST CANCEL 850\nCONSOLE OFF\n");
    assert(strstr(s_output, "OK FORECAST CANCEL 850") != NULL);
    assert(!Debug_IsConsoleActive() && s_forecast_requests == 0U);
}

static uint32_t feed_crc(uint32_t crc, const char *p_line)
{
    uint8_t bit;
    while (*p_line != '\0') {
        crc ^= (uint8_t)*p_line++;
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
        }
    }
    return crc;
}

static void test_weather_feed_integration(void)
{
    static const char s_begin[] = "WDATA BEGIN 1000,1,0,0,2,2026-10-28T14:30:00,-,0\n";
    static const char s_now[] = "WDATA NOW 1000,0,2,235,180,270,72,2,50,3,4,2026-10-28T14:30\n";
    static const char s_metrics[] = "WDATA METRICS 1000,1,251,60,42,0,1012,180\n";
    static const char s_time2[] = "TIME2 2026-10-28T14:30:00.250\n";
    weather_feed_snapshot_t before;
    weather_feed_snapshot_t after;
    display_snapshot_t display;
    weather_feed_lease_t lease;
    uint32_t crc;
    uint32_t format_requests;
    uint32_t file_requests;
    uint32_t writes;
    char commit[80];
    char damaged[230];
    assert(!Debug_IsConsoleActive());
    command(s_begin); weather_feed_get_snapshot(s_tick, &before);
    assert(s_output_length == 0U && before.available == 0U && weather_feed_busy() == 0U);
    command("CONSOLE ON\nPAGE CLOCK\n");
    age_hour();
    command(s_begin); assert(strcmp(s_output, "OK WDATA BEGIN\r\n") == 0);
    assert(weather_feed_busy() != 0U);
    format_requests = s_format_requests; file_requests = s_file_requests; writes = s_writes;
    command("TIME?\nTIME2 2026-10-28T14:30:00.250\nTIME 2026-10-28T14:30:00\nFS MKDIR /probe\nFORECAST BEGIN 1000,\xE6\xB7\xB1\xE5\x9C\xB3,1\nCONSOLE OFF\n");
    assert(s_writes == writes && s_file_requests == file_requests && Debug_IsConsoleActive());
    assert(time_protocol_busy() == 0U && strstr(s_output, "ERR BUSY") != NULL);
    command("FORMAT 12\n");
    assert(strcmp(s_output, "ERR BUSY\r\n") == 0 && s_format_requests == format_requests);
    command(s_now); assert(strstr(s_output, "OK WDATA 1000,0") != NULL);
    command(s_metrics); assert(strstr(s_output, "OK WDATA 1000,1") != NULL);
    crc = feed_crc(UINT32_MAX, s_now); crc = feed_crc(crc, s_metrics) ^ UINT32_MAX;
    (void)snprintf(commit, sizeof(commit), "WDATA COMMIT 1000,2,%08lX\n", (unsigned long)crc);
    command("PAGE CLOCK\n"); s_fail_send = 1U; command(commit);
    assert(s_output_length == 0U); expect_wifi(CLOCK_DATA_READY, 0);
    weather_feed_get_snapshot(s_tick, &after);
    assert(after.available != 0U && after.commit_count == before.commit_count + 1U);
    assert(weather_feed_acquire(after.revision, &lease));
    assert(lease.p_batch->live.condition == 2U && lease.p_batch->metrics.value[WEATHER_METRIC_AQI_CN] == 42);
    weather_feed_release(&lease);
    display_get_snapshot(s_tick, &display); assert(display.page == DISPLAY_PAGE_CLOCK);
    /* Failed final ACK does not roll back RAM or reset rotation. Replay has no sync side effect. */
    age_hour(); command(commit); expect_wifi(CLOCK_DATA_READY, 1);
    assert(strcmp(s_output, "OK WDATA COMMIT\r\n") == 0);
    weather_feed_get_snapshot(s_tick, &before);
    assert(before.commit_count == after.commit_count && before.revision == after.revision);
    /* Same content under a new batch ID resets synchronization, without redraw revision. */
    command("WDATA BEGIN 1001,1,0,0,2,2026-10-28T14:30:00,-,0\n");
    command("WDATA NOW 1001,0,2,235,180,270,72,2,50,3,4,2026-10-28T14:30\n");
    command("WDATA METRICS 1001,1,251,60,42,0,1012,180\n");
    crc = feed_crc(UINT32_MAX, "WDATA NOW 1001,0,2,235,180,270,72,2,50,3,4,2026-10-28T14:30\n");
    crc = feed_crc(crc, "WDATA METRICS 1001,1,251,60,42,0,1012,180\n") ^ UINT32_MAX;
    (void)snprintf(commit, sizeof(commit), "WDATA COMMIT 1001,2,%08lX\n", (unsigned long)crc);
    command(commit); expect_wifi(CLOCK_DATA_READY, 0);
    weather_feed_get_snapshot(s_tick, &after);
    assert(after.commit_count == before.commit_count + 1U && after.revision == before.revision);
    /* RX framing error and overlong WDATA cancel only the uncommitted candidate. */
    command("WDATA BEGIN 1002,1,0,0,2,2026-10-28T14:30:00,-,0\n");
    s_rx_errors = STM32_DEBUG_UART_RX_ERROR; command("");
    assert(strcmp(s_output, "ERR RX\r\n") == 0 && weather_feed_busy() == 0U);
    command("WDATA BEGIN 1003,1,0,0,2,2026-10-28T14:30:00,-,0\n");
    memcpy(damaged, "WDATA NOW ", 10U); memset(damaged + 10U, 'X', 214U);
    damaged[224] = '\n'; damaged[225] = '\0'; command(damaged);
    assert(strcmp(s_output, "ERR RX\r\n") == 0 && weather_feed_busy() == 0U);
    weather_feed_get_snapshot(s_tick, &before); assert(before.commit_count == after.commit_count);
    /* Asynchronous TIME2 excludes a new feed/format/FS/forecast until owner completion. */
    age_hour(); command(s_time2); assert(time_protocol_busy() != 0U && s_output_length == 0U);
    format_requests = s_format_requests; file_requests = s_file_requests;
    command("WDATA BEGIN 1004,1,0,0,2,2026-10-28T14:30:00,-,0\nFORMAT 12\nFS MKDIR /probe\nFORECAST BEGIN 1004,\xE6\xB7\xB1\xE5\x9C\xB3,1\n");
    assert(weather_feed_busy() == 0U && s_format_requests == format_requests && s_file_requests == file_requests);
    clock_service_poll(); command("");
    assert(strstr(s_output, "OK TIME2 2026-10-28T14:30:00.250 RX ") != NULL);
    expect_wifi(CLOCK_DATA_READY, 0);
    /* File service pending in text mode excludes the same new operations. */
    s_hold_file = 1U; command("FS MKDIR /probe\n");
    assert(file_transfer_busy() != 0U && s_output_length == 0U);
    command("WDATA BEGIN 1005,1,0,0,2,2026-10-28T14:30:00,-,0\nTIME?\n");
    assert(weather_feed_busy() == 0U && time_protocol_busy() == 0U);
    assert(strstr(s_output, "ERR BUSY") != NULL);
    s_file_complete = 1U; command(""); assert(strcmp(s_output, "OK FS\r\n") == 0);
    assert(file_transfer_busy() == 0U);
    /* TIME2 grants the old forecast gate only after verified completion; feed remains excluded. */
    command("FORECAST BEGIN 1006,\xE6\xB7\xB1\xE5\x9C\xB3,1\n");
    assert(strstr(s_output, "OK FORECAST BEGIN 1006") != NULL);
    command("WDATA BEGIN 1006,1,0,0,2,2026-10-28T14:30:00,-,0\n");
    assert(strcmp(s_output, "ERR BUSY\r\n") == 0 && weather_feed_busy() == 0U);
    command("FORECAST CANCEL 1006\nCONSOLE OFF\n"); assert(!Debug_IsConsoleActive());
}

int main(void)
{
    s_config.ready = 1U;
    s_config.format = CLOCK_FORMAT_24;
    s_tick = 100U;
    clock_service_init();
    Debug_Init();
    start_clock_owner();
    expect_wifi(CLOCK_DATA_UNAVAILABLE, 0);
    test_weather();
    test_time();
    test_forecast_time_session_boundary();
    test_forecast();
    test_weather_feed_integration();
    puts("PASS: real USART2/weather/clock, WDATA/TIME2/FS/FORECAST exclusion, commit/ACK replay, RX cancellation, silent sessions and RAM reset");
    return 0;
}
