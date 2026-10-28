#include "debug.h"
#include "clock_faults.h"
#include "clock_service.h"
#include "display.h"
#include "storage.h"
#include "storage_files.h"
#include "stm32_debug_uart.h"
#include "weather.h"
#include "queue.h"
#include "task.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf s_finished;
static char s_output[4096];
static uint32_t s_output_length;
static uint32_t s_binary_frames;
static uint32_t s_step;
static const char *s_input = "";
static float s_frame[VOFA_MAX_CHANNELS];
static storage_config_snapshot_t s_config;
static uint32_t s_requested_token;
static uint32_t s_storage_requests;
static uint32_t s_fault_mask;
static uint32_t s_send_failure;
static display_page_t s_selected_page = DISPLAY_PAGE_CLOCK;
static uint32_t s_weather_publications;
static uint32_t s_last_weather_tick;
static char s_boundary_input[256];
static uint32_t s_time_token;
static uint32_t s_time_requests;
static uint32_t s_time_request_calls;
static uint32_t s_time_takes;
static int32_t s_time_accept_result;
static int32_t s_time_result;
static uint8_t s_time_done;
static clock_calendar_t s_requested_time;

static void clear_output(void)
{
    s_output_length = 0U;
    s_output[0] = '\0';
}

static void test_time_step(uint32_t step)
{
    switch (step) {
    case 1U:
        assert(s_binary_frames == 2U && Debug_GetQueueDropCount() == 0U);
        clear_output();
        s_input = "TIME 2026-02-29T14:30:00\nTIME 2026-10-27T14:30:00\r\n"
            "TIME 2026-10-27T14:30:01\n";
        break;
    case 2U:
        assert(s_time_requests == 1U && s_time_request_calls == 1U);
        assert(s_time_token != 0U && s_output_length == 0U);
        assert(s_requested_time.year == 2026U && s_requested_time.weekday == 2U);
        assert(s_requested_time.hour == 14U && s_requested_time.second == 0U);
        s_time_done = 1U;
        s_time_result = 0;
        break;
    case 3U:
        assert(s_time_takes == 1U && s_time_token == 0U && s_output_length == 0U);
        s_input = "TIME 2000-02-29T00:00:00\nCONSOLE ON\n";
        break;
    case 4U:
        assert(s_time_requests == 2U && Debug_IsConsoleActive());
        assert(strcmp(s_output, "OK CONSOLE ON\r\n") == 0);
        assert(s_requested_time.year == 2000U && s_requested_time.weekday == 2U);
        s_time_done = 1U;
        s_time_result = -1;
        break;
    case 5U:
        assert(s_time_takes == 2U && s_time_token == 0U);
        /* A request submitted in binary mode stays silent after CONSOLE ON. */
        assert(strcmp(s_output, "OK CONSOLE ON\r\n") == 0);
        clear_output();
        s_input = "TIME\nTIME 2026-02-29T14:30:00\nTIME 2026-10-27T14:30:00Z\n"
            "TIME 2026-10-27T14:30:00\nTIME 2026-10-27T14:30:01\nCONSOLE OFF\n";
        break;
    case 6U:
        assert(s_time_requests == 3U && s_time_token != 0U && Debug_IsConsoleActive());
        assert(strcmp(s_output, "ERR TIME\r\nERR TIME\r\nERR TIME\r\n"
                      "ERR BUSY\r\nERR BUSY\r\n") == 0);
        s_input = "FORMAT 24\n";
        break;
    case 7U:
        assert(s_storage_requests == 3U && s_time_token != 0U);
        assert(strstr(s_output, "OK TIME") == NULL);
        s_time_done = 1U;
        s_time_result = 0;
        s_input = "PAGE WEATHER\nWEATHER "
            "\xE6\xB7\xB1\xE5\x9C\xB3,2,237,180,270,65,2,35,2026-10-26T10:32\n";
        break;
    case 8U:
        assert(s_time_takes == 3U && s_time_token == 0U);
        assert(strstr(s_output, "OK PAGE WEATHER\r\nOK WEATHER\r\nOK TIME\r\n") != NULL);
        assert(strstr(s_output, "OK FORMAT 24") == NULL);
        s_config.completed_token = s_requested_token;
        s_config.completed_result = 0;
        break;
    case 9U:
        assert(strstr(s_output, "OK FORMAT 24\r\n") != NULL);
        clear_output();
        s_input = "FORMAT 24\nTIME 2026-10-27T14:30:00\nCONSOLE OFF\n";
        break;
    case 10U:
        assert(s_time_requests == 4U && s_storage_requests == 4U);
        assert(s_time_token != 0U && strcmp(s_output, "ERR BUSY\r\n") == 0);
        s_time_done = 1U;
        s_time_result = -1;
        s_config.completed_token = s_requested_token;
        s_config.completed_result = 0;
        break;
    case 11U:
        assert(s_time_takes == 4U && s_time_token == 0U);
        assert(strcmp(s_output, "ERR BUSY\r\nOK FORMAT 24\r\nERR RTC\r\n") == 0);
        clear_output();
        s_time_accept_result = -2;
        s_input = "TIME 2026-10-27T14:30:00\n";
        break;
    case 12U:
        assert(strcmp(s_output, "ERR RTC\r\n") == 0 && s_time_requests == 4U);
        clear_output();
        s_time_accept_result = -1;
        s_input = "TIME 2026-10-27T14:30:00\n";
        break;
    case 13U:
        assert(strcmp(s_output, "ERR BUSY\r\n") == 0 && s_time_requests == 4U);
        clear_output();
        s_time_accept_result = -3;
        s_input = "TIME 2026-10-27T14:30:00\n";
        break;
    case 14U:
        assert(strcmp(s_output, "ERR TIME\r\n") == 0 && s_time_requests == 4U);
        clear_output();
        s_time_accept_result = 0;
        s_input = "TIME 2026-10-27T14:30:00\nCONSOLE OFF\n";
        break;
    case 15U:
        assert(s_time_requests == 5U && strcmp(s_output, "ERR BUSY\r\n") == 0);
        s_time_done = 1U;
        s_time_result = 0;
        s_send_failure = 1U;
        break;
    case 16U:
        assert(s_time_takes == 5U && s_time_token == 0U);
        assert(strcmp(s_output, "ERR BUSY\r\n") == 0 && Debug_GetUartErrorCount() == 3U);
        clear_output();
        s_input = "TIME 2026-10-27T14:30:00\nCONSOLE OFF\n";
        break;
    case 17U:
        assert(s_time_requests == 6U && strcmp(s_output, "ERR BUSY\r\n") == 0);
        s_time_done = 1U;
        s_time_result = 0;
        break;
    case 18U:
        assert(s_time_takes == 6U && s_time_token == 0U);
        assert(strcmp(s_output, "ERR BUSY\r\nOK TIME\r\n") == 0);
        s_input = "CONSOLE OFF\n";
        break;
    case 19U:
        assert(!Debug_IsConsoleActive());
        assert(strcmp(s_output, "ERR BUSY\r\nOK TIME\r\nOK CONSOLE OFF\r\n") == 0);
        clear_output();
        s_time_accept_result = -2;
        s_input = "TIME invalid\nTIME 2026-10-27T14:30:00\n";
        break;
    case 20U:
        assert(s_output_length == 0U && s_time_requests == 6U);
        s_time_accept_result = 0;
        s_input = "TIME 2026-10-27T14:30:00\n";
        break;
    case 21U:
        assert(s_output_length == 0U && s_time_requests == 7U && s_time_token != 0U);
        s_time_done = 1U;
        s_time_result = -1;
        break;
    default:
        assert(s_time_takes == 7U && s_time_token == 0U && s_output_length == 0U);
        assert(s_time_request_calls == 11U && s_storage_requests == 4U);
        longjmp(s_finished, 1);
    }
}

