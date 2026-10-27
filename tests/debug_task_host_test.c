#include "debug.h"
#include "clock_faults.h"
#include "display.h"
#include "storage.h"
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
static char s_boundary_input[256];

static void make_boundary_input(void)
{
    static const char s_before[] = "FORMAT 12\nERRORS\n";
    static const char s_prefix[] = "WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,";
    static const char s_suffix[] = "2,235,180,270,65,2,35,2026-10-26T10:30";
    static const char s_after[] = "\nPAGE WEATHER\n";
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
        s_input = "ERRORS\n";
        break;
    case 6U:
        assert(strstr(s_output, "ERRORS 1 REV 3 EPD -2\r\n") != NULL);
        assert(strstr(s_output, "RTC active=001 first=001 2026-10-18 12:34:56 tick=42") != NULL);
        s_input = "FORMAT 24\n";
        break;
    case 7U:
        assert(s_storage_requests == 2U && strstr(s_output, "OK FORMAT 24") == NULL);
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
        assert(s_binary_frames == 2U);
        assert(Debug_GetQueueDropCount() == 0U);
        longjmp(s_finished, 1);
    }
    return 1U;
}

bool stm32_debug_uart_rx_start(stm32_debug_uart_notify_t notify)
{ assert(notify != NULL); return true; }
bool stm32_debug_uart_rx_recover(void) { return true; }
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
const char *clock_fault_source_label(clock_fault_source_t source)
{ assert(source == CLOCK_FAULT_RTC); return "RTC"; }
epd_gdey029t94_status_t display_get_last_status(void)
{ return (epd_gdey029t94_status_t)-2; }
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
    puts("PASS: binary/text ownership, valid 127-byte weather, WEATHER/PAGE during FORMAT, persistence and recovery");
    return 0;
}
