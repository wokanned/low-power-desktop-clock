#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include "../device/epd_gdey029t94.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create one task with application-owned stack and control block.
 * Call during RTOS object initialization; no FreeRTOS heap is consumed. */
bool display_start(void);

/* Current refresh status, or the retained error after retries are exhausted. */
epd_gdey029t94_status_t display_get_last_status(void);

#ifdef __cplusplus
}
#endif

#endif
