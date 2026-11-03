#include "../app/epd_weather_pages.h"
#include "../app/epd_clock_view.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static weather_feed_batch_t s_batch;
static uint8_t s_bw[EPD_CLOCK_VIEW_BW_BYTES + 16U];
static uint8_t s_gray[EPD_CLOCK_VIEW_GRAY4_BYTES + 16U];
static uint8_t s_copy[EPD_CLOCK_VIEW_BW_BYTES];
static uint32_t s_checks;
static const clock_calendar_t s_today = {2026U, 11U, 2U, 1U, 0U, 0U, 0U};

static uint8_t black(const uint8_t *p_frame, uint16_t x, uint16_t y, uint8_t gray)
{
    uint16_t nx = (uint16_t)(127U - y);
    uint16_t ny = (uint16_t)(295U - x);
    size_t offset = (size_t)ny * (gray != 0U ? 32U : 16U) + nx / (gray != 0U ? 4U : 8U);
    uint8_t shift = gray != 0U ? (uint8_t)(6U - 2U * (nx % 4U)) : (uint8_t)(7U - nx % 8U);
    return gray != 0U ? (uint8_t)(((p_frame[offset] >> shift) & 3U) == 3U) :
        (uint8_t)((p_frame[offset] & (1U << shift)) == 0U);
}

static void fixture(weather_page_kind_t page, uint8_t index, uint16_t text_page,
                     uint8_t fresh)
{
    uint16_t x;
    uint16_t y;
    uint8_t i;
    memset(s_bw, 0xA5, sizeof(s_bw));
    memset(s_gray, 0xA5, sizeof(s_gray));
    assert(epd_weather_pages_draw_bw_for_date(s_bw, EPD_CLOCK_VIEW_BW_BYTES,
        &s_batch, page, index, text_page, fresh, &s_today) != 0U);
    assert(epd_weather_pages_draw_gray4_for_date(s_gray, EPD_CLOCK_VIEW_GRAY4_BYTES,
        &s_batch, page, index, text_page, fresh, &s_today) != 0U);
    for (i = 0U; i < 16U; i++) {
        assert(s_bw[EPD_CLOCK_VIEW_BW_BYTES + i] == 0xA5U);
        assert(s_gray[EPD_CLOCK_VIEW_GRAY4_BYTES + i] == 0xA5U);
    }
    for (y = 0U; y < 128U; y++) {
        for (x = 0U; x < 296U; x++) {
            assert(black(s_bw, x, y, 0U) == black(s_gray, x, y, 1U));
            s_checks++;
        }
    }
}

static void preview(const char *p_directory, const char *p_name)
{
    char path[1024];
    FILE *p_file;
    uint16_t x;
    uint16_t y;
    uint8_t pixel;
    (void)snprintf(path, sizeof(path), "%s/%s.pgm", p_directory, p_name);
    p_file = fopen(path, "wb");
    assert(p_file != NULL);
    assert(fprintf(p_file, "P5\n296 128\n255\n") > 0);
    for (y = 0U; y < 128U; y++) {
        for (x = 0U; x < 296U; x++) {
            pixel = black(s_bw, x, y, 0U) != 0U ? 0U : 255U;
            assert(fwrite(&pixel, 1U, 1U, p_file) == 1U);
        }
    }
    assert(fclose(p_file) == 0);
}

