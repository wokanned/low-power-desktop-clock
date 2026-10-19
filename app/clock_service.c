#include "clock_service.h"
#include "clock_faults.h"
#include "storage.h"
#include "stm32_clock.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define CLOCK_MEMORY_SAMPLE_MS 10000U
#define CLOCK_SRAM_BYTES       131072UL
#define CLOCK_FLASH_BYTES      524288UL

static clock_view_model_t s_model;
static clock_calendar_t s_calendar;
static TickType_t s_memory_tick;
static uint8_t s_memory_sampled;
static uint8_t s_polled;

void clock_service_init(void)
{
    memset(&s_model, 0, sizeof(s_model));
    s_model.calendar_state = CLOCK_DATA_INITIALIZING;
    s_model.format = CLOCK_FORMAT_24;
    s_memory_tick = 0U;
    s_memory_sampled = 0U;
    s_polled = 0U;
    clock_faults_init();
}

void clock_service_poll(void)
{
    int32_t result;
    uint32_t linked_sram;
    uint32_t image_flash;
    uint32_t free_heap;
    TickType_t now;
    clock_field_t sram = {CLOCK_DATA_UNAVAILABLE, 0};
    clock_field_t flash = {CLOCK_DATA_UNAVAILABLE, 0};
    uint8_t sample_memory;

    now = xTaskGetTickCount();
    result = stm32_clock_read_calendar(&s_calendar);
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
    taskEXIT_CRITICAL();
}

void clock_service_mark_unavailable(void)
{
    taskENTER_CRITICAL();
    s_model.calendar_state = CLOCK_DATA_UNAVAILABLE;
    s_polled = 1U;
    taskEXIT_CRITICAL();
}

void clock_service_get_model(clock_view_model_t *p_model)
{
    storage_config_snapshot_t config;
    if (p_model == NULL) { return; }
    taskENTER_CRITICAL();
    *p_model = s_model;
    storage_get_config_snapshot(&config);
    p_model->format = config.format;
    p_model->format_revision = config.revision;
    p_model->startup_ready = (uint8_t)(s_polled != 0U && config.ready != 0U);
    clock_faults_get_snapshot(&p_model->faults);
    taskEXIT_CRITICAL();
}
