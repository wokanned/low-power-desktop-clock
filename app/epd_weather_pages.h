#ifndef EPD_WEATHER_PAGES_H
#define EPD_WEATHER_PAGES_H

#include "weather_feed.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WEATHER_PAGE_LIVE = 0, WEATHER_PAGE_FORECAST, WEATHER_PAGE_DATA,
    WEATHER_PAGE_ALERT, WEATHER_PAGE_TEXT, WEATHER_PAGE_SOURCE
} weather_page_kind_t;

/* Pure native 296x128 rasterizers. Caller pins p_batch while drawing; release
 * before waiting on SPI/BUSY. No global Paint, RTOS, heap or controller IO.
 * text_page indexes body pages, guidance pages, then source pages. */
uint8_t epd_weather_pages_draw_bw(uint8_t *p_frame, size_t length,
    const weather_feed_batch_t *p_batch, weather_page_kind_t page,
    uint8_t alert_index, uint16_t text_page, uint8_t fresh);
uint8_t epd_weather_pages_draw_gray4(uint8_t *p_frame, size_t length,
    const weather_feed_batch_t *p_batch, weather_page_kind_t page,
    uint8_t alert_index, uint16_t text_page, uint8_t fresh);
/* Date-aware forms select RTC today/next two days. A NULL/untrusted date uses
 * batch dates with weekday labels; it never claims that day[0] is TODAY. */
uint8_t epd_weather_pages_draw_bw_for_date(uint8_t *p_frame, size_t length,
    const weather_feed_batch_t *p_batch, weather_page_kind_t page,
    uint8_t alert_index, uint16_t text_page, uint8_t fresh,
    const clock_calendar_t *p_today);
uint8_t epd_weather_pages_draw_gray4_for_date(uint8_t *p_frame, size_t length,
    const weather_feed_batch_t *p_batch, weather_page_kind_t page,
    uint8_t alert_index, uint16_t text_page, uint8_t fresh,
    const clock_calendar_t *p_today);
uint16_t epd_weather_pages_text_count(const weather_feed_batch_t *p_batch,
    uint8_t alert_index);

#endif
