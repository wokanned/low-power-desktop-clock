#include "debug.h"
#include "debug_console.h"
#include "file_transfer.h"
#include "weather.h"
#include "clock_faults.h"
#include "clock_service.h"
#include "display.h"
#include "storage.h"
#include "stm32_debug_uart.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define DEBUG_QUEUE_DEPTH 8U
#define DEBUG_COUNTER_MAX 0x00FFFFFFU
#define DEBUG_POLL_MS 20U
#define DEBUG_TEXT_CHUNK 64U
#define DEBUG_TEXT_SIZE 128U

static VOFA_HandleTypeDef s_hvofa;
static QueueHandle_t s_debug_queue;
static StaticQueue_t s_queue_control;
static uint8_t s_queue_storage[DEBUG_QUEUE_DEPTH * VOFA_MAX_CHANNELS * sizeof(float)];
static volatile uint32_t s_queue_drop_count;
static volatile uint32_t s_uart_error_count;
static volatile uint32_t s_suppressed_count;
static volatile uint8_t s_console_active;
static TaskHandle_t s_debug_task;
static debug_console_parser_t s_parser;
static clock_fault_snapshot_t s_fault_snapshot;
static storage_config_snapshot_t s_config_snapshot;
static char s_text[DEBUG_TEXT_SIZE];
static uint32_t s_debug_fault_mask;
static uint32_t s_next_token;
static uint32_t s_pending_token;
static clock_format_t s_pending_format;
static uint32_t s_pending_time_token;
static uint8_t s_pending_time_reply;

static void debug_fault_update(uint32_t set_mask, uint32_t clear_mask)
{
    s_debug_fault_mask = (s_debug_fault_mask & ~clear_mask) | set_mask;
    clock_fault_report(CLOCK_FAULT_DEBUG, s_debug_fault_mask);
}

static void debug_uart_error(void)
{
    if (s_uart_error_count < DEBUG_COUNTER_MAX) { s_uart_error_count++; }
}

static void debug_notify_rx(void)
{
    BaseType_t higher_priority = pdFALSE;
    if (s_debug_task != NULL) {
        vTaskNotifyGiveFromISR(s_debug_task, &higher_priority);
        portYIELD_FROM_ISR(higher_priority);
    }
}

static uint8_t debug_send_bytes(const uint8_t *p_data, uint16_t length)
{
    uint16_t chunk;
    while (length != 0U) {
        chunk = length > DEBUG_TEXT_CHUNK ? DEBUG_TEXT_CHUNK : length;
        if (!stm32_debug_uart_send(NULL, p_data, chunk)) {
            debug_uart_error();
            debug_fault_update(CLOCK_FAULT_TX, 0U);
            return 0U;
        }
        p_data += chunk;
        length = (uint16_t)(length - chunk);
    }
    debug_fault_update(0U, CLOCK_FAULT_TX);
    return 1U;
}

static bool debug_send_text(const char *p_text)
{
    return debug_send_bytes((const uint8_t *)p_text, (uint16_t)strlen(p_text)) != 0U;
}

static void debug_print_errors(void)
{
    const clock_fault_source_record_t *p_record;
    const clock_calendar_t *p_time;
    uint8_t i;
    clock_faults_get_snapshot(&s_fault_snapshot);
    (void)snprintf(s_text, sizeof(s_text), "ERRORS %u REV %lu EPD %ld\r\n",
        (unsigned int)s_fault_snapshot.active_categories,
        (unsigned long)s_fault_snapshot.revision,
        (long)display_get_last_status());
    if (!debug_send_text(s_text)) { return; }
    for (i = 0U; i < CLOCK_FAULT_SOURCE_COUNT; i++) {
        p_record = &s_fault_snapshot.source[i];
        /* Cleared records retain the most recent episode for diagnostics. */
        if (p_record->first_mask == 0U) { continue; }
        p_time = &p_record->first_time;
        if (p_record->time_valid != 0U) {
            (void)snprintf(s_text, sizeof(s_text),
                "%s active=%03lX first=%03lX %04u-%02u-%02u %02u:%02u:%02u tick=%lu\r\n",
                clock_fault_source_label((clock_fault_source_t)i),
                (unsigned long)p_record->active_mask,
                (unsigned long)p_record->first_mask,
                (unsigned int)p_time->year, (unsigned int)p_time->month,
                (unsigned int)p_time->day, (unsigned int)p_time->hour,
                (unsigned int)p_time->minute, (unsigned int)p_time->second,
                (unsigned long)p_record->first_tick);
        } else {
            (void)snprintf(s_text, sizeof(s_text),
                "%s active=%03lX first=%03lX --:-- tick=%lu\r\n",
                clock_fault_source_label((clock_fault_source_t)i),
                (unsigned long)p_record->active_mask,
                (unsigned long)p_record->first_mask,
                (unsigned long)p_record->first_tick);
        }
        if (!debug_send_text(s_text)) { return; }
    }
}

