#ifndef CLOCK_SERVICE_H
#define CLOCK_SERVICE_H

#include "clock_model.h"

void clock_service_init(void);
/* Startup fallback when the default task cannot be created. */
void clock_service_mark_unavailable(void);
/* Runs on defaultTask; owns RTC IO and wakes for time requests or the 1s poll. */
void clock_service_run(void);
/* One owner-task iteration; also available to host tests. */
void clock_service_poll(void);
/* One task-context request at a time, retained until its result is consumed.
 * Returns 0 accepted, -1 busy, -2 owner unavailable, or -3 invalid calendar.
 * Weekday is calculated from the date. The result token must be nonzero. */
int32_t clock_service_request_time(const clock_calendar_t *p_calendar,
                                   uint32_t token);
/* Returns 1 and releases a matching completed request; otherwise returns 0.
 * The result is 0 only after the platform write and verified readback succeed. */
uint8_t clock_service_take_time_result(uint32_t token, int32_t *p_result);
/* Coherent task-context read; no IO or formatting. */
void clock_service_get_model(clock_view_model_t *p_model);

#endif
