#ifndef CLOCK_SCHEDULE_H
#define CLOCK_SCHEDULE_H

#include "clock_model.h"

typedef struct {
    uint32_t minute_key;
    uint32_t fault_revision;
    uint32_t format_revision;
    uint32_t completed_ms;
    clock_memory_mode_t next_memory;
    uint8_t has_frame;
    uint8_t calendar_ready;
} clock_schedule_t;

void clock_schedule_init(clock_schedule_t *p_schedule);
uint8_t clock_schedule_due(const clock_schedule_t *p_schedule,
                          const clock_view_model_t *p_model, uint32_t now_ms);
void clock_schedule_commit(clock_schedule_t *p_schedule,
                           const clock_view_model_t *p_model, uint32_t now_ms);

#endif
