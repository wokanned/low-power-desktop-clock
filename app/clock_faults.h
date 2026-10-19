#ifndef CLOCK_FAULTS_H
#define CLOCK_FAULTS_H

#include "clock_model.h"

/* Initialize before starting producers. All other functions are task-context. */
void clock_faults_init(void);
void clock_faults_set_clock(const clock_calendar_t *p_calendar, uint8_t valid);
/* Repeated masks preserve the first timestamp; zero clears the active episode. */
void clock_fault_report(clock_fault_source_t source, uint32_t active_mask);
void clock_faults_get_snapshot(clock_fault_snapshot_t *p_snapshot);

#endif