static void debug_command(debug_command_t command)
{
    clock_format_t format;
    weather_data_t weather;
    clock_calendar_t calendar;
    int32_t result;
    if (command == DEBUG_COMMAND_NONE) { return; }
    if (command == DEBUG_COMMAND_TIME) {
        if (!debug_console_parse_time(s_parser.line, &calendar)) {
            if (s_console_active != 0U) { (void)debug_send_text("ERR TIME\r\n"); }
            return;
        }
        if (s_pending_time_token != 0U) {
            if (s_console_active != 0U) { (void)debug_send_text("ERR BUSY\r\n"); }
            return;
        }
        s_next_token++;
        if (s_next_token == 0U) { s_next_token++; }
        result = clock_service_request_time(&calendar, s_next_token);
        if (result != 0) {
            if (s_console_active != 0U) {
                (void)debug_send_text(result == -1 ? "ERR BUSY\r\n" :
                    (result == -3 ? "ERR TIME\r\n" : "ERR RTC\r\n"));
            }
            return;
        }
        s_pending_time_token = s_next_token;
        s_pending_time_reply = s_console_active;
        debug_fault_update(0U, CLOCK_FAULT_RX | CLOCK_FAULT_OVERFLOW);
        return;
    }
    if (command == DEBUG_COMMAND_WEATHER) {
        if (!weather_parse_line(s_parser.line, &weather)) {
            if (s_console_active != 0U) { (void)debug_send_text("ERR WEATHER\r\n"); }
            return;
        }
        if (!weather_publish(&weather, (uint32_t)xTaskGetTickCount())) {
            if (s_console_active != 0U) { (void)debug_send_text("ERR WEATHER\r\n"); }
            return;
        }
        debug_fault_update(0U, CLOCK_FAULT_RX | CLOCK_FAULT_OVERFLOW);
        if (s_console_active != 0U) { (void)debug_send_text("OK WEATHER\r\n"); }
        return;
    }
    if ((command == DEBUG_COMMAND_PAGE_WEATHER) ||
        (command == DEBUG_COMMAND_PAGE_CLOCK)) {
        display_page_t page = command == DEBUG_COMMAND_PAGE_WEATHER ?
            DISPLAY_PAGE_WEATHER : DISPLAY_PAGE_CLOCK;
        (void)display_select_page(page);
        debug_fault_update(0U, CLOCK_FAULT_RX | CLOCK_FAULT_OVERFLOW);
        if (s_console_active != 0U) {
            (void)debug_send_text(command == DEBUG_COMMAND_PAGE_WEATHER ?
                                  "OK PAGE WEATHER\r\n" : "OK PAGE CLOCK\r\n");
        }
        return;
    }
    if (s_console_active == 0U) {
        if (command != DEBUG_COMMAND_ON) { return; }
        /* This task parses only between complete blocking TX operations. */
        taskENTER_CRITICAL();
        s_console_active = 1U;
        (void)xQueueReset(s_debug_queue);
        taskEXIT_CRITICAL();
        debug_fault_update(0U, CLOCK_FAULT_RX | CLOCK_FAULT_OVERFLOW);
        (void)debug_send_text("OK CONSOLE ON\r\n");
        return;
    }
    if (command == DEBUG_COMMAND_RX_ERROR || command == DEBUG_COMMAND_RX_OVERFLOW) {
        debug_uart_error();
        debug_fault_update(command == DEBUG_COMMAND_RX_OVERFLOW ?
                           CLOCK_FAULT_OVERFLOW : CLOCK_FAULT_RX, 0U);
        (void)debug_send_text("ERR RX\r\n");
        return;
    }
    if (command == DEBUG_COMMAND_UNKNOWN) {
        (void)debug_send_text("ERR COMMAND\r\n");
        return;
    }
    debug_fault_update(0U, CLOCK_FAULT_RX | CLOCK_FAULT_OVERFLOW);
    if (command == DEBUG_COMMAND_ERRORS) { debug_print_errors(); return; }
    if (command == DEBUG_COMMAND_ON) {
        (void)debug_send_text("OK CONSOLE ON\r\n");
        return;
    }
    if (command == DEBUG_COMMAND_FILES) {
        if (strcmp(s_parser.line, "FS TRANSFER") == 0 &&
            (s_pending_token != 0U || s_pending_time_token != 0U)) {
            (void)debug_send_text("ERR BUSY\r\n");
            return;
        }
        (void)file_transfer_console(s_parser.line, (uint32_t)xTaskGetTickCount());
        stm32_debug_uart_set_binary(file_transfer_active());
        return;
    }
    if (s_pending_token != 0U) {
        (void)debug_send_text("ERR BUSY\r\n");
        return;
    }
    if (command == DEBUG_COMMAND_OFF) {
        if (s_pending_time_token != 0U || file_transfer_busy() != 0U) {
            (void)debug_send_text("ERR BUSY\r\n");
            return;
        }
        if (debug_send_text("OK CONSOLE OFF\r\n")) {
            taskENTER_CRITICAL();
            (void)xQueueReset(s_debug_queue);
            s_console_active = 0U;
            taskEXIT_CRITICAL();
        }
        return;
    }
    format = command == DEBUG_COMMAND_FORMAT_12 ? CLOCK_FORMAT_12 : CLOCK_FORMAT_24;
    s_next_token++;
    if (s_next_token == 0U) { s_next_token++; }
    if (storage_request_format(format, s_next_token) != 0) {
        (void)debug_send_text("ERR STORAGE\r\n");
        return;
    }
    s_pending_token = s_next_token;
    s_pending_format = format;
}

