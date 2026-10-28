#include "clock_service.h"
#include "clock_faults.h"
#include "storage.h"
#include "storage_files.h"
#include "stm32_clock.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define CLOCK_MEMORY_SAMPLE_MS 10000U
#define CLOCK_SRAM_BYTES       131072UL
#define CLOCK_FLASH_BYTES      524288UL
#define CLOCK_POLL_MS          1000U
#define CLOCK_TICK_HALF_RANGE  ((TickType_t)(UINT32_MAX / 2U + 1U))

typedef enum {
    CLOCK_TIME_IDLE = 0,
    CLOCK_TIME_PENDING,
    CLOCK_TIME_APPLYING,
    CLOCK_TIME_COMPLETE
} clock_time_state_t;

typedef struct {
    clock_calendar_t calendar;
    uint32_t token;
    int32_t result;
    clock_time_state_t state;
} clock_time_request_t;

static clock_view_model_t s_model;
static clock_calendar_t s_calendar;
static TickType_t s_memory_tick;
static uint8_t s_memory_sampled;
static uint8_t s_polled;
static uint8_t s_rtc_trust_lost;
static TaskHandle_t s_owner_task;
static clock_time_request_t s_time_request;

void clock_service_init(void)
{
    memset(&s_model, 0, sizeof(s_model));
    s_model.calendar_state = CLOCK_DATA_INITIALIZING;
    s_model.format = CLOCK_FORMAT_24;
    s_memory_tick = 0U;
    s_memory_sampled = 0U;
    s_polled = 0U;
    s_rtc_trust_lost = 0U;
    s_owner_task = NULL;
    memset(&s_time_request, 0, sizeof(s_time_request));
    clock_faults_init();
}

int32_t clock_service_request_time(const clock_calendar_t *p_calendar,
                                   uint32_t token)
{
    clock_calendar_t calendar;
    TaskHandle_t owner;
    int32_t result = 0;
    if (p_calendar == NULL || token == 0U) { return -3; }
    calendar = *p_calendar;
    calendar.weekday = clock_calendar_weekday(calendar.year, calendar.month,
                                               calendar.day);
    if (!clock_calendar_valid(&calendar)) { return -3; }
    taskENTER_CRITICAL();
    owner = s_owner_task;
    if (owner == NULL) { result = -2; }
    else if (s_time_request.state != CLOCK_TIME_IDLE) { result = -1; }
    else {
        s_time_request.calendar = calendar;
        s_time_request.token = token;
        s_time_request.state = CLOCK_TIME_PENDING;
    }
    taskEXIT_CRITICAL();
    if (result == 0) { (void)xTaskNotifyGive(owner); }
    return result;
}

uint8_t clock_service_take_time_result(uint32_t token, int32_t *p_result)
{
    uint8_t complete = 0U;
    if (p_result == NULL || token == 0U) { return 0U; }
    taskENTER_CRITICAL();
    if (s_time_request.state == CLOCK_TIME_COMPLETE &&
        s_time_request.token == token) {
        *p_result = s_time_request.result;
        s_time_request.state = CLOCK_TIME_IDLE;
        complete = 1U;
    }
    taskEXIT_CRITICAL();
    return complete;
}

void clock_service_run(void)
{
    const TickType_t period = pdMS_TO_TICKS(CLOCK_POLL_MS);
    TickType_t next_poll = xTaskGetTickCount();
    TickType_t now;
    TickType_t overdue;
    taskENTER_CRITICAL();
    s_owner_task = xTaskGetCurrentTaskHandle();
    taskEXIT_CRITICAL();
    for (;;) {
        clock_service_poll();
        now = xTaskGetTickCount();
        overdue = now - next_poll;
        if (overdue < CLOCK_TICK_HALF_RANGE) {
            /* Keep the periodic phase on request wakes and skip missed periods. */
            next_poll += (overdue / period + 1U) * period;
        }
        /* A notification before this call remains pending, so no wake is lost. */
        (void)ulTaskNotifyTake(pdTRUE, (TickType_t)(next_poll - now));
    }
}

