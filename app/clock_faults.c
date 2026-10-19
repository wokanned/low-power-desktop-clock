#include "clock_faults.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define CLOCK_FAULT_ANCHOR_MAX_AGE_MS 2000U

static clock_fault_snapshot_t s_faults;
static clock_calendar_t s_anchor;
static TickType_t s_anchor_tick;
static uint8_t s_anchor_valid;

void clock_faults_init(void)
{
    memset(&s_faults, 0, sizeof(s_faults));
    memset(&s_anchor, 0, sizeof(s_anchor));
    s_anchor_tick = 0U;
    s_anchor_valid = 0U;
}

void clock_faults_set_clock(const clock_calendar_t *p_calendar, uint8_t valid)
{
    taskENTER_CRITICAL();
    s_anchor_valid = (uint8_t)(valid != 0U && clock_calendar_valid(p_calendar));
    if (s_anchor_valid != 0U) {
        s_anchor = *p_calendar;
        s_anchor_tick = xTaskGetTickCount();
    }
    taskEXIT_CRITICAL();
}

void clock_fault_report(clock_fault_source_t source, uint32_t active_mask)
{
    clock_fault_source_record_t *p_source;
    clock_fault_category_record_t *p_category;
    TickType_t now;
    TickType_t elapsed;
    uint32_t bit;
    uint8_t time_valid;
    if ((unsigned)source >= CLOCK_FAULT_SOURCE_COUNT) { return; }
    taskENTER_CRITICAL();
    now = xTaskGetTickCount();
    p_source = &s_faults.source[source];
    if (p_source->active_mask != active_mask) {
        p_category = &s_faults.category[clock_fault_category(source)];
        bit = 1UL << (uint32_t)source;
        if (active_mask != 0U && p_source->active_mask == 0U) {
            elapsed = now - s_anchor_tick;
            time_valid = (uint8_t)(s_anchor_valid != 0U &&
                elapsed < pdMS_TO_TICKS(CLOCK_FAULT_ANCHOR_MAX_AGE_MS));
            p_source->first_tick = now;
            p_source->first_mask = active_mask;
            p_source->time_valid = time_valid;
            memset(&p_source->first_time, 0, sizeof(p_source->first_time));
            if (time_valid != 0U) {
                p_source->first_time = s_anchor;
                clock_calendar_add_seconds(&p_source->first_time,
                    (uint32_t)(elapsed / configTICK_RATE_HZ));
            }
            if (p_category->source_bits == 0U) {
                p_category->first_tick = now;
                p_category->first_time = p_source->first_time;
                p_category->time_valid = time_valid;
                s_faults.active_categories++;
            }
        }
        p_source->active_mask = active_mask;
        if (active_mask != 0U) { p_category->source_bits |= bit; }
        else {
            p_category->source_bits &= ~bit;
            if (p_category->source_bits == 0U) { s_faults.active_categories--; }
        }
        s_faults.revision++;
    }
    taskEXIT_CRITICAL();
}

void clock_faults_get_snapshot(clock_fault_snapshot_t *p_snapshot)
{
    if (p_snapshot != NULL) {
        taskENTER_CRITICAL();
        *p_snapshot = s_faults;
        taskEXIT_CRITICAL();
    }
}