static void debug_poll_time(void)
{
    int32_t result;
    uint8_t reply;
    if (s_pending_time_token == 0U ||
        clock_service_take_time_result(s_pending_time_token, &result) == 0U) {
        return;
    }
    reply = s_pending_time_reply;
    s_pending_time_token = 0U;
    s_pending_time_reply = 0U;
    if (reply != 0U) {
        (void)debug_send_text(result == 0 ? "OK TIME\r\n" : "ERR RTC\r\n");
    }
}

static void debug_poll_config(void)
{
    if (s_pending_token == 0U) { return; }
    storage_get_config_snapshot(&s_config_snapshot);
    if (s_config_snapshot.completed_token != s_pending_token) { return; }
    s_pending_token = 0U;
    if (s_config_snapshot.completed_result != 0) {
        (void)debug_send_text("ERR STORAGE\r\n");
    } else {
        (void)snprintf(s_text, sizeof(s_text), "OK FORMAT %u\r\n",
                       (unsigned int)s_pending_format);
        (void)debug_send_text(s_text);
    }
}

void Debug_Init(void)
{
    VOFA_Init(&s_hvofa, NULL, stm32_debug_uart_send,
              VOFA_FMT_JUSTFLOAT, VOFA_BAUD_115200);
    s_queue_drop_count = 0U;
    s_uart_error_count = 0U;
    s_suppressed_count = 0U;
    s_console_active = 0U;
    s_debug_task = NULL;
    s_debug_fault_mask = 0U;
    s_next_token = 0U;
    s_pending_token = 0U;
    s_pending_time_token = 0U;
    s_pending_time_reply = 0U;
    debug_console_parser_reset(&s_parser);
    file_transfer_init(debug_send_bytes);
    weather_init();
    s_debug_queue = xQueueCreateStatic(DEBUG_QUEUE_DEPTH,
        VOFA_MAX_CHANNELS * sizeof(float), s_queue_storage, &s_queue_control);
}

bool Debug_IsReady(void)
{
    return s_debug_queue != NULL;
}

void Debug_Log(const char *p_msg)
{
    (void)p_msg;
}

