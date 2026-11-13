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

typedef enum {
    DISPLAY_ACTION_PREVIOUS = 0,
    DISPLAY_ACTION_NEXT,
    DISPLAY_ACTION_HOME,
    DISPLAY_ACTION_TOGGLE_CYCLE
} display_action_t;

typedef struct {
    display_page_t page;
    uint32_t page_revision;
    bool cycle_paused;
    clock_view_model_t clock;
    weather_snapshot_t weather;
    weather_feed_snapshot_t feed;
    uint8_t use_feed_renderer;
    uint8_t alert_index;
    uint16_t text_page;
} display_snapshot_t;

/* 选择 RAM 页面并重新计时，同页也重计；复位默认时钟页。
 * 无 WDATA 为两页各180秒；有 WDATA 为时钟180秒、其他页60秒。
 * 成功选择时确认此前已发布的预警事件；选择后新事件仍可抢屏。 */
bool display_select_page(display_page_t page);
/* Task context only. Atomic RAM update; never draws or waits for the panel.
 * PREVIOUS/NEXT use the normal two/four-page cycle; alerts use the saved page.
 * Navigation acknowledges old alerts, TOGGLE_CYCLE leaves attention untouched. */
bool display_handle_action(display_action_t action);
/* Console-only navigation, strict complete PAGE command. One-based arguments. */
bool display_page_command(const char *p_line);
/* 完整有效天气原子更新缓存和同步年龄，保留页面和轮换期限。
 * now_tick 使用单调 RTOS tick。 */
bool display_publish_weather(const weather_data_t *p_data, uint32_t now_tick);
/* A newly committed forecast records a sync and selects weather after any
 * active automatic alert. Manual navigation overrides that deferred page.
 * Restoring a saved batch must not call this command-side hook. */
void display_forecast_committed(void);
/* 获取一致快照并推进按页面时长配置的轮换/预警停留；
 * 耗时刷新后只切一次，不补跑错过的页面。 */
void display_get_snapshot(uint32_t now_tick, display_snapshot_t *p_snapshot);
/* displayTask calls after a successful panel refresh, including BUSY completion.
 * Starts the matching automatic alert's hold only when its frame is visible. */
void display_frame_completed(const display_snapshot_t *p_snapshot, uint32_t now_tick);

/* Create one task with application-owned stack and control block.
 * Call during RTOS object initialization; no FreeRTOS heap is consumed. */
bool display_start(void);

/* Current refresh status, or the retained error after retries are exhausted. */
epd_gdey029t94_status_t display_get_last_status(void);

#ifdef __cplusplus
}
#endif

#endif