static void init(void)
{
    uint8_t i;
    memset(&s_batch, 0, sizeof(s_batch));
    s_batch.city = WEATHER_CITY_ZHANJIANG;
    s_batch.live_valid = 1U;
    s_batch.live.city = WEATHER_CITY_ZHANJIANG;
    s_batch.live.condition = 2U;
    s_batch.live.temperature_tenths = 235;
    s_batch.live.minimum_tenths = 180;
    s_batch.live.maximum_tenths = 270;
    s_batch.live.humidity = 72U;
    s_batch.live.wind_direction = 2U;
    s_batch.wind_bft_valid = 1U;
    s_batch.wind_bft_min = 3U;
    s_batch.wind_bft_max = 4U;
    (void)strcpy(s_batch.live.source_time, "2026-11-02T10:30");
    (void)strcpy(s_batch.checked, "2026-11-02T10:35");
    (void)strcpy(s_batch.last_success, "2026-11-02T10:30");
    s_batch.metrics.valid_mask = 0x3FU;
    s_batch.metrics.value[WEATHER_METRIC_FEELS] = 251;
    s_batch.metrics.value[WEATHER_METRIC_UV] = 60;
    s_batch.metrics.value[WEATHER_METRIC_AQI_CN] = 42;
    s_batch.metrics.value[WEATHER_METRIC_RAIN_1H] = 0;
    s_batch.metrics.value[WEATHER_METRIC_PRESSURE] = 1012;
    s_batch.metrics.value[WEATHER_METRIC_VISIBILITY] = 180;
    s_batch.day_count = 3U;
    for (i = 0U; i < s_batch.day_count; i++) {
        s_batch.days[i].date.year = 2026U;
        s_batch.days[i].date.month = 11U;
        s_batch.days[i].date.day = (uint8_t)(2U + i);
        s_batch.days[i].condition = (uint8_t)(2U + i);
        s_batch.days[i].minimum_tenths = (int16_t)(180 + i * 10);
        s_batch.days[i].maximum_tenths = (int16_t)(270 - i * 10);
        s_batch.days[i].rain_probability_valid = 1U;
        s_batch.days[i].rain_probability = (uint8_t)(20U + 30U * i);
    }
    s_batch.alert_state = WEATHER_ALERT_ACTIVE;
    s_batch.alert_count = 2U;
    for (i = 0U; i < s_batch.alert_count; i++) {
        s_batch.alerts[i].type = i == 0U ? WEATHER_ALERT_RAIN : WEATHER_ALERT_STORM;
        s_batch.alerts[i].color = WEATHER_ALERT_ORANGE;
        (void)strcpy(s_batch.alerts[i].issued, "2026-11-02T10:20");
        (void)strcpy(s_batch.alerts[i].expires, "2026-11-02T13:20");
        (void)strcpy(s_batch.alerts[i].source, "Official weather station");
        (void)strcpy(s_batch.alerts[i].body,
            "Heavy rain is expected within the next three hours. Local accumulations may exceed 50 mm. Avoid low lying roads.");
        (void)strcpy(s_batch.alerts[i].guide,
            "Stay clear of flooded areas. Follow official emergency instructions.");
    }
}

