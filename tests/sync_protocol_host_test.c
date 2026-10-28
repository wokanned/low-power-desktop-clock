/* Real USART2 command handling, weather/page commit and clock service.
 * RTOS waits delimit owner-task iterations; only IO and storage are mocked. */
#include "debug.h"
#include "clock_service.h"
#include "clock_faults.h"
#include "display.h"
#include "epd_bringup.h"
#include "storage.h"
#include "storage_files.h"
#include "stm32_clock.h"
#include "stm32_debug_uart.h"
#include "queue.h"
#include "task.h"
#include "cmsis_os.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

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
static char s_output[2048];
static size_t s_output_length;
static clock_calendar_t s_rtc = {2026U, 10U, 28U, 3U, 14U, 29U, 0U};
static storage_config_snapshot_t s_config;

static clock_view_model_t model(void)
{
    clock_view_model_t snapshot;
    clock_service_get_model(&snapshot);
    return snapshot;
}

static void expect_wifi(clock_data_state_t state, int32_t hours)
{
    clock_view_model_t snapshot = model();
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
        assert(snapshot.page == DISPLAY_PAGE_WEATHER && snapshot.weather.available);
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
    *p_errors = 0U;
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
{ assert(p_request != NULL); s_file_requests++; return FILE_STORE_ERROR_BUSY; }
uint8_t storage_file_take_result(uint32_t token, file_store_response_t *p_response)
{ (void)token; (void)p_response; assert(0); return 0U; }

osThreadId_t osThreadNew(osThreadFunc_t function, void *p_argument,
                         const osThreadAttr_t *p_attributes)
{ (void)function; (void)p_argument; (void)p_attributes; assert(0); return NULL; }
osThreadId_t osThreadGetId(void) { assert(0); return NULL; }
int32_t osThreadSuspend(osThreadId_t thread) { (void)thread; assert(0); return -1; }
int32_t osDelay(uint32_t ticks) { (void)ticks; assert(0); return -1; }
epd_gdey029t94_status_t epd_bringup_run_clock(void) { assert(0); return EPD_GDEY029T94_OK; }
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
    puts("PASS: real USART2/weather/clock sync, duplicates, rejects, async TIME, silent mode, failed TX and RAM reset");
    return 0;
}