void clock_service_poll(void)
{
    int32_t result;
    int32_t write_result = 0;
    uint32_t linked_sram;
    uint32_t image_flash;
    uint32_t free_heap;
    TickType_t now;
    clock_field_t sram = {CLOCK_DATA_UNAVAILABLE, 0};
    clock_field_t flash = {CLOCK_DATA_UNAVAILABLE, 0};
    uint8_t sample_memory;
    uint8_t apply_time;
    clock_calendar_t requested;

    taskENTER_CRITICAL();
    apply_time = (uint8_t)(s_time_request.state == CLOCK_TIME_PENDING);
    if (apply_time != 0U) {
        requested = s_time_request.calendar;
        s_time_request.state = CLOCK_TIME_APPLYING;
    }
    taskEXIT_CRITICAL();
    if (apply_time != 0U) {
        write_result = stm32_clock_write_calendar(&requested, &s_calendar);
        if (write_result == 0 && !clock_calendar_valid(&s_calendar)) {
            write_result = -2;
        }
        s_rtc_trust_lost = (uint8_t)(write_result != 0);
        result = write_result;
    } else {
        result = stm32_clock_read_calendar(&s_calendar);
    }
    /* A plausible read after a failed write does not restore RTC trust. */
    if (s_rtc_trust_lost != 0U) { result = -1; }
    now = xTaskGetTickCount();
    sample_memory = (uint8_t)(s_memory_sampled == 0U ||
        (TickType_t)(now - s_memory_tick) >= pdMS_TO_TICKS(CLOCK_MEMORY_SAMPLE_MS));
    if (sample_memory != 0U) {
        if (stm32_clock_image_sizes(&linked_sram, &image_flash) == 0) {
            free_heap = (uint32_t)xPortGetFreeHeapSize();
            if (free_heap <= linked_sram && linked_sram <= CLOCK_SRAM_BYTES) {
                sram.state = CLOCK_DATA_READY;
                sram.value = (int32_t)(((linked_sram - free_heap) * 100UL +
                    CLOCK_SRAM_BYTES / 2U) / CLOCK_SRAM_BYTES);
            }
            if (image_flash <= CLOCK_FLASH_BYTES) {
                flash.state = CLOCK_DATA_READY;
                flash.value = (int32_t)((image_flash * 100UL + CLOCK_FLASH_BYTES / 2U) /
                    CLOCK_FLASH_BYTES);
            }
        }
        s_memory_tick = now;
        s_memory_sampled = 1U;
    }
    taskENTER_CRITICAL();
    clock_faults_set_clock(&s_calendar, (uint8_t)(result == 0));
    clock_fault_report(CLOCK_FAULT_RTC, result == 0 ? 0U :
        (result == -2 ? CLOCK_FAULT_CONFIGURATION : CLOCK_FAULT_IO));
    if (result == 0) { s_model.calendar = s_calendar; }
    s_model.calendar_state = result == 0 ? CLOCK_DATA_READY : CLOCK_DATA_ERROR;
    if (sample_memory != 0U) {
        s_model.memory[CLOCK_MEMORY_SRAM] = sram;
        s_model.memory[CLOCK_MEMORY_IFLASH] = flash;
    }
    s_polled = 1U;
    if (apply_time != 0U) {
        /* Publish completion with the matching calendar/fault model. */
        s_time_request.result = write_result;
        s_time_request.state = CLOCK_TIME_COMPLETE;
    }
    taskEXIT_CRITICAL();
}

void clock_service_mark_unavailable(void)
{
    taskENTER_CRITICAL();
    s_owner_task = NULL;
    if (s_time_request.state == CLOCK_TIME_PENDING) {
        s_time_request.result = -1;
        s_time_request.state = CLOCK_TIME_COMPLETE;
    }
    s_model.calendar_state = CLOCK_DATA_UNAVAILABLE;
    s_polled = 1U;
    taskEXIT_CRITICAL();
}

void clock_service_get_model(clock_view_model_t *p_model)
{
    storage_config_snapshot_t config;
    file_store_snapshot_t usage;
    if (p_model == NULL) { return; }
    taskENTER_CRITICAL();
    *p_model = s_model;
    storage_get_config_snapshot(&config);
    storage_get_files_snapshot(&usage);
    p_model->format = config.format;
    p_model->format_revision = config.revision;
    p_model->startup_ready = (uint8_t)(s_polled != 0U && config.ready != 0U);
    p_model->memory[CLOCK_MEMORY_W25Q].state =
        usage.state == FILE_STORE_READY ? CLOCK_DATA_READY :
        (usage.state == FILE_STORE_ERROR ? CLOCK_DATA_ERROR : CLOCK_DATA_UNAVAILABLE);
    p_model->memory[CLOCK_MEMORY_W25Q].value = 0;
    if (usage.state == FILE_STORE_READY) {
        if (usage.total_blocks == FILE_STORE_BLOCK_COUNT &&
            usage.used_blocks <= usage.total_blocks) {
            p_model->memory[CLOCK_MEMORY_W25Q].value = (int32_t)(
                (usage.used_blocks * 100UL + usage.total_blocks - 1UL) /
                usage.total_blocks);
        } else {
            p_model->memory[CLOCK_MEMORY_W25Q].state = CLOCK_DATA_ERROR;
        }
    }
    clock_faults_get_snapshot(&p_model->faults);
    taskEXIT_CRITICAL();
}
