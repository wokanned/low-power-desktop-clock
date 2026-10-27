#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include "clock_model.h"
#include "weather.h"
#include "../device/epd_gdey029t94.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_PAGE_CLOCK = 0,
    DISPLAY_PAGE_WEATHER
} display_page_t;

typedef struct {
    display_page_t page;
    uint32_t page_revision;
    clock_view_model_t clock;
    weather_snapshot_t weather;
} display_snapshot_t;

/* Select the RAM-only view; reset always starts on the clock page. */
bool display_select_page(display_page_t page);
void display_get_snapshot(uint32_t now_tick, display_snapshot_t *p_snapshot);

/* Create one task with application-owned stack and control block.
 * Call during RTOS object initialization; no FreeRTOS heap is consumed. */
bool display_start(void);

/* Current refresh status, or the retained error after retries are exhausted. */
epd_gdey029t94_status_t display_get_last_status(void);

#ifdef __cplusplus
}
#endif

#endif
