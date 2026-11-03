#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include "clock_model.h"
#include "weather.h"
#include "weather_feed.h"
#include "../device/epd_gdey029t94.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_PAGE_CLOCK = 0,
    DISPLAY_PAGE_WEATHER, DISPLAY_PAGE_FORECAST, DISPLAY_PAGE_DATA,
    DISPLAY_PAGE_ALERT, DISPLAY_PAGE_TEXT, DISPLAY_PAGE_SOURCE
} display_page_t;

typedef struct {
    display_page_t page;
    uint32_t page_revision;
    clock_view_model_t clock;
    weather_snapshot_t weather;
    weather_feed_snapshot_t feed;
    uint8_t alert_index;
    uint16_t text_page;
} display_snapshot_t;

/* 选择 RAM 页面并重新计时；同页命令也重置三分钟轮换，复位默认时钟页。 */
bool display_select_page(display_page_t page);
/* Console-only navigation, strict complete PAGE command. One-based arguments. */
bool display_page_command(const char *p_line);
/* 完整有效天气原子更新缓存、选择天气页并重新计时，重复包也重置。
 * 失败不改变缓存、页面或计时；now_tick 使用单调 RTOS tick。 */
bool display_publish_weather(const weather_data_t *p_data, uint32_t now_tick);
/* A newly committed forecast selects weather and records a sync. Restoring a
 * saved batch must publish it without calling this command-side hook. */
void display_forecast_committed(void);
/* 获取一致快照并推进三分钟轮换，耗时刷新后只切一次，不补跑错过的页面。 */
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
