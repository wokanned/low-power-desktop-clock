#ifndef CLOCK_SERVICE_H
#define CLOCK_SERVICE_H

#include "clock_model.h"

void clock_service_init(void);
/* Startup fallback when the default task cannot be created. */
void clock_service_mark_unavailable(void);
/* Called by defaultTask once per second. */
void clock_service_poll(void);
/* Coherent task-context read; no IO or formatting. */
void clock_service_get_model(clock_view_model_t *p_model);

#endif
