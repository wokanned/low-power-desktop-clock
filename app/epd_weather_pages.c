#include "epd_weather_pages.h"
#include "epd_clock_view.h"
#include "../resources/weather_pages_assets.h"

#include <stdio.h>
#include <string.h>

#define WEATHER_PAGE_BW_STRIDE 16U
#define WEATHER_PAGE_GRAY_STRIDE 32U
#define WEATHER_PAGE_LEFT 10
#define WEATHER_PAGE_RIGHT 286
#define WEATHER_PAGE_BAR_TOP 110
#define WEATHER_PAGE_TEXT_WIDTH 272U
#define WEATHER_PAGE_TEXT_ROWS 2U
#define WEATHER_PAGE_TEXT_SIZE 26U
#define WEATHER_PAGE_ASCII_WIDTH 13U
#define WEATHER_PAGE_MAX_LINES (WEATHER_FEED_BODY_MAX + 1U)

typedef struct {
    uint8_t *p_frame;
    uint8_t gray;
    uint8_t valid;
    uint8_t fresh;
    const clock_calendar_t *p_today;
} weather_page_canvas_t;

typedef struct {
    const char *p_begin;
    const char *p_end;
} weather_page_line_t;

static const char *const s_conditions[] = {
    "UNKNOWN", "SUNNY", "CLOUDY", "OVERCAST", "LIGHT RAIN", "RAIN",
    "HEAVY RAIN", "STORM", "LIGHT SNOW", "SNOW", "HEAVY SNOW", "SLEET", "FOG", "HAZE"
};
static const char *const s_types[] = {
    "RAIN", "STORM", "WIND", "TYPHOON", "HEAT", "FOG", "SNOW", "FLOOD", "ALERT"
};
static const char *const s_colors[] = {
    "UNKNOWN", "WHITE", "BLUE", "YELLOW", "ORANGE", "RED"
};
static const char *const s_winds[] = {"--", "N", "NE", "E", "SE", "S", "SW", "W", "NW"};
static const char *const s_weekdays[] = {"---", "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};

static void s_pixel(weather_page_canvas_t *p_canvas, int16_t x, int16_t y,
                    uint8_t black)
{
    uint16_t native_x;
    uint16_t native_y;
    size_t offset;
    uint8_t shift;
    if (x < 0 || y < 0 || x >= (int16_t)EPD_CLOCK_VIEW_WIDTH ||
        y >= (int16_t)EPD_CLOCK_VIEW_HEIGHT) {
        p_canvas->valid = 0U;
        return;
    }
    native_x = (uint16_t)(127 - y);
    native_y = (uint16_t)(295 - x);
    if (p_canvas->gray != 0U) {
        offset = (size_t)native_y * WEATHER_PAGE_GRAY_STRIDE + native_x / 4U;
        shift = (uint8_t)(6U - 2U * (native_x % 4U));
        if (black != 0U) { p_canvas->p_frame[offset] |= (uint8_t)(3U << shift); }
        else { p_canvas->p_frame[offset] &= (uint8_t)~(3U << shift); }
    } else {
        offset = (size_t)native_y * WEATHER_PAGE_BW_STRIDE + native_x / 8U;
        shift = (uint8_t)(7U - native_x % 8U);
        if (black != 0U) { p_canvas->p_frame[offset] &= (uint8_t)~(1U << shift); }
        else { p_canvas->p_frame[offset] |= (uint8_t)(1U << shift); }
    }
}

static void s_rect(weather_page_canvas_t *p_canvas, int16_t x, int16_t y,
                   uint16_t width, uint16_t height, uint8_t black)
{
    uint16_t i;
    uint16_t j;
    for (i = 0U; i < height; i++) {
        for (j = 0U; j < width; j++) {
            s_pixel(p_canvas, (int16_t)(x + j), (int16_t)(y + i), black);
        }
    }
}

static const weather_page_glyph_t *s_ascii(const weather_page_font_t *p_font,
                                          uint32_t codepoint)
{
    uint32_t index;
    index = codepoint == 176U ? 95U :
        (codepoint >= 32U && codepoint <= 126U ? codepoint - 32U : 31U);
    return &p_font->p_glyphs[index];
}

static uint16_t s_width(const char *p_text, const weather_page_font_t *p_font,
                        uint8_t scale)
{
    uint16_t width = 0U;
    const weather_page_glyph_t *p_glyph;
    uint8_t code;
    while (*p_text != '\0') {
        code = (uint8_t)*p_text++;
        p_glyph = s_ascii(p_font, code);
        width = (uint16_t)(width + (uint16_t)p_glyph->advance * scale);
    }
    return width;
}

static void s_char(weather_page_canvas_t *p_canvas,
    const weather_page_font_t *p_font, uint32_t codepoint, int16_t x, int16_t y,
    uint8_t scale, uint8_t invert)
{
    const weather_page_glyph_t *p_glyph = s_ascii(p_font, codepoint);
    const uint8_t *p_bits = p_font->p_bits + p_glyph->offset;
    uint16_t i;
    uint16_t j;
    uint16_t bit;
    for (i = 0U; i < p_glyph->height; i++) {
        for (j = 0U; j < p_glyph->width; j++) {
            bit = (uint16_t)(i * p_glyph->width + j);
            if ((p_bits[bit / 8U] & (uint8_t)(0x80U >> (bit % 8U))) != 0U) {
                s_rect(p_canvas, (int16_t)(x + j * scale),
                    (int16_t)(y + (i + p_glyph->top) * scale), scale, scale,
                    (uint8_t)(invert == 0U));
            }
        }
    }
}

static void s_text(weather_page_canvas_t *p_canvas, const char *p_text,
    const weather_page_font_t *p_font, int16_t x, int16_t y, uint8_t scale,
    uint8_t invert)
{
    uint8_t code;
    while (*p_text != '\0') {
        code = (uint8_t)*p_text++;
        s_char(p_canvas, p_font, code, x, y, scale, invert);
        x = (int16_t)(x + s_ascii(p_font, code)->advance * scale);
    }
}

static void s_fit(weather_page_canvas_t *p_canvas, const char *p_text,
    const weather_page_font_t *p_font, int16_t left, int16_t y,
    uint16_t width, uint8_t align, uint8_t invert)
{
    uint16_t measured = s_width(p_text, p_font, 1U);
    int16_t x = left;
    if (measured > width) {
        p_font = &g_weather_page_normal;
        measured = s_width(p_text, p_font, 1U);
    }
    if (measured > width) { p_font = &g_weather_page_small; }
    measured = s_width(p_text, p_font, 1U);
    if (measured <= width) {
        if (align == 1U) { x = (int16_t)(left + (width - measured) / 2U); }
        if (align == 2U) { x = (int16_t)(left + width - measured); }
        s_text(p_canvas, p_text, p_font, x, y, 1U, invert);
    } else {
        while (*p_text != '\0' &&
            (uint16_t)(x - left + s_ascii(p_font, (uint8_t)*p_text)->advance) <= width) {
            s_char(p_canvas, p_font, (uint8_t)*p_text, x, y, 1U, invert);
            x = (int16_t)(x + s_ascii(p_font, (uint8_t)*p_text)->advance);
            p_text++;
        }
    }
}

static void s_tenths(char *p_text, size_t capacity, int32_t value)
{
    uint32_t magnitude = value < 0 ? (uint32_t)(-(value + 1)) + 1U : (uint32_t)value;
    (void)snprintf(p_text, capacity, "%s%lu.%lu", value < 0 ? "-" : "",
        (unsigned long)(magnitude / 10U), (unsigned long)(magnitude % 10U));
}

static int16_t s_round_tenths(int16_t value)
{
    return (int16_t)(value < 0 ? -((-value + 5) / 10) : (value + 5) / 10);
}

static void s_range(char *p_text, size_t capacity, int16_t minimum, int16_t maximum)
{
    (void)snprintf(p_text, capacity, "%d~%d", (int)s_round_tenths(maximum),
        (int)s_round_tenths(minimum));
}

static const char *s_stamp_time(const char *p_stamp)
{
    return strlen(p_stamp) >= WEATHER_SOURCE_TIME_LENGTH ? p_stamp + 11 : "--:--";
}

static const char *s_type(weather_alert_type_t type)
{
    return s_types[(uint8_t)type <= WEATHER_ALERT_OTHER ? (uint8_t)type : WEATHER_ALERT_OTHER];
}

static const char *s_color(weather_alert_color_t color)
{
    return s_colors[(uint8_t)color <= WEATHER_ALERT_RED ? (uint8_t)color : 0U];
}

static const char *s_condition(uint8_t condition)
{
    return s_conditions[condition < sizeof(s_conditions) / sizeof(s_conditions[0]) ? condition : 0U];
}

static uint8_t s_active_count(const weather_feed_batch_t *p_batch)
{
    uint8_t i;
    uint8_t count = 0U;
    for (i = 0U; i < p_batch->alert_count; i++) {
        if (p_batch->alerts[i].expired == 0U) { count++; }
    }
    return count;
}

static void s_header(weather_page_canvas_t *p_canvas, const weather_feed_batch_t *p_batch)
{
    char text[16];
    s_text(p_canvas, p_batch->city == WEATHER_CITY_SHENZHEN ? "SHENZHEN" : "ZHANJIANG",
        &g_weather_page_small, WEATHER_PAGE_LEFT, 6, 1U, 0U);
    (void)snprintf(text, sizeof(text), "%s %.5s", p_canvas->fresh != 0U ? "UPD" : "STALE", p_batch->live_valid != 0U ?
        s_stamp_time(p_batch->live.source_time) : "--:--");
    s_fit(p_canvas, text, &g_weather_page_small, 180, 6, 106U, 2U, 0U);
}

static void s_alert_bar(weather_page_canvas_t *p_canvas, const weather_feed_batch_t *p_batch)
{
    char left[32];
    char right[16];
    uint8_t invert = 0U;
    uint8_t index = 0U;
    uint8_t i;
    uint8_t active = s_active_count(p_batch);
    if (p_batch->alert_state == WEATHER_ALERT_ACTIVE && active != 0U) {
        for (i = 0U; i < p_batch->alert_count; i++) {
            if (p_batch->alerts[i].expired == 0U) { index = i; break; }
        }
        (void)snprintf(left, sizeof(left), "%s/%s %u", s_type(p_batch->alerts[index].type),
            s_color(p_batch->alerts[index].color), (unsigned)active);
        (void)snprintf(right, sizeof(right), "CHK %.5s", s_stamp_time(p_batch->checked));
        s_rect(p_canvas, 0, WEATHER_PAGE_BAR_TOP, 296U, 18U, 1U);
        invert = 1U;
    } else {
        if (p_batch->alert_state == WEATHER_ALERT_OFFLINE) {
            (void)snprintf(left, sizeof(left), "ALERT: OFFLINE");
            (void)snprintf(right, sizeof(right), "LAST %.5s", s_stamp_time(p_batch->last_success));
        } else {
            (void)snprintf(left, sizeof(left), "%s", p_batch->alert_state == WEATHER_ALERT_NONE ?
                "NO ALERTS" : (p_batch->alert_state == WEATHER_ALERT_CLEARED ||
                    (p_batch->alert_state == WEATHER_ALERT_ACTIVE && active == 0U) ?
                    "ALERT CLEARED" : "ALERT: UNKNOWN"));
            (void)snprintf(right, sizeof(right), "CHK %.5s", s_stamp_time(p_batch->checked));
        }
        s_rect(p_canvas, 10, 108, 276U, 1U, 1U);
    }
    s_fit(p_canvas, left, &g_weather_page_small, 10, 112, 184U, 0U, invert);
    s_fit(p_canvas, right, &g_weather_page_small, 198, 112, 88U, 2U, invert);
}

static void s_pair(weather_page_canvas_t *p_canvas, const char *p_value, const char *p_unit,
    int16_t left, int16_t top, uint16_t width, uint8_t large)
{
    const weather_page_font_t *p_numbers = large != 0U ? &g_weather_page_temperature : &g_weather_page_number;
    const weather_page_font_t *p_units = large != 0U ? &g_weather_page_bold : &g_weather_page_small;
    uint16_t value_width = s_width(p_value, p_numbers, 1U);
    uint16_t unit_width = s_width(p_unit, p_units, 1U);
    uint16_t combined = (uint16_t)(value_width + (unit_width != 0U ? 3U : 0U) + unit_width);
    int16_t unit_y;
    int16_t x = left;
    if (combined > width) {
        p_numbers = &g_weather_page_normal;
        p_units = &g_weather_page_small;
        value_width = s_width(p_value, p_numbers, 1U);
        unit_width = s_width(p_unit, p_units, 1U);
        combined = (uint16_t)(value_width + (unit_width != 0U ? 2U : 0U) + unit_width);
    }
    if (combined > width) { s_fit(p_canvas, "N/A", &g_weather_page_normal, left, top, width, 0U, 0U); return; }
    if (large != 0U) { x = (int16_t)(left + (width - combined) / 2U); }
    s_text(p_canvas, p_value, p_numbers, x, top, 1U, 0U);
    unit_y = (int16_t)(top + p_numbers->height - p_units->height);
    /* Numeric fonts omit letters; N/A must be drawn with the native font. */
    s_text(p_canvas, p_unit, p_units, (int16_t)(x + value_width + (unit_width != 0U ? 3U : 0U)), unit_y, 1U, 0U);
}

static uint8_t s_cloud_inside(int16_t x, int16_t y)
{
    int16_t dx;
    int16_t dy;
    uint8_t inside = (uint8_t)(x >= 31 && x <= 58 && y >= 52 && y <= 65);
    dx = (int16_t)(x - 31); dy = (int16_t)(y - 52);
    inside = (uint8_t)(inside != 0U || dx * dx + dy * dy <= 13 * 13);
    dx = (int16_t)(x - 45); dy = (int16_t)(y - 46);
    inside = (uint8_t)(inside != 0U || dx * dx + dy * dy <= 16 * 16);
    dx = (int16_t)(x - 58); dy = (int16_t)(y - 53);
    return (uint8_t)(inside != 0U || dx * dx + dy * dy <= 12 * 12);
}

static void s_line(weather_page_canvas_t *p_canvas, int16_t x0, int16_t y0,
                    int16_t x1, int16_t y1)
{
    int16_t dx = (int16_t)(x1 >= x0 ? x1 - x0 : x0 - x1);
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t dy = (int16_t)(y1 >= y0 ? y0 - y1 : y1 - y0);
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t error = (int16_t)(dx + dy);
    int16_t twice;
    for (;;) {
        s_pixel(p_canvas, x0, y0, 1U);
        if (x0 == x1 && y0 == y1) { break; }
        twice = (int16_t)(error * 2);
        if (twice >= dy) { error = (int16_t)(error + dy); x0 = (int16_t)(x0 + sx); }
        if (twice <= dx) { error = (int16_t)(error + dx); y0 = (int16_t)(y0 + sy); }
    }
}

static void s_snowflake(weather_page_canvas_t *p_canvas, int16_t x, int16_t y)
{
    s_line(p_canvas, (int16_t)(x - 3), y, (int16_t)(x + 3), y);
    s_line(p_canvas, x, (int16_t)(y - 3), x, (int16_t)(y + 3));
    s_line(p_canvas, (int16_t)(x - 2), (int16_t)(y - 2), (int16_t)(x + 2), (int16_t)(y + 2));
    s_line(p_canvas, (int16_t)(x - 2), (int16_t)(y + 2), (int16_t)(x + 2), (int16_t)(y - 2));
}

static void s_icon(weather_page_canvas_t *p_canvas, uint8_t condition)
{
    int16_t x;
    int16_t y;
    int16_t dx;
    int16_t dy;
    int16_t sun_x = condition == 1U ? 43 : 58;
    int16_t sun_y = condition == 1U ? 43 : 35;
    int16_t count;
    uint8_t edge;
    if (condition == 12U) {
        s_rect(p_canvas, 20, 34, 47U, 2U, 1U);
        s_rect(p_canvas, 15, 46, 56U, 2U, 1U);
        s_rect(p_canvas, 20, 58, 47U, 2U, 1U);
        return;
    }
    if (condition == 13U) {
        for (y = 37; y <= 53; y = (int16_t)(y + 16)) {
            for (x = 18; x <= 58; x = (int16_t)(x + 16)) {
                s_line(p_canvas, x, y, (int16_t)(x + 8), (int16_t)(y - 5));
                s_line(p_canvas, (int16_t)(x + 8), (int16_t)(y - 5), (int16_t)(x + 16), y);
            }
        }
        return;
    }
    if (condition == 1U || condition == 2U) {
        for (y = (int16_t)(sun_y - 18); y <= sun_y + 18; y++) {
            for (x = (int16_t)(sun_x - 18); x <= sun_x + 18; x++) {
                dx = (int16_t)(x - sun_x); dy = (int16_t)(y - sun_y);
                if ((dx * dx + dy * dy >= 80 && dx * dx + dy * dy <= 121) ||
                    (dx == 0 && (dy < -13 || dy > 13)) ||
                    (dy == 0 && (dx < -13 || dx > 13)) ||
                    (dx == dy && (dx < -10 || dx > 10)) ||
                    (dx == -dy && (dx < -10 || dx > 10))) { s_pixel(p_canvas, x, y, 1U); }
            }
        }
        if (condition == 1U) { return; }
    }
    if (condition != 0U) {
        for (y = 28; y <= 67; y++) {
            for (x = 15; x <= 71; x++) {
                if (s_cloud_inside(x, y) != 0U) {
                    edge = (uint8_t)(s_cloud_inside((int16_t)(x - 1), y) == 0U ||
                        s_cloud_inside((int16_t)(x + 1), y) == 0U ||
                        s_cloud_inside(x, (int16_t)(y - 1)) == 0U ||
                        s_cloud_inside(x, (int16_t)(y + 1)) == 0U);
                    s_pixel(p_canvas, x, y, edge);
                }
            }
        }
        if (condition >= 4U && condition <= 6U) {
            count = (int16_t)(condition - 3U);
            for (x = 0; x < count; x++) {
                s_line(p_canvas, (int16_t)(35 + x * 12), 68, (int16_t)(32 + x * 12), 73);
            }
        } else if (condition == 7U) {
            s_line(p_canvas, 47, 64, 40, 70);
            s_line(p_canvas, 40, 70, 47, 70);
            s_line(p_canvas, 47, 70, 43, 74);
        } else if (condition >= 8U && condition <= 10U) {
            count = (int16_t)(condition - 7U);
            for (x = 0; x < count; x++) { s_snowflake(p_canvas, (int16_t)(32 + x * 13), 70); }
        } else if (condition == 11U) {
            s_line(p_canvas, 32, 68, 29, 73);
            s_snowflake(p_canvas, 53, 70);
        }
    } else { s_fit(p_canvas, "?", &g_weather_page_large, 15, 34, 56U, 1U, 0U); }
}

static void s_live(weather_page_canvas_t *p_canvas, const weather_feed_batch_t *p_batch)
{
    char value[24];
    char label[32];
    s_header(p_canvas, p_batch);
    if (p_batch->live_valid == 0U) {
        s_fit(p_canvas, "NO LIVE DATA", &g_weather_page_medium, 10, 42, 276U, 1U, 0U);
    } else {
        s_icon(p_canvas, p_batch->live.condition);
        s_fit(p_canvas, s_condition(p_batch->live.condition), &g_weather_page_small, 4, 76, 86U, 1U, 0U);
        s_tenths(value, sizeof(value), p_batch->live.temperature_tenths);
        (void)snprintf(label, sizeof(label), "%cC", (char)176);
        s_pair(p_canvas, value, label, 94, 24, 192U, 1U);
        s_range(value, sizeof(value), p_batch->live.minimum_tenths, p_batch->live.maximum_tenths);
        s_fit(p_canvas, value, &g_weather_page_number, 94, 71, 192U, 1U, 0U);
        (void)snprintf(label, sizeof(label), "RH %u%%", (unsigned)p_batch->live.humidity);
        s_fit(p_canvas, label, &g_weather_page_normal, 10, 93, 106U, 0U, 0U);
        if (p_batch->wind_bft_valid != 0U) {
            if (p_batch->wind_bft_min == p_batch->wind_bft_max) {
                (void)snprintf(label, sizeof(label), "%s %u Bft", s_winds[p_batch->live.wind_direction <= 8U ? p_batch->live.wind_direction : 0U],
                    (unsigned)p_batch->wind_bft_min);
            } else {
                (void)snprintf(label, sizeof(label), "%s %u~%u Bft", s_winds[p_batch->live.wind_direction <= 8U ? p_batch->live.wind_direction : 0U],
                    (unsigned)p_batch->wind_bft_min, (unsigned)p_batch->wind_bft_max);
            }
        } else {
            s_tenths(value, sizeof(value), p_batch->live.wind_speed_tenths);
            (void)snprintf(label, sizeof(label), "%s %s m/s", s_winds[p_batch->live.wind_direction <= 8U ? p_batch->live.wind_direction : 0U], value);
        }
        s_fit(p_canvas, label, &g_weather_page_normal, 118, 93, 168U, 2U, 0U);
    }
    s_alert_bar(p_canvas, p_batch);
}

static void s_forecast(weather_page_canvas_t *p_canvas, const weather_feed_batch_t *p_batch)
{
    const weather_feed_day_t *p_day;
    clock_calendar_t target;
    uint8_t i;
    uint8_t j;
    uint8_t weekday;
    uint8_t has_today;
    char value[24];
    int16_t left;
    s_header(p_canvas, p_batch);
    has_today = (uint8_t)(p_canvas->p_today != NULL && clock_calendar_valid(p_canvas->p_today));
    memset(&target, 0, sizeof(target));
    if (has_today != 0U) { target = *p_canvas->p_today; }
    for (i = 0U; i < WEATHER_FEED_MAX_DAYS; i++) {
        left = (int16_t)(2 + i * 98U);
        p_day = NULL;
        if (has_today != 0U) {
            for (j = 0U; j < p_batch->day_count; j++) {
                if (p_batch->days[j].date.year == target.year &&
                    p_batch->days[j].date.month == target.month &&
                    p_batch->days[j].date.day == target.day) { p_day = &p_batch->days[j]; break; }
            }
            weekday = clock_calendar_weekday(target.year, target.month, target.day);
        } else if (i < p_batch->day_count) {
            p_day = &p_batch->days[i];
            weekday = clock_calendar_weekday(p_day->date.year, p_day->date.month, p_day->date.day);
        } else { weekday = 0U; }
        s_fit(p_canvas, has_today != 0U && i == 0U ? "TODAY" : s_weekdays[weekday <= 7U ? weekday : 0U],
            &g_weather_page_normal, left, 29, 94U, 1U, 0U);
        if (p_day != NULL) {
            s_fit(p_canvas, s_condition(p_day->condition), &g_weather_page_small,
                left, 49, 94U, 1U, 0U);
            s_range(value, sizeof(value), p_day->minimum_tenths, p_day->maximum_tenths);
            s_fit(p_canvas, value, &g_weather_page_number, left, 67, 94U, 1U, 0U);
            if (p_day->rain_probability_valid != 0U) {
                (void)snprintf(value, sizeof(value), "RAIN %u%%", (unsigned)p_day->rain_probability);
            } else { (void)snprintf(value, sizeof(value), "RAIN N/A"); }
            s_fit(p_canvas, value, &g_weather_page_small, left, 93, 94U, 1U, 0U);
        } else {
            s_fit(p_canvas, "N/A", &g_weather_page_medium, left, 55, 94U, 1U, 0U);
        }
        if (has_today != 0U) { clock_calendar_add_seconds(&target, 86400U); }
    }
    s_rect(p_canvas, 98, 27, 1U, 77U, 1U);
    s_rect(p_canvas, 196, 27, 1U, 77U, 1U);
    s_alert_bar(p_canvas, p_batch);
}

static void s_metrics(weather_page_canvas_t *p_canvas, const weather_feed_batch_t *p_batch)
{
    static const char *const s_labels[] = {"FEELS", "UV", "AQI-CN", "RAIN 1h", "PRESS", "VIS"};
    static const char *const s_units[] = {"\260C", "", "", "mm", "hPa", "km"};
    uint8_t i;
    int16_t left;
    int16_t top;
    char value[24];
    s_header(p_canvas, p_batch);
    for (i = 0U; i < WEATHER_METRIC_COUNT; i++) {
        left = (int16_t)(10 + (i % 3U) * 98U);
        top = (int16_t)(29 + (i / 3U) * 40U);
        s_text(p_canvas, s_labels[i], &g_weather_page_small, left, top, 1U, 0U);
        if ((p_batch->metrics.valid_mask & (1U << i)) == 0U) {
            s_text(p_canvas, "N/A", &g_weather_page_medium, left, (int16_t)(top + 14), 1U, 0U);
        } else {
            if (i == WEATHER_METRIC_AQI_CN || i == WEATHER_METRIC_PRESSURE) {
                (void)snprintf(value, sizeof(value), "%ld", (long)p_batch->metrics.value[i]);
            } else { s_tenths(value, sizeof(value), p_batch->metrics.value[i]); }
            s_pair(p_canvas, value, s_units[i], left, (int16_t)(top + 14), 84U, 0U);
        }
    }
    s_rect(p_canvas, 98, 27, 1U, 77U, 1U);
    s_rect(p_canvas, 196, 27, 1U, 77U, 1U);
    s_rect(p_canvas, 10, 65, 276U, 1U, 1U);
    s_alert_bar(p_canvas, p_batch);
}

static uint32_t s_utf8(const char **pp_text, const char *p_end)
{
    const uint8_t *p_text = (const uint8_t *)*pp_text;
    uint32_t codepoint;
    uint8_t count;
    uint8_t i;
    if (*pp_text >= p_end) { return 0U; }
    if (*p_text < 0x80U) { (*pp_text)++; return *p_text; }
    count = *p_text >= 0xF0U ? 4U : (*p_text >= 0xE0U ? 3U : 2U);
    if ((size_t)(p_end - *pp_text) < count) { *pp_text = p_end; return 0xFFFDU; }
    codepoint = (uint32_t)(*p_text & (uint8_t)(0x7FU >> count));
    for (i = 1U; i < count; i++) {
        if ((p_text[i] & 0xC0U) != 0x80U) { (*pp_text)++; return 0xFFFDU; }
        codepoint = (codepoint << 6) | (uint32_t)(p_text[i] & 0x3FU);
    }
    *pp_text += count;
    return codepoint;
}

static uint8_t s_punctuation(uint32_t codepoint)
{
    return (uint8_t)(codepoint == 0x3001U || codepoint == 0x3002U ||
        codepoint == 0xFF0CU || codepoint == 0xFF1BU || codepoint == 0xFF1AU ||
        codepoint == 0xFF01U || codepoint == 0xFF1FU || codepoint == 0xFF09U ||
        codepoint == 0x300BU || codepoint == 0x201DU || codepoint == 0x2019U);
}

/* O(length) bounded wrapping; preserve UTF-8 boundaries and explicit newlines.
 * Prefer the last clause separator/space, and keep closing punctuation with
 * its preceding character. No duplicated/dropped bytes across page breaks. */
static uint16_t s_lines(const char *p_text, uint16_t requested,
                        weather_page_line_t *p_selected)
{
    const char *p_begin = p_text;
    const char *p_end = p_text + strlen(p_text);
    const char *p_cursor;
    const char *p_next;
    const char *p_break;
    const char *p_previous;
    uint32_t codepoint;
    uint16_t count = 0U;
    uint16_t width;
    uint16_t char_width;
    while (p_begin < p_end && count < WEATHER_PAGE_MAX_LINES) {
        p_cursor = p_begin;
        p_break = NULL;
        p_previous = p_begin;
        width = 0U;
        while (p_cursor < p_end) {
            p_next = p_cursor;
            codepoint = s_utf8(&p_next, p_end);
            char_width = codepoint < 128U || codepoint == 176U ? WEATHER_PAGE_ASCII_WIDTH : WEATHER_PAGE_TEXT_SIZE;
            if (codepoint == '\n' || codepoint == '\r') { break; }
            if (width + char_width > WEATHER_PAGE_TEXT_WIDTH) {
                if (p_break != NULL && p_break > p_begin) { p_cursor = p_break; }
                else if (s_punctuation(codepoint) != 0U && p_previous > p_begin) {
                    p_cursor = p_previous;
                }
                break;
            }
            width = (uint16_t)(width + char_width);
            p_previous = p_cursor;
            p_cursor = p_next;
            if (codepoint == ' ' || codepoint == ',' || codepoint == '.' ||
                codepoint == ';' || codepoint == 0x3002U || codepoint == 0xFF0CU ||
                codepoint == 0xFF1BU) { p_break = p_cursor; }
        }
        if (p_cursor == p_begin && *p_begin != '\n' && *p_begin != '\r') {
            p_cursor = p_begin; (void)s_utf8(&p_cursor, p_end);
        }
        if (count == requested && p_selected != NULL) {
            p_selected->p_begin = p_begin;
            p_selected->p_end = p_cursor;
        }
        count++;
        p_begin = p_cursor;
        if (p_begin < p_end && (*p_begin == '\n' || *p_begin == '\r')) {
            if (*p_begin++ == '\r' && p_begin < p_end && *p_begin == '\n') { p_begin++; }
        }
    }
    return count == 0U ? 1U : count;
}

static const weather_feed_glyph_t *s_glyph(const weather_feed_batch_t *p_batch,
                                          uint32_t codepoint)
{
    uint16_t i;
    for (i = 0U; i < p_batch->glyph_count; i++) {
        if (p_batch->glyphs[i].codepoint == codepoint) { return &p_batch->glyphs[i]; }
    }
    return NULL;
}

static void s_utf8_line(weather_page_canvas_t *p_canvas,
    const weather_feed_batch_t *p_batch, const weather_page_line_t *p_line,
    int16_t left, int16_t top)
{
    const char *p_cursor = p_line->p_begin;
    const weather_feed_glyph_t *p_glyph;
    uint32_t codepoint;
    uint16_t i;
    uint16_t j;
    uint16_t bit;
    int16_t x = left;
    while (p_cursor != NULL && p_cursor < p_line->p_end) {
        codepoint = s_utf8(&p_cursor, p_line->p_end);
        if (codepoint < 128U || codepoint == 176U) {
            s_char(p_canvas, &g_weather_page_medium, codepoint, x,
                (int16_t)(top + 4), 1U, 0U);
            x = (int16_t)(x + WEATHER_PAGE_ASCII_WIDTH);
        } else {
            p_glyph = s_glyph(p_batch, codepoint);
            if (p_glyph != NULL) {
                for (i = 0U; i < WEATHER_PAGE_TEXT_SIZE; i++) {
                    for (j = 0U; j < WEATHER_PAGE_TEXT_SIZE; j++) {
                        bit = (uint16_t)(i * WEATHER_PAGE_TEXT_SIZE + j);
                        if ((p_glyph->bitmap[bit / 8U] & (uint8_t)(0x80U >> (bit % 8U))) != 0U) {
                            s_pixel(p_canvas, (int16_t)(x + j), (int16_t)(top + i), 1U);
                        }
                    }
                }
            } else {
                s_rect(p_canvas, (int16_t)(x + 2), (int16_t)(top + 2), 22U, 1U, 1U);
                s_rect(p_canvas, (int16_t)(x + 2), (int16_t)(top + 23), 22U, 1U, 1U);
                s_rect(p_canvas, (int16_t)(x + 2), (int16_t)(top + 2), 1U, 22U, 1U);
                s_rect(p_canvas, (int16_t)(x + 23), (int16_t)(top + 2), 1U, 22U, 1U);
            }
            x = (int16_t)(x + WEATHER_PAGE_TEXT_SIZE);
        }
    }
}

static uint16_t s_pages(const char *p_text)
{
    return (uint16_t)((s_lines(p_text, UINT16_MAX, NULL) + WEATHER_PAGE_TEXT_ROWS - 1U) / WEATHER_PAGE_TEXT_ROWS);
}

uint16_t epd_weather_pages_text_count(const weather_feed_batch_t *p_batch,
                                     uint8_t alert_index)
{
    const weather_feed_alert_t *p_alert;
    if (p_batch == NULL || alert_index >= p_batch->alert_count) { return 0U; }
    p_alert = &p_batch->alerts[alert_index];
    return (uint16_t)((p_alert->body[0] != '\0' ? s_pages(p_alert->body) : 0U) +
        (p_alert->guide[0] != '\0' ? s_pages(p_alert->guide) : 0U) + s_pages(p_alert->source));
}

static void s_alert_state(weather_page_canvas_t *p_canvas,
                          const weather_feed_batch_t *p_batch)
{
    const char *p_title;
    const char *p_subtitle;
    uint8_t cleared = (uint8_t)(p_batch->alert_state == WEATHER_ALERT_CLEARED ||
        (p_batch->alert_state == WEATHER_ALERT_ACTIVE && s_active_count(p_batch) == 0U));
    p_title = p_batch->alert_state == WEATHER_ALERT_NONE ? "NO ALERTS" :
        (cleared != 0U ? "CLEARED" : "UNKNOWN");
    p_subtitle = p_batch->alert_state == WEATHER_ALERT_NONE ? "CHECK OK" :
        (cleared != 0U ? "ALERT ENDED" : "CHECK FAILED");
    s_text(p_canvas, "WEATHER ALERT", &g_weather_page_small, 12, 7, 1U, 0U);
    s_fit(p_canvas, p_title, &g_weather_page_large, 12, 43, 272U, 1U, 0U);
    s_fit(p_canvas, p_subtitle, &g_weather_page_normal, 12, 86, 272U, 1U, 0U);
    s_alert_bar(p_canvas, p_batch);
}

static void s_summary(weather_page_canvas_t *p_canvas,
    const weather_feed_batch_t *p_batch, uint8_t alert_index)
{
    const weather_feed_alert_t *p_alert;
    const weather_page_font_t *p_font;
    const char *p_type;
    uint8_t scale;
    uint8_t ordinal = 0U;
    uint8_t i;
    char text[24];
    if (alert_index >= p_batch->alert_count) { s_alert_state(p_canvas, p_batch); return; }
    p_alert = &p_batch->alerts[alert_index];
    p_type = s_type(p_alert->type);
    p_font = &g_weather_page_large;
    scale = s_width(p_type, p_font, 2U) <= 164U ? 2U : 1U;
    s_text(p_canvas, "WEATHER ALERT", &g_weather_page_small, 12, 7, 1U, 0U);
    for (i = 0U; i <= alert_index; i++) {
        if (p_batch->alerts[i].expired == 0U) { ordinal++; }
    }
    if (p_alert->expired != 0U) { (void)snprintf(text, sizeof(text), "EXPIRED"); }
    else { (void)snprintf(text, sizeof(text), "%u/%u", (unsigned)ordinal, (unsigned)s_active_count(p_batch)); }
    s_fit(p_canvas, text, &g_weather_page_small, 220, 7, 64U, 2U, 0U);
    s_text(p_canvas, p_type, p_font, 12, 37, scale, 0U);
    s_rect(p_canvas, 181, 36, 103U, 30U, 1U);
    s_fit(p_canvas, s_color(p_alert->color), &g_weather_page_bold, 181, 42, 103U, 1U, 1U);
    s_text(p_canvas, p_alert->expired != 0U ? "EXPIRED" : (p_alert->pending != 0U ? "STARTS" : "UNTIL"),
        &g_weather_page_small, 181, 75, 1U, 0U);
    (void)snprintf(text, sizeof(text), "%.5s", s_stamp_time(p_alert->pending != 0U ? p_alert->effective : p_alert->expires));
    s_fit(p_canvas, text, &g_weather_page_number, 181, 90, 103U, 2U, 0U);
    s_rect(p_canvas, 12, 109, 272U, 1U, 1U);
    s_text(p_canvas, p_batch->city == WEATHER_CITY_SHENZHEN ? "SHENZHEN" : "ZHANJIANG",
        &g_weather_page_small, 12, 112, 1U, 0U);
    (void)snprintf(text, sizeof(text), "CHK %.5s", s_stamp_time(p_batch->checked));
    s_fit(p_canvas, text, &g_weather_page_small, 181, 112, 103U, 2U, 0U);
}

static void s_reading(weather_page_canvas_t *p_canvas,
    const weather_feed_batch_t *p_batch, uint8_t alert_index, uint16_t page,
    uint8_t force_source)
{
    const weather_feed_alert_t *p_alert;
    const char *p_text;
    const char *p_title;
    uint16_t body_pages;
    uint16_t guide_pages;
    uint16_t total;
    uint16_t local;
    uint8_t row;
    uint8_t source;
    weather_page_line_t line;
    char text[40];
    if (alert_index >= p_batch->alert_count) { s_alert_state(p_canvas, p_batch); return; }
    p_alert = &p_batch->alerts[alert_index];
    body_pages = p_alert->body[0] != '\0' ? s_pages(p_alert->body) : 0U;
    guide_pages = p_alert->guide[0] != '\0' ? s_pages(p_alert->guide) : 0U;
    total = epd_weather_pages_text_count(p_batch, alert_index);
    if (force_source != 0U) { page = (uint16_t)(body_pages + guide_pages + page); }
    if (page >= total) { page = (uint16_t)(total - 1U); }
    source = 0U;
    if (page < body_pages) {
        p_text = p_alert->body; p_title = "MESSAGE"; local = page;
    } else if (page < body_pages + guide_pages) {
        p_text = p_alert->guide; p_title = "GUIDANCE"; local = (uint16_t)(page - body_pages);
    } else {
        p_text = p_alert->source; p_title = "SOURCE";
        local = (uint16_t)(page - body_pages - guide_pages); source = 1U;
    }
    (void)snprintf(text, sizeof(text), "%s / %s", s_type(p_alert->type), s_color(p_alert->color));
    s_fit(p_canvas, source != 0U ? "SOURCE" : text, &g_weather_page_small, 12, 7, 216U, 0U, 0U);
    (void)snprintf(text, sizeof(text), "%u/%u", (unsigned)page + 1U, (unsigned)total);
    s_fit(p_canvas, text, &g_weather_page_small, 220, 7, 64U, 2U, 0U);
    s_rect(p_canvas, 12, 28, 272U, 1U, 1U);
    if (p_text[0] == '\0') {
        s_fit(p_canvas, "N/A", &g_weather_page_medium, 12, 41, 272U, 0U, 0U);
    }
    for (row = 0U; row < WEATHER_PAGE_TEXT_ROWS; row++) {
        memset(&line, 0, sizeof(line));
        (void)s_lines(p_text, (uint16_t)(local * WEATHER_PAGE_TEXT_ROWS + row), &line);
        if (line.p_begin != NULL) {
            s_utf8_line(p_canvas, p_batch, &line, 12, (int16_t)(source != 0U ? 34 + row * 28 : 41 + row * 36));
        }
    }
    if (source != 0U) {
        (void)snprintf(text, sizeof(text), "ISSUED %s", p_alert->issued[0] != '\0' ? p_alert->issued : "UNKNOWN");
        s_fit(p_canvas, text, &g_weather_page_small, 12, 94, 272U, 0U, 0U);
        (void)snprintf(text, sizeof(text), "UNTIL %s", p_alert->expires[0] != '\0' ? p_alert->expires : "UNKNOWN");
        s_fit(p_canvas, text, &g_weather_page_small, 12, 112, 272U, 0U, 0U);
    } else {
        s_text(p_canvas, p_title, &g_weather_page_small, 12, 113, 1U, 0U);
    }
}

static uint8_t s_draw(uint8_t *p_frame, size_t length,
    const weather_feed_batch_t *p_batch, weather_page_kind_t page,
    uint8_t alert_index, uint16_t text_page, uint8_t gray, uint8_t fresh,
    const clock_calendar_t *p_today)
{
    weather_page_canvas_t canvas;
    size_t required = gray != 0U ? EPD_CLOCK_VIEW_GRAY4_BYTES : EPD_CLOCK_VIEW_BW_BYTES;
    if (p_frame == NULL || p_batch == NULL || length < required ||
        page > WEATHER_PAGE_SOURCE || p_batch->day_count > WEATHER_FEED_MAX_DAYS ||
        p_batch->alert_count > WEATHER_FEED_MAX_ALERTS || p_batch->glyph_count > WEATHER_FEED_MAX_GLYPHS) { return 0U; }
    memset(p_frame, gray != 0U ? 0 : 0xFF, required);
    canvas.p_frame = p_frame; canvas.gray = gray; canvas.valid = 1U; canvas.fresh = fresh;
    canvas.p_today = p_today;
    switch (page) {
    case WEATHER_PAGE_LIVE: s_live(&canvas, p_batch); break;
    case WEATHER_PAGE_FORECAST: s_forecast(&canvas, p_batch); break;
    case WEATHER_PAGE_DATA: s_metrics(&canvas, p_batch); break;
    case WEATHER_PAGE_ALERT: s_summary(&canvas, p_batch, alert_index); break;
    case WEATHER_PAGE_TEXT: s_reading(&canvas, p_batch, alert_index, text_page, 0U); break;
    case WEATHER_PAGE_SOURCE: s_reading(&canvas, p_batch, alert_index, text_page, 1U); break;
    default: return 0U;
    }
    return canvas.valid;
}

uint8_t epd_weather_pages_draw_bw(uint8_t *p_frame, size_t length,
    const weather_feed_batch_t *p_batch, weather_page_kind_t page,
    uint8_t alert_index, uint16_t text_page, uint8_t fresh)
{
    return s_draw(p_frame, length, p_batch, page, alert_index, text_page, 0U, fresh, NULL);
}

uint8_t epd_weather_pages_draw_gray4(uint8_t *p_frame, size_t length,
    const weather_feed_batch_t *p_batch, weather_page_kind_t page,
    uint8_t alert_index, uint16_t text_page, uint8_t fresh)
{
    return s_draw(p_frame, length, p_batch, page, alert_index, text_page, 1U, fresh, NULL);
}

uint8_t epd_weather_pages_draw_bw_for_date(uint8_t *p_frame, size_t length,
    const weather_feed_batch_t *p_batch, weather_page_kind_t page,
    uint8_t alert_index, uint16_t text_page, uint8_t fresh,
    const clock_calendar_t *p_today)
{
    return s_draw(p_frame, length, p_batch, page, alert_index, text_page, 0U, fresh, p_today);
}

uint8_t epd_weather_pages_draw_gray4_for_date(uint8_t *p_frame, size_t length,
    const weather_feed_batch_t *p_batch, weather_page_kind_t page,
    uint8_t alert_index, uint16_t text_page, uint8_t fresh,
    const clock_calendar_t *p_today)
{
    return s_draw(p_frame, length, p_batch, page, alert_index, text_page, 1U, fresh, p_today);
}