static void make_boundary_input(void)
{
    static const char s_before[] = "FORMAT 12\nERRORS\n";
    static const char s_prefix[] = "WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,";
    static const char s_suffix[] = "2,235,180,270,65,2,35,2026-10-26T10:30";
    static const char s_after[] = "\n";
    uint32_t offset = (uint32_t)strlen(s_before);
    uint32_t prefix_length = (uint32_t)strlen(s_prefix);
    uint32_t suffix_length = (uint32_t)strlen(s_suffix);
    uint32_t padding = 127U - prefix_length - suffix_length;
    memcpy(s_boundary_input, s_before, offset);
    memcpy(s_boundary_input + offset, s_prefix, prefix_length);
    memset(s_boundary_input + offset + prefix_length, '0', padding);
    memcpy(s_boundary_input + offset + prefix_length + padding, s_suffix, suffix_length);
    memcpy(s_boundary_input + offset + 127U, s_after, strlen(s_after) + 1U);
}

QueueHandle_t xQueueCreateStatic(uint32_t depth, uint32_t size, uint8_t *p_storage,
                                 StaticQueue_t *p_control)
{
    assert(depth == 8U && size == sizeof(s_frame));
    (void)p_storage;
    memset(p_control, 0, sizeof(*p_control));
    return p_control;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *p_item, TickType_t wait)
{
    assert(wait == 0U);
    if (queue->count == 8U) { return pdFALSE; }
    memcpy(queue->data[queue->count++], p_item, sizeof(s_frame));
    return pdPASS;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *p_item, TickType_t wait)
{
    assert(wait == 0U);
    if (queue->count == 0U) { return pdFALSE; }
    memcpy(p_item, queue->data[0], sizeof(s_frame));
    queue->count--;
    memmove(queue->data[0], queue->data[1], queue->count * sizeof(s_frame));
    return pdPASS;
}
BaseType_t xQueueReset(QueueHandle_t queue) { queue->count = 0U; return pdPASS; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &s_step; }
TickType_t xTaskGetTickCount(void) { return s_step * 20U; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task != NULL); }
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *p_woken)
{ assert(task != NULL); *p_woken = pdFALSE; }
void vTaskSuspend(TaskHandle_t task) { (void)task; assert(0); }

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    assert(clear == pdTRUE && wait == 20U);
    s_step++;
    switch (s_step) {
    case 1U:
        assert(s_binary_frames == 1U && s_output_length == 0U);
        make_boundary_input();
        s_input = s_boundary_input;
        break;
    case 2U:
        assert(s_output_length == 0U);
        {
            weather_snapshot_t snapshot;
            weather_get_snapshot(xTaskGetTickCount(), &snapshot);
            assert(snapshot.available == 1U && snapshot.fresh == 1U);
            assert(snapshot.data.temperature_tenths == 235);
            assert(s_selected_page == DISPLAY_PAGE_WEATHER);
            assert(s_weather_publications == 1U && s_last_weather_tick == 20U);
        }
        s_input = "CONSOLE ON\r\n";
        break;
    case 3U:
        assert(Debug_IsConsoleActive());
        assert(strcmp(s_output, "OK CONSOLE ON\r\n") == 0);
        assert(Debug_SendFloats(s_frame, VOFA_MAX_CHANNELS));
        assert(Debug_GetSuppressedCount() == 1U && Debug_GetQueueDropCount() == 0U);
        s_input = "FORMAT 12\nWEATHER "
            "\xE6\xB7\xB1\xE5\x9C\xB3,99,236,180,270,65,2,35,2026-10-26T10:31\n"
            "WEATHER "
            "\xE6\xB7\xB1\xE5\x9C\xB3,2,236,180,270,65,2,35,2026-10-26T10:31\n"
            "PAGE CLOCK\nFORMAT 24\nCONSOLE OFF\n";
        break;
    case 4U:
        assert(s_storage_requests == 1U && Debug_IsConsoleActive());
        assert(strstr(s_output, "OK FORMAT") == NULL);
        assert(strstr(s_output, "ERR WEATHER\r\nOK WEATHER\r\nOK PAGE CLOCK\r\n") != NULL);
        assert(strstr(s_output, "ERR BUSY\r\nERR BUSY\r\n") != NULL);
        assert(s_selected_page == DISPLAY_PAGE_CLOCK);
        assert(s_weather_publications == 2U && s_last_weather_tick == 60U);
        {
            weather_snapshot_t snapshot;
            weather_get_snapshot(xTaskGetTickCount(), &snapshot);
            assert(snapshot.data.temperature_tenths == 236);
            assert(strcmp(snapshot.data.source_time, "2026-10-26T10:31") == 0);
        }
        s_config.completed_token = s_requested_token;
        s_config.completed_result = 0;
        break;
    case 5U:
        assert(strstr(s_output, "OK FORMAT 12\r\n") != NULL);
        s_input = "ERRORS\nWEATHER invalid\n";
        break;
    case 6U:
        assert(strstr(s_output, "ERRORS 1 REV 3 EPD -2\r\n") != NULL);
        assert(strstr(s_output, "RTC active=001 first=001 2026-10-18 12:34:56 tick=42") != NULL);
        assert(strstr(s_output, "ERR WEATHER\r\n") != NULL);
        assert(s_weather_publications == 2U && s_selected_page == DISPLAY_PAGE_CLOCK);
        s_input = "FORMAT 24\nWEATHER "
            "\xE6\xB7\xB1\xE5\x9C\xB3,2,236,180,270,65,2,35,2026-10-26T10:31\n";
        break;
    case 7U:
        assert(s_storage_requests == 2U && strstr(s_output, "OK FORMAT 24") == NULL);
        /* Even an identical valid packet selects weather again while FORMAT is pending. */
        assert(s_weather_publications == 3U && s_last_weather_tick == 120U);
        assert(s_selected_page == DISPLAY_PAGE_WEATHER);
        s_config.completed_token = s_requested_token;
        s_config.completed_result = -5;
        break;
    case 8U:
        assert(strstr(s_output, "ERR STORAGE\r\n") != NULL);
        assert(strstr(s_output, "OK FORMAT 24") == NULL);
        s_input = "CONSOLE OFF\x01\n";
        break;
    case 9U:
        assert(Debug_IsConsoleActive() && (s_fault_mask & CLOCK_FAULT_RX) != 0U);
        assert(strstr(s_output, "ERR RX\r\n") != NULL);
        s_send_failure = 1U;
        s_input = "CONSOLE OFF\n";
        break;
    case 10U:
        assert(Debug_IsConsoleActive() && (s_fault_mask & CLOCK_FAULT_TX) != 0U);
        assert(Debug_GetUartErrorCount() == 2U);
        s_input = "CONSOLE OFF\n";
        break;
    case 11U:
        assert(!Debug_IsConsoleActive() && s_fault_mask == 0U);
        assert(Debug_SendFloats(s_frame, VOFA_MAX_CHANNELS));
        break;
    default:
        test_time_step(s_step - 11U);
        break;
    }
    return 1U;
}

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
bool stm32_debug_uart_send(void *p_handle, const uint8_t *p_data, uint16_t len)
{
    (void)p_handle;
    if (len == 68U) {
        assert(!Debug_IsConsoleActive());
        assert(memcmp(p_data + 64U, "\0\0\x80\x7F", 4U) == 0);
        s_binary_frames++;
    } else {
        assert(Debug_IsConsoleActive() && len <= 64U);
        if (s_send_failure != 0U) { s_send_failure = 0U; return false; }
        assert(s_output_length + len < sizeof(s_output));
        memcpy(s_output + s_output_length, p_data, len);
        s_output_length += len;
        s_output[s_output_length] = '\0';
    }
    return true;
}
int32_t storage_request_format(clock_format_t format, uint32_t token)
{
    assert(format == (s_storage_requests == 0U ? CLOCK_FORMAT_12 : CLOCK_FORMAT_24));
    s_requested_token = token;
    s_storage_requests++;
    return 0;
}
void storage_get_config_snapshot(storage_config_snapshot_t *p_snapshot)
{ *p_snapshot = s_config; }
int32_t storage_file_request(const file_store_request_t *p_request)
{ (void)p_request; assert(0); return FILE_STORE_ERROR_BUSY; }
uint8_t storage_file_take_result(uint32_t token, file_store_response_t *p_response)
{ (void)token; (void)p_response; assert(0); return 0U; }
void storage_get_files_snapshot(file_store_snapshot_t *p_snapshot)
{ memset(p_snapshot, 0, sizeof(*p_snapshot)); }
void clock_fault_report(clock_fault_source_t source, uint32_t mask)
{ assert(source == CLOCK_FAULT_DEBUG); s_fault_mask = mask; }
void clock_faults_get_snapshot(clock_fault_snapshot_t *p_snapshot)
{
    memset(p_snapshot, 0, sizeof(*p_snapshot));
    p_snapshot->active_categories = 1U;
    p_snapshot->revision = 3U;
    p_snapshot->source[CLOCK_FAULT_RTC].active_mask = CLOCK_FAULT_IO;
    p_snapshot->source[CLOCK_FAULT_RTC].first_mask = CLOCK_FAULT_IO;
    p_snapshot->source[CLOCK_FAULT_RTC].first_tick = 42U;
    p_snapshot->source[CLOCK_FAULT_RTC].time_valid = 1U;
    p_snapshot->source[CLOCK_FAULT_RTC].first_time.year = 2026U;
    p_snapshot->source[CLOCK_FAULT_RTC].first_time.month = 10U;
    p_snapshot->source[CLOCK_FAULT_RTC].first_time.day = 18U;
    p_snapshot->source[CLOCK_FAULT_RTC].first_time.hour = 12U;
    p_snapshot->source[CLOCK_FAULT_RTC].first_time.minute = 34U;
    p_snapshot->source[CLOCK_FAULT_RTC].first_time.second = 56U;
}
int32_t clock_service_request_time(const clock_calendar_t *p_calendar, uint32_t token)
{
    assert(p_calendar != NULL && token != 0U && clock_calendar_valid(p_calendar));
    assert(p_calendar->weekday == clock_calendar_weekday(p_calendar->year,
        p_calendar->month, p_calendar->day));
    s_time_request_calls++;
    if (s_time_accept_result != 0) { return s_time_accept_result; }
    assert(s_time_token == 0U);
    s_time_token = token;
    s_requested_time = *p_calendar;
    s_time_requests++;
    return 0;
}
uint8_t clock_service_take_time_result(uint32_t token, int32_t *p_result)
{
    assert(token != 0U && token == s_time_token && p_result != NULL);
    if (s_time_done == 0U) { return 0U; }
    *p_result = s_time_result;
    s_time_token = 0U;
    s_time_done = 0U;
    s_time_takes++;
    return 1U;
}
epd_gdey029t94_status_t display_get_last_status(void)
{ return (epd_gdey029t94_status_t)-2; }
bool display_publish_weather(const weather_data_t *p_data, uint32_t now_tick)
{
    assert(now_tick == xTaskGetTickCount());
    if (!weather_publish(p_data, now_tick)) { return false; }
    s_selected_page = DISPLAY_PAGE_WEATHER;
    s_last_weather_tick = now_tick;
    s_weather_publications++;
    return true;
}
bool display_select_page(display_page_t page)
{
    if (s_step == 3U) {
        assert(s_storage_requests == 1U);
        assert(s_config.completed_token != s_requested_token);
    }
    s_selected_page = page;
    return true;
}

int main(void)
{
    Debug_Init();
    assert(Debug_IsReady() && !Debug_IsConsoleActive());
    assert(Debug_SendFloats(s_frame, VOFA_MAX_CHANNELS));
    if (setjmp(s_finished) == 0) { StartDebugTask(NULL); }
    puts("PASS: TIME async result ownership, silent default, console errors, FORMAT coexistence and UART recovery");
    return 0;
}