bool Debug_SendFloats(const float *p_data, uint8_t ch_cnt)
{
    BaseType_t result;
    if (p_data == NULL || ch_cnt != VOFA_MAX_CHANNELS || s_debug_queue == NULL) {
        return false;
    }
    taskENTER_CRITICAL();
    if (s_console_active != 0U) {
        if (s_suppressed_count < DEBUG_COUNTER_MAX) { s_suppressed_count++; }
        taskEXIT_CRITICAL();
        return true;
    }
    result = xQueueSend(s_debug_queue, p_data, 0U);
    if (result != pdPASS) {
        if (s_queue_drop_count < DEBUG_COUNTER_MAX) {
            s_queue_drop_count++;
        }
    }
    taskEXIT_CRITICAL();
    if (result == pdPASS && s_debug_task != NULL) { xTaskNotifyGive(s_debug_task); }
    return result == pdPASS;
}

bool Debug_SendFrame(const void *p_data, uint8_t ch_cnt,
                     const VOFA_Data_TypeDef_Enum *p_types)
{
    uint8_t i;
    if (p_types == NULL || ch_cnt != VOFA_MAX_CHANNELS) {
        return false;
    }
    for (i = 0U; i < ch_cnt; i++) {
        if (p_types[i] != VOFA_DATA_FLOAT) {
            return false;
        }
    }
    return Debug_SendFloats((const float *)p_data, ch_cnt);
}

void StartDebugTask(void *p_argument)
{
    float frame[VOFA_MAX_CHANNELS];
    VOFA_Data_TypeDef_Enum types[VOFA_MAX_CHANNELS];
    uint8_t i;
    uint8_t byte;
    uint16_t consumed;
    uint32_t errors;
    bool available;
    (void)p_argument;
    for (i = 0U; i < VOFA_MAX_CHANNELS; i++) {
        types[i] = VOFA_DATA_FLOAT;
    }
    if (!Debug_IsReady()) {
        vTaskSuspend(NULL);
    }
    s_debug_task = xTaskGetCurrentTaskHandle();
    if (!stm32_debug_uart_rx_start(debug_notify_rx)) {
        debug_uart_error();
        debug_fault_update(CLOCK_FAULT_RX, 0U);
    }
    for (;;) {
        if (!stm32_debug_uart_rx_recover()) {
            debug_fault_update(CLOCK_FAULT_RX, 0U);
        }
        /* Bound command work so a continuous RX stream cannot starve telemetry. */
        for (consumed = 0U; consumed < 256U; consumed++) {
            available = stm32_debug_uart_receive(&byte, &errors);
            if (errors != 0U) {
                debug_console_parser_reset(&s_parser);
                debug_uart_error();
                debug_fault_update(
                    ((errors & STM32_DEBUG_UART_RX_ERROR) != 0U ? CLOCK_FAULT_RX : 0U) |
                    ((errors & STM32_DEBUG_UART_RX_OVERFLOW) != 0U ? CLOCK_FAULT_OVERFLOW : 0U),
                    0U);
                if (file_transfer_active() != 0U) { file_transfer_rx_error(); }
                else if (s_console_active != 0U) { (void)debug_send_text("ERR RX\r\n"); }
            }
            if (!available) { break; }
            if (file_transfer_active() != 0U) {
                file_transfer_feed(byte, (uint32_t)xTaskGetTickCount());
            } else { debug_command(debug_console_parser_feed(&s_parser, byte)); }
        }
        file_transfer_poll((uint32_t)xTaskGetTickCount());
        stm32_debug_uart_set_binary(file_transfer_active());
        (void)weather_poll_expiry((uint32_t)xTaskGetTickCount());
        debug_poll_config();
        debug_poll_time();
        if (s_console_active == 0U &&
            xQueueReceive(s_debug_queue, frame, 0U) == pdPASS) {
            /* Capture current transmit counters, not their old queued values. */
            frame[13] = (float)s_queue_drop_count;
            frame[14] = (float)s_uart_error_count;
            if (!VOFA_SendData(&s_hvofa, frame, VOFA_MAX_CHANNELS, types)) {
                debug_uart_error();
                debug_fault_update(CLOCK_FAULT_TX, 0U);
            } else {
                debug_fault_update(0U, CLOCK_FAULT_TX);
            }
            continue;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DEBUG_POLL_MS));
    }
}

uint32_t Debug_GetQueueDropCount(void) { return s_queue_drop_count; }
uint32_t Debug_GetUartErrorCount(void) { return s_uart_error_count; }
uint32_t Debug_GetSuppressedCount(void) { return s_suppressed_count; }
bool Debug_IsConsoleActive(void) { return s_console_active != 0U; }