int main(int argc, char **argv)
{
    static const int16_t s_temperatures[] = {-999, -235, -1, 0, 99, 235, 999};
    uint16_t page;
    uint16_t count;
    uint8_t i;
    uint8_t type;
    uint8_t state;
    uint8_t color;
    uint16_t bit;
    uint16_t ink;
    clock_calendar_t today;
    const char *p_output = argc > 1 ? argv[1] : ".";
    init();
    /* Date context, rather than input slot zero, determines the three columns.
     * Once yesterday is outside the window, changing it cannot change pixels. */
    today = s_today;
    clock_calendar_add_seconds(&today, 86400U);
    assert(epd_weather_pages_draw_bw_for_date(s_bw, EPD_CLOCK_VIEW_BW_BYTES,
        &s_batch, WEATHER_PAGE_FORECAST, 0U, 0U, 1U, &today) != 0U);
    memcpy(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES);
    s_batch.days[0].maximum_tenths = 990;
    assert(epd_weather_pages_draw_bw_for_date(s_bw, EPD_CLOCK_VIEW_BW_BYTES,
        &s_batch, WEATHER_PAGE_FORECAST, 0U, 0U, 1U, &today) != 0U);
    assert(memcmp(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES) == 0);
    s_batch.days[1].maximum_tenths = 990;
    assert(epd_weather_pages_draw_bw_for_date(s_bw, EPD_CLOCK_VIEW_BW_BYTES,
        &s_batch, WEATHER_PAGE_FORECAST, 0U, 0U, 1U, &today) != 0U);
    assert(memcmp(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES) != 0);
    preview(p_output, "forecast-after-midnight");
    assert(epd_weather_pages_draw_bw(s_bw, EPD_CLOCK_VIEW_BW_BYTES,
        &s_batch, WEATHER_PAGE_FORECAST, 0U, 0U, 1U) != 0U);
    memcpy(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES);
    assert(epd_weather_pages_draw_bw_for_date(s_bw, EPD_CLOCK_VIEW_BW_BYTES,
        &s_batch, WEATHER_PAGE_FORECAST, 0U, 0U, 1U, &s_today) != 0U);
    assert(memcmp(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES) != 0);
    init();
    fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U); preview(p_output, "live");
    fixture(WEATHER_PAGE_FORECAST, 0U, 0U, 1U); preview(p_output, "forecast");
    fixture(WEATHER_PAGE_DATA, 0U, 0U, 1U); preview(p_output, "data");
    fixture(WEATHER_PAGE_ALERT, 0U, 0U, 1U); preview(p_output, "alert");
    fixture(WEATHER_PAGE_TEXT, 0U, 0U, 1U); preview(p_output, "body");
    fixture(WEATHER_PAGE_SOURCE, 0U, 0U, 1U); preview(p_output, "source");
    s_batch.alerts[0].expired = 1U;
    fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U);
    memcpy(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES);
    s_batch.alerts[0] = s_batch.alerts[1];
    s_batch.alert_count = 1U;
    fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U);
    assert(memcmp(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES) == 0);
    init();
    s_batch.alerts[0].expired = 1U;
    fixture(WEATHER_PAGE_ALERT, 1U, 0U, 1U);
    memcpy(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES);
    s_batch.alerts[0] = s_batch.alerts[1];
    s_batch.alert_count = 1U;
    fixture(WEATHER_PAGE_ALERT, 0U, 0U, 1U);
    assert(memcmp(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES) == 0);
    s_batch.alerts[0].expired = 1U;
    fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U);
    memcpy(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES);
    s_batch.alert_state = WEATHER_ALERT_CLEARED;
    fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U);
    assert(memcmp(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES) == 0);
    init();
    for (i = 0U; i < sizeof(s_temperatures) / sizeof(s_temperatures[0]); i++) {
        s_batch.live.temperature_tenths = s_temperatures[i];
        fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U);
    }
    for (i = 0U; i <= 13U; i++) {
        s_batch.live.condition = i;
        fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U);
    }
    for (state = WEATHER_ALERT_UNKNOWN; state <= WEATHER_ALERT_CLEARED; state++) {
        s_batch.alert_state = (weather_alert_state_t)state;
        fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U);
        fixture(WEATHER_PAGE_DATA, 0U, 0U, 1U);
        fixture(WEATHER_PAGE_FORECAST, 0U, 0U, 1U);
        fixture(WEATHER_PAGE_ALERT, WEATHER_FEED_MAX_ALERTS, 0U, 1U);
    }
    for (type = WEATHER_ALERT_RAIN; type <= WEATHER_ALERT_OTHER; type++) {
        s_batch.alerts[0].type = (weather_alert_type_t)type;
        for (color = WEATHER_ALERT_COLOR_UNKNOWN; color <= WEATHER_ALERT_RED; color++) {
            s_batch.alerts[0].color = (weather_alert_color_t)color;
            fixture(WEATHER_PAGE_ALERT, 0U, 0U, 1U);
        }
    }
    init();
    fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U);
    memcpy(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES);
    fixture(WEATHER_PAGE_LIVE, 0U, 0U, 0U);
    assert(memcmp(s_copy, s_bw, EPD_CLOCK_VIEW_BW_BYTES) != 0);
    preview(p_output, "stale");
    s_batch.live_valid = 0U; s_batch.day_count = 0U; s_batch.metrics.valid_mask = 0U;
    fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U);
    fixture(WEATHER_PAGE_FORECAST, 0U, 0U, 1U);
    fixture(WEATHER_PAGE_DATA, 0U, 0U, 1U); preview(p_output, "missing");
    s_batch.live_valid = 1U; s_batch.wind_bft_valid = 0U;
    s_batch.live.wind_speed_tenths = 123U;
    fixture(WEATHER_PAGE_LIVE, 0U, 0U, 1U);
    /* Actual runtime glyph pool is used, including non-BMP code points.
     * A single black bit at a known offset checks packing/orientation. */
    s_batch.glyph_count = 2U;
    s_batch.glyphs[0].codepoint = 0x96E8U;
    s_batch.glyphs[1].codepoint = 0x1F300U;
    memset(s_batch.glyphs[0].bitmap, 0, WEATHER_FEED_GLYPH_BYTES);
    memset(s_batch.glyphs[1].bitmap, 0, WEATHER_FEED_GLYPH_BYTES);
    bit = (uint16_t)(7U * 26U + 3U);
    s_batch.glyphs[0].bitmap[bit / 8U] = (uint8_t)(0x80U >> (bit % 8U));
    s_batch.glyphs[1].bitmap[0] = 0x80U;
    (void)strcpy(s_batch.alerts[0].body, "\xE9\x9B\xA8\xF0\x9F\x8C\x80");
    fixture(WEATHER_PAGE_TEXT, 0U, 0U, 1U);
    assert(black(s_bw, 15U, 48U, 0U) != 0U);
    assert(black(s_bw, 38U, 41U, 0U) != 0U);
    (void)strcpy(s_batch.alerts[0].body, "\xC2\xB0");
    s_batch.glyph_count = 0U;
    fixture(WEATHER_PAGE_TEXT, 0U, 0U, 1U);
    assert(black(s_bw, 15U, 47U, 0U) != 0U);
    assert(black(s_bw, 14U, 43U, 0U) == 0U);
    assert(epd_weather_pages_text_count(&s_batch, WEATHER_FEED_MAX_ALERTS) == 0U);
    memset(s_batch.alerts[0].body, 'A', WEATHER_FEED_BODY_MAX);
    s_batch.alerts[0].body[WEATHER_FEED_BODY_MAX] = '\0';
    memset(s_batch.alerts[0].guide, 'B', WEATHER_FEED_GUIDE_MAX);
    s_batch.alerts[0].guide[WEATHER_FEED_GUIDE_MAX] = '\0';
    count = epd_weather_pages_text_count(&s_batch, 0U);
    assert(count == 20U + 13U + 1U);
    for (page = 0U; page < count; page++) { fixture(WEATHER_PAGE_TEXT, 0U, page, 1U); }
    (void)strcpy(s_batch.alerts[0].body, "First clause, second clause.\nLast line.");
    count = epd_weather_pages_text_count(&s_batch, 0U);
    for (page = 0U; page < count; page++) { fixture(WEATHER_PAGE_TEXT, 0U, page, 1U); }
    (void)strcpy(s_batch.alerts[0].body, "\nA");
    fixture(WEATHER_PAGE_TEXT, 0U, 0U, 1U);
    for (page = 12U; page < 25U; page++) {
        for (bit = 41U; bit < 67U; bit++) { assert(black(s_bw, page, bit, 0U) == 0U); }
    }
    memset(s_batch.alerts[0].body, '\n', WEATHER_FEED_BODY_MAX);
    s_batch.alerts[0].body[WEATHER_FEED_BODY_MAX] = '\0';
    s_batch.alerts[0].guide[0] = '\0';
    assert(epd_weather_pages_text_count(&s_batch, 0U) == WEATHER_FEED_BODY_MAX / 2U + 1U);
    fixture(WEATHER_PAGE_TEXT, 0U, WEATHER_FEED_BODY_MAX / 2U - 1U, 1U);
    s_batch.alerts[0].body[WEATHER_FEED_BODY_MAX - 1U] = 'A';
    fixture(WEATHER_PAGE_TEXT, 0U, WEATHER_FEED_BODY_MAX / 2U - 1U, 1U);
    ink = 0U;
    for (page = 12U; page < 25U; page++) {
        for (bit = 81U; bit < 106U; bit++) { ink = (uint16_t)(ink + black(s_bw, page, bit, 0U)); }
    }
    assert(ink != 0U);
    s_batch.alerts[0].body[0] = '\0';
    s_batch.alerts[0].guide[0] = '\0';
    s_batch.alerts[0].source[0] = '\0';
    s_batch.alerts[0].issued[0] = '\0';
    s_batch.alerts[0].expires[0] = '\0';
    assert(epd_weather_pages_text_count(&s_batch, 0U) == 1U);
    fixture(WEATHER_PAGE_TEXT, 0U, 0U, 1U);
    assert(epd_weather_pages_draw_bw(NULL, EPD_CLOCK_VIEW_BW_BYTES, &s_batch, WEATHER_PAGE_LIVE, 0U, 0U, 1U) == 0U);
    assert(epd_weather_pages_draw_bw(s_bw, EPD_CLOCK_VIEW_BW_BYTES - 1U, &s_batch, WEATHER_PAGE_LIVE, 0U, 0U, 1U) == 0U);
    printf("PASS: weather pages: %lu BW/gray pixel comparisons, bounded fixtures, UTF-8 paging and injected glyphs\n", (unsigned long)s_checks);
    return 0;
}
