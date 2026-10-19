#include "clock_schedule.h"
#include <string.h>

static uint32_t s_minute_key(const clock_calendar_t *p_calendar)
{
    return ((uint32_t)(p_calendar->year - 2000U) << 20) |
        ((uint32_t)p_calendar->month << 16) | ((uint32_t)p_calendar->day << 11) |
        ((uint32_t)p_calendar->hour << 6) | p_calendar->minute;
}

void clock_schedule_init(clock_schedule_t *p_schedule)
{
    if (p_schedule != NULL) { memset(p_schedule, 0, sizeof(*p_schedule)); }
}

uint8_t clock_schedule_due(const clock_schedule_t *p_schedule,
                          const clock_view_model_t *p_model, uint32_t now_ms)
{
    uint8_t ready;
    (void)now_ms;
    if (p_schedule == NULL || p_model == NULL || p_model->startup_ready == 0U) { return 0U; }
    ready = (uint8_t)(p_model->calendar_state == CLOCK_DATA_READY);
    if (p_schedule->has_frame == 0U ||
        p_schedule->fault_revision != p_model->faults.revision ||
        p_schedule->format_revision != p_model->format_revision ||
        p_schedule->calendar_ready != ready) { return 1U; }
    if (ready != 0U) { return (uint8_t)(p_schedule->minute_key != s_minute_key(&p_model->calendar)); }
    /* An invalid clock cannot change the header; wait for an actual change. */
    return 0U;
}

void clock_schedule_commit(clock_schedule_t *p_schedule,
                           const clock_view_model_t *p_model, uint32_t now_ms)
{
    if (p_schedule == NULL || p_model == NULL) { return; }
    p_schedule->minute_key = s_minute_key(&p_model->calendar);
    p_schedule->fault_revision = p_model->faults.revision;
    p_schedule->format_revision = p_model->format_revision;
    p_schedule->calendar_ready = (uint8_t)(p_model->calendar_state == CLOCK_DATA_READY);
    p_schedule->completed_ms = now_ms;
    p_schedule->has_frame = 1U;
    if (p_model->faults.active_categories == 0U) {
        p_schedule->next_memory = (clock_memory_mode_t)
            (((uint32_t)p_model->memory_mode + 1U) % CLOCK_MEMORY_COUNT);
    }
}
