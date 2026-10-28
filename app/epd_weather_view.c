#include "epd_weather_view.h"

#include "epd_clock_view.h"
#include "../device/epd_gdey029t94.h"
#include "../resources/weather_glyphs.h"

#include <string.h>

#define WEATHER_BW_ROW_BYTES 16U
#define WEATHER_GRAY_ROW_BYTES 32U
#define WEATHER_BLACK_GRAY 3U

typedef struct {
    uint8_t *p_frame;
    uint8_t monochrome;
    uint8_t valid;
} weather_canvas_t;

static void s_pixel(weather_canvas_t *p_canvas, int16_t x, int16_t y,
                    uint8_t black)
{
    uint16_t native_x;
    uint16_t native_y;
    size_t offset;
    uint8_t shift;
    if ((x < 0) || (y < 0) || (x >= (int16_t)EPD_CLOCK_VIEW_WIDTH) ||
        (y >= (int16_t)EPD_CLOCK_VIEW_HEIGHT)) {
        p_canvas->valid = 0U;
        return;
    }
    native_x = (uint16_t)(127 - y);
    native_y = (uint16_t)(295 - x);
    if (p_canvas->monochrome != 0U) {
        offset = (size_t)native_y * WEATHER_BW_ROW_BYTES + native_x / 8U;
        shift = (uint8_t)(7U - native_x % 8U);
        if (black != 0U) {
            p_canvas->p_frame[offset] &= (uint8_t)~(1U << shift);
        } else {
            p_canvas->p_frame[offset] |= (uint8_t)(1U << shift);
        }
    } else {
        offset = (size_t)native_y * WEATHER_GRAY_ROW_BYTES + native_x / 4U;
        shift = (uint8_t)(6U - 2U * (native_x % 4U));
        if (black != 0U) {
            p_canvas->p_frame[offset] |= (uint8_t)(WEATHER_BLACK_GRAY << shift);
        } else {
            p_canvas->p_frame[offset] &= (uint8_t)~(3U << shift);
        }
    }
}

static void s_rect(weather_canvas_t *p_canvas, int16_t x, int16_t y,
                   int16_t width, int16_t height, uint8_t black)
{
    int16_t px;
    int16_t py;
    for (py = y; py < y + height; py++) {
        for (px = x; px < x + width; px++) { s_pixel(p_canvas, px, py, black); }
    }
}

static void s_line(weather_canvas_t *p_canvas, int16_t x0, int16_t y0,
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
        if ((x0 == x1) && (y0 == y1)) { break; }
        twice = (int16_t)(2 * error);
        if (twice >= dy) { error = (int16_t)(error + dy); x0 = (int16_t)(x0 + sx); }
        if (twice <= dx) { error = (int16_t)(error + dx); y0 = (int16_t)(y0 + sy); }
    }
}

static void s_circle(weather_canvas_t *p_canvas, int16_t cx, int16_t cy,
                     int16_t radius, uint8_t filled)
{
    int16_t x;
    int16_t y;
    int16_t distance;
    int16_t inner;
    for (y = (int16_t)-radius; y <= radius; y++) {
        for (x = (int16_t)-radius; x <= radius; x++) {
            distance = (int16_t)(x * x + y * y);
            inner = (int16_t)((radius - 1) * (radius - 1));
            if ((distance <= radius * radius) &&
                ((filled != 0U) || (distance >= inner))) {
                s_pixel(p_canvas, (int16_t)(cx + x), (int16_t)(cy + y), 1U);
            }
        }
    }
}

static uint8_t s_cloud_inside(int16_t x, int16_t y)
{
    int16_t dx;
    int16_t dy;
    uint8_t inside = (uint8_t)((x >= 8) && (x <= 34) && (y >= 20) && (y <= 29));
    dx = (int16_t)(x - 13); dy = (int16_t)(y - 20);
    inside = (uint8_t)(inside || (dx * dx + dy * dy <= 9 * 9));
    dx = (int16_t)(x - 22); dy = (int16_t)(y - 16);
    inside = (uint8_t)(inside || (dx * dx + dy * dy <= 12 * 12));
    dx = (int16_t)(x - 31); dy = (int16_t)(y - 21);
    return (uint8_t)(inside || (dx * dx + dy * dy <= 8 * 8));
}

static void s_cloud(weather_canvas_t *p_canvas, int16_t left, int16_t top)
{
    int16_t x;
    int16_t y;
    uint8_t boundary;
    for (y = 4; y <= 30; y++) {
        for (x = 1; x <= 38; x++) {
            if (s_cloud_inside(x, y) == 0U) { continue; }
            boundary = (uint8_t)(s_cloud_inside((int16_t)(x - 1), y) == 0U ||
                s_cloud_inside((int16_t)(x + 1), y) == 0U ||
                s_cloud_inside(x, (int16_t)(y - 1)) == 0U ||
                s_cloud_inside(x, (int16_t)(y + 1)) == 0U);
            s_pixel(p_canvas, (int16_t)(left + x), (int16_t)(top + y), boundary);
        }
    }
}

static void s_sun(weather_canvas_t *p_canvas, int16_t cx, int16_t cy,
                  int16_t radius)
{
    static const int8_t s_rays[8][4] = {
        {0, -1, 0, -1}, {1, -1, 1, -1}, {1, 0, 1, 0}, {1, 1, 1, 1},
        {0, 1, 0, 1}, {-1, 1, -1, 1}, {-1, 0, -1, 0}, {-1, -1, -1, -1}
    };
    int16_t i;
    int16_t inner;
    int16_t outer;
    s_circle(p_canvas, cx, cy, radius, 0U);
    inner = (int16_t)(radius + 3);
    outer = (int16_t)(radius + 6);
    for (i = 0; i < 8; i++) {
        s_line(p_canvas,
            (int16_t)(cx + s_rays[i][0] * inner),
            (int16_t)(cy + s_rays[i][1] * inner),
            (int16_t)(cx + s_rays[i][2] * outer),
            (int16_t)(cy + s_rays[i][3] * outer));
    }
}

static void s_snowflake(weather_canvas_t *p_canvas, int16_t cx, int16_t cy)
{
    s_line(p_canvas, (int16_t)(cx - 4), cy, (int16_t)(cx + 4), cy);
    s_line(p_canvas, cx, (int16_t)(cy - 4), cx, (int16_t)(cy + 4));
    s_line(p_canvas, (int16_t)(cx - 3), (int16_t)(cy - 3),
           (int16_t)(cx + 3), (int16_t)(cy + 3));
    s_line(p_canvas, (int16_t)(cx - 3), (int16_t)(cy + 3),
           (int16_t)(cx + 3), (int16_t)(cy - 3));
}

static void s_weather_icon(weather_canvas_t *p_canvas, uint8_t condition,
                           int16_t left, int16_t top)
{
    int16_t i;
    if (condition == 1U) {
        s_sun(p_canvas, (int16_t)(left + 20), (int16_t)(top + 19), 8);
    } else if (condition == 2U) {
        s_sun(p_canvas, (int16_t)(left + 29), (int16_t)(top + 10), 6);
        s_cloud(p_canvas, left, top);
    } else if (condition == 3U) {
        s_cloud(p_canvas, left, top);
    } else if ((condition >= 4U) && (condition <= 6U)) {
        s_cloud(p_canvas, left, top);
        for (i = 0; i < (int16_t)(condition - 3U); i++) {
            s_line(p_canvas, (int16_t)(left + 13 + i * 10), (int16_t)(top + 32),
                   (int16_t)(left + 10 + i * 10), (int16_t)(top + 37));
        }
    } else if (condition == 7U) {
        s_cloud(p_canvas, left, top);
        s_line(p_canvas, (int16_t)(left + 24), (int16_t)(top + 29),
               (int16_t)(left + 18), (int16_t)(top + 36));
        s_line(p_canvas, (int16_t)(left + 18), (int16_t)(top + 36),
               (int16_t)(left + 24), (int16_t)(top + 36));
        s_line(p_canvas, (int16_t)(left + 24), (int16_t)(top + 36),
               (int16_t)(left + 20), (int16_t)(top + 40));
    } else if ((condition >= 8U) && (condition <= 10U)) {
        s_cloud(p_canvas, left, top);
        for (i = 0; i < (int16_t)(condition - 7U); i++) {
            s_snowflake(p_canvas, (int16_t)(left + 13 + i * 10),
                        (int16_t)(top + 35));
        }
    } else if (condition == 11U) {
        s_cloud(p_canvas, left, top);
        s_line(p_canvas, (int16_t)(left + 13), (int16_t)(top + 32),
               (int16_t)(left + 10), (int16_t)(top + 37));
        s_snowflake(p_canvas, (int16_t)(left + 28), (int16_t)(top + 35));
    } else if (condition == 12U) {
        s_rect(p_canvas, (int16_t)(left + 5), (int16_t)(top + 10), 30, 2, 1U);
        s_rect(p_canvas, (int16_t)(left + 2), (int16_t)(top + 19), 34, 2, 1U);
        s_rect(p_canvas, (int16_t)(left + 5), (int16_t)(top + 28), 30, 2, 1U);
    } else if (condition == 13U) {
        s_line(p_canvas, (int16_t)(left + 4), (int16_t)(top + 13),
               (int16_t)(left + 10), (int16_t)(top + 9));
        s_line(p_canvas, (int16_t)(left + 10), (int16_t)(top + 9),
               (int16_t)(left + 16), (int16_t)(top + 13));
        s_line(p_canvas, (int16_t)(left + 16), (int16_t)(top + 13),
               (int16_t)(left + 22), (int16_t)(top + 9));
        s_line(p_canvas, (int16_t)(left + 22), (int16_t)(top + 9),
               (int16_t)(left + 28), (int16_t)(top + 13));
        s_line(p_canvas, (int16_t)(left + 28), (int16_t)(top + 13),
               (int16_t)(left + 34), (int16_t)(top + 9));
        s_line(p_canvas, (int16_t)(left + 4), (int16_t)(top + 25),
               (int16_t)(left + 10), (int16_t)(top + 21));
        s_line(p_canvas, (int16_t)(left + 10), (int16_t)(top + 21),
               (int16_t)(left + 16), (int16_t)(top + 25));
        s_line(p_canvas, (int16_t)(left + 16), (int16_t)(top + 25),
               (int16_t)(left + 22), (int16_t)(top + 21));
        s_line(p_canvas, (int16_t)(left + 22), (int16_t)(top + 21),
               (int16_t)(left + 28), (int16_t)(top + 25));
        s_line(p_canvas, (int16_t)(left + 28), (int16_t)(top + 25),
               (int16_t)(left + 34), (int16_t)(top + 21));
    } else {
        s_circle(p_canvas, (int16_t)(left + 20), (int16_t)(top + 19), 13, 0U);
        s_rect(p_canvas, (int16_t)(left + 19), (int16_t)(top + 10), 2, 13, 1U);
        s_circle(p_canvas, (int16_t)(left + 20), (int16_t)(top + 28), 1, 1U);
    }
}

static const weather_glyph_t *s_glyph(uint16_t codepoint)
{
    uint8_t first = 0U;
    uint8_t last = g_weather_glyph_count;
    uint8_t middle;
    while (first < last) {
        middle = (uint8_t)(first + (uint8_t)((last - first) / 2U));
        if (g_weather_glyphs[middle].codepoint < codepoint) {
            first = (uint8_t)(middle + 1U);
        } else {
            last = middle;
        }
    }
    if ((first >= g_weather_glyph_count) ||
        (g_weather_glyphs[first].codepoint != codepoint)) { return NULL; }
    return &g_weather_glyphs[first];
}

static const uint8_t *s_ascii_rows(char character)
{
    static const uint8_t s_digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12}
    };
    static const uint8_t s_dash[7] = {0,0,0,31,0,0,0};
    static const uint8_t s_dot[7] = {0,0,0,0,0,12,12};
    static const uint8_t s_colon[7] = {0,12,12,0,12,12,0};
    static const uint8_t s_percent[7] = {17,2,4,8,17,0,0};
    static const uint8_t s_c[7] = {14,17,16,16,16,17,14};
    static const uint8_t s_a[7] = {14,17,17,31,17,17,17};
    static const uint8_t s_m[7] = {17,27,21,21,17,17,17};
    static const uint8_t s_p[7] = {30,17,17,30,16,16,16};
    static const uint8_t s_blank[7] = {0,0,0,0,0,0,0};
    if ((character >= '0') && (character <= '9')) {
        return s_digits[(uint8_t)(character - '0')];
    }
    switch (character) {
    case '-': return s_dash;
    case '.': return s_dot;
    case ':': return s_colon;
    case '%': return s_percent;
    case 'C': return s_c;
    case 'A': return s_a;
    case 'M': return s_m;
    case 'P': return s_p;
    case ' ': return s_blank;
    default: return NULL;
    }
}

static uint8_t s_text_width(const char *p_text, uint16_t *p_width)
{
    const unsigned char *p_bytes = (const unsigned char *)p_text;
    uint16_t width = 0U;
    uint16_t codepoint;
    const weather_glyph_t *p_entry;
    const uint8_t *p_ascii;
    while (*p_bytes != 0U) {
        if (*p_bytes < 0x80U) {
            p_ascii = s_ascii_rows((char)*p_bytes++);
            if (p_ascii == NULL) { return 0U; }
            width = (uint16_t)(width + 12U);
        } else if (((p_bytes[0] & 0xF0U) == 0xE0U) &&
                   ((p_bytes[1] & 0xC0U) == 0x80U) &&
                   ((p_bytes[2] & 0xC0U) == 0x80U)) {
            codepoint = (uint16_t)(((p_bytes[0] & 0x0FU) << 12U) |
                ((p_bytes[1] & 0x3FU) << 6U) | (p_bytes[2] & 0x3FU));
            p_entry = s_glyph(codepoint);
            if (p_entry == NULL) { return 0U; }
            width = (uint16_t)(width + 16U);
            p_bytes += 3;
        } else {
            return 0U;
        }
    }
    *p_width = width;
    return 1U;
}

static uint8_t s_draw_text(weather_canvas_t *p_canvas, uint16_t x, uint16_t y,
                           uint16_t right, const char *p_text)
{
    const unsigned char *p_bytes = (const unsigned char *)p_text;
    const uint8_t *p_ascii;
    const weather_glyph_t *p_entry;
    uint16_t width;
    uint16_t codepoint;
    uint8_t row;
    uint8_t col;
    if (!s_text_width(p_text, &width) || (x + width > right) || (y + 16U > 128U)) {
        p_canvas->valid = 0U;
        return 0U;
    }
    while (*p_bytes != 0U) {
        if (*p_bytes < 0x80U) {
            p_ascii = s_ascii_rows((char)*p_bytes++);
            for (row = 0U; row < 7U; row++) {
                for (col = 0U; col < 5U; col++) {
                    if ((p_ascii[row] & (1U << (4U - col))) != 0U) {
                        s_rect(p_canvas, (int16_t)(x + col * 2U),
                            (int16_t)(y + row * 2U + 1U), 2, 2, 1U);
                    }
                }
            }
            x = (uint16_t)(x + 12U);
        } else {
            codepoint = (uint16_t)(((p_bytes[0] & 0x0FU) << 12U) |
                ((p_bytes[1] & 0x3FU) << 6U) | (p_bytes[2] & 0x3FU));
            p_entry = s_glyph(codepoint);
            for (row = 0U; row < 16U; row++) {
                for (col = 0U; col < 16U; col++) {
                    if ((p_entry->bitmap[row * 2U + col / 8U] &
                         (1U << (7U - col % 8U))) != 0U) {
                        s_pixel(p_canvas, (int16_t)(x + col), (int16_t)(y + row), 1U);
                    }
                }
            }
            p_bytes += 3;
            x = (uint16_t)(x + 16U);
        }
    }
    return p_canvas->valid;
}

static void s_format_tenths(char text[8], int16_t value)
{
    uint16_t magnitude = (uint16_t)(value < 0 ? -value : value);
    uint8_t index = 0U;
    uint16_t whole = (uint16_t)(magnitude / 10U);
    uint8_t reversed[3];
    uint8_t count = 0U;
    if (value < 0) { text[index++] = '-'; }
    do {
        reversed[count++] = (uint8_t)(whole % 10U);
        whole = (uint16_t)(whole / 10U);
    } while ((whole != 0U) && (count < 3U));
    while (count != 0U) { text[index++] = (char)('0' + reversed[--count]); }
    text[index++] = '.';
    text[index++] = (char)('0' + magnitude % 10U);
    text[index] = '\0';
}

static void s_format_unsigned(char text[8], uint16_t value)
{
    char reversed[5];
    uint8_t count = 0U;
    uint8_t index = 0U;
    do {
        reversed[count++] = (char)('0' + value % 10U);
        value = (uint16_t)(value / 10U);
    } while ((value != 0U) && (count < sizeof(reversed)));
    while (count != 0U) { text[index++] = reversed[--count]; }
    text[index] = '\0';
}

static void s_large_digit(weather_canvas_t *p_canvas, int16_t x, int16_t y,
                          uint8_t digit)
{
    static const uint8_t s_segments[10] = {
        0x3FU, 0x06U, 0x5BU, 0x4FU, 0x66U,
        0x6DU, 0x7DU, 0x07U, 0x7FU, 0x6FU
    };
    uint8_t mask = s_segments[digit];
    if ((mask & 0x01U) != 0U) { s_rect(p_canvas, x + 3, y, 10, 3, 1U); }
    if ((mask & 0x02U) != 0U) { s_rect(p_canvas, x + 13, y + 3, 3, 11, 1U); }
    if ((mask & 0x04U) != 0U) { s_rect(p_canvas, x + 13, y + 16, 3, 11, 1U); }
    if ((mask & 0x08U) != 0U) { s_rect(p_canvas, x + 3, y + 27, 10, 3, 1U); }
    if ((mask & 0x10U) != 0U) { s_rect(p_canvas, x, y + 16, 3, 11, 1U); }
    if ((mask & 0x20U) != 0U) { s_rect(p_canvas, x, y + 3, 3, 11, 1U); }
    if ((mask & 0x40U) != 0U) { s_rect(p_canvas, x + 3, y + 14, 10, 3, 1U); }
}

static void s_large_temperature(weather_canvas_t *p_canvas, int16_t value)
{
    uint16_t magnitude = (uint16_t)(value < 0 ? -value : value);
    uint8_t whole = (uint8_t)(magnitude / 10U);
    int16_t x = 180;
    int16_t y = 31;
    if (value < 0) {
        s_rect(p_canvas, x, y + 14, 9, 3, 1U);
        x = (int16_t)(x + 12);
    }
    if (whole >= 10U) {
        s_large_digit(p_canvas, x, y, (uint8_t)(whole / 10U));
        x = (int16_t)(x + 19);
    }
    s_large_digit(p_canvas, x, y, (uint8_t)(whole % 10U));
    x = (int16_t)(x + 19);
    s_rect(p_canvas, x, y + 25, 3, 3, 1U);
    x = (int16_t)(x + 5);
    s_large_digit(p_canvas, x, y, (uint8_t)(magnitude % 10U));
    s_circle(p_canvas, (int16_t)(x + 20), (int16_t)(y + 10), 2, 0U);
    (void)s_draw_text(p_canvas, (uint16_t)(x + 23), (uint16_t)(y + 13),
                      (uint16_t)(x + 35), "C");
}

static void s_large_placeholder(weather_canvas_t *p_canvas)
{
    s_rect(p_canvas, 193, 42, 12, 3, 1U);
    s_rect(p_canvas, 212, 42, 12, 3, 1U);
    s_rect(p_canvas, 228, 57, 3, 3, 1U);
}

static const char *s_condition_text(uint8_t condition)
{
    static const char *const s_text[] = {
        "\xE6\x9C\xAA\xE7\x9F\xA5", "\xE6\x99\xB4",
        "\xE5\xA4\x9A\xE4\xBA\x91", "\xE9\x98\xB4",
        "\xE5\xB0\x8F\xE9\x9B\xA8", "\xE4\xB8\xAD\xE9\x9B\xA8",
        "\xE5\xA4\xA7\xE9\x9B\xA8", "\xE9\x9B\xB7\xE9\x98\xB5\xE9\x9B\xA8",
        "\xE5\xB0\x8F\xE9\x9B\xAA", "\xE4\xB8\xAD\xE9\x9B\xAA",
        "\xE5\xA4\xA7\xE9\x9B\xAA", "\xE9\x9B\xA8\xE5\xA4\xB9\xE9\x9B\xAA",
        "\xE9\x9B\xBE", "\xE9\x9C\xBE"
    };
    return condition < (uint8_t)(sizeof(s_text) / sizeof(s_text[0])) ?
        s_text[condition] : NULL;
}

static const char *s_wind_text(uint8_t direction)
{
    static const char *const s_text[] = {
        "\xE9\x9D\x99\xE9\xA3\x8E", "\xE5\x8C\x97",
        "\xE4\xB8\x9C\xE5\x8C\x97", "\xE4\xB8\x9C",
        "\xE4\xB8\x9C\xE5\x8D\x97", "\xE5\x8D\x97",
        "\xE8\xA5\xBF\xE5\x8D\x97", "\xE8\xA5\xBF",
        "\xE8\xA5\xBF\xE5\x8C\x97"
    };
    return direction < (uint8_t)(sizeof(s_text) / sizeof(s_text[0])) ?
        s_text[direction] : NULL;
}

static void s_two_digits(char *p_text, uint8_t value)
{
    p_text[0] = (char)('0' + value / 10U);
    p_text[1] = (char)('0' + value % 10U);
}

static uint8_t s_draw(weather_canvas_t *p_canvas,
                      const weather_snapshot_t *p_weather,
                      const clock_view_model_t *p_clock)
{
    char text[12];
    char value[8];
    const char *p_label;
    uint8_t hour;
    uint8_t source_month;
    uint8_t source_day;

    if ((p_weather->available != 0U) && (p_weather->data.condition > 13U)) { return 0U; }
    if (p_weather->available != 0U && p_weather->data.city == WEATHER_CITY_ZHANJIANG) {
        (void)s_draw_text(p_canvas, 8U, 4U, 56U,
                         "\xE6\xB9\x9B\xE6\xB1\x9F");
    } else if (p_weather->available != 0U) {
        (void)s_draw_text(p_canvas, 8U, 4U, 56U,
                         "\xE6\xB7\xB1\xE5\x9C\xB3");
    } else {
        (void)s_draw_text(p_canvas, 8U, 4U, 56U,
                         "\xE5\xA4\xA9\xE6\xB0\x94");
    }
    if (p_weather->expired != 0U) {
        (void)s_draw_text(p_canvas, 52U, 4U, 108U,
                         "\xE5\xB7\xB2\xE8\xBF\x87\xE6\x9C\x9F");
    }
    if ((p_clock->calendar_state == CLOCK_DATA_READY) &&
        clock_calendar_valid(&p_clock->calendar)) {
        hour = p_clock->calendar.hour;
        if (p_clock->format == CLOCK_FORMAT_12) {
            (void)s_draw_text(p_canvas, 194U, 4U, 218U,
                hour >= 12U ? "PM" : "AM");
            hour = (uint8_t)(hour % 12U);
            if (hour == 0U) { hour = 12U; }
        }
        s_two_digits(text, hour);
        text[2] = ':';
        s_two_digits(&text[3], p_clock->calendar.minute);
        text[5] = '\0';
    } else {
        strcpy(text, "--:--");
    }
    (void)s_draw_text(p_canvas, 228U, 4U, 296U, text);
    s_line(p_canvas, 8, 24, 288, 24);

    if (p_weather->available != 0U) {
        s_weather_icon(p_canvas, p_weather->data.condition, 8, 27);
        p_label = s_condition_text(p_weather->data.condition);
        if (p_label == NULL) { return 0U; }
        (void)s_draw_text(p_canvas, 56U, 38U, 168U, p_label);
        s_large_temperature(p_canvas, p_weather->data.temperature_tenths);

        (void)s_draw_text(p_canvas, 8U, 72U, 44U,
                         "\xE6\x9C\x80\xE9\xAB\x98");
        s_format_tenths(value, p_weather->data.maximum_tenths);
        (void)s_draw_text(p_canvas, 48U, 72U, 148U, value);
        (void)s_draw_text(p_canvas, 158U, 72U, 194U,
                         "\xE6\x9C\x80\xE4\xBD\x8E");
        s_format_tenths(value, p_weather->data.minimum_tenths);
        (void)s_draw_text(p_canvas, 198U, 72U, 288U, value);

        (void)s_draw_text(p_canvas, 8U, 94U, 44U,
                         "\xE6\xB9\xBF\xE5\xBA\xA6");
        s_format_unsigned(text, p_weather->data.humidity);
        (void)s_draw_text(p_canvas, 48U, 94U, 100U, text);
        (void)s_draw_text(p_canvas, 100U, 94U, 136U,
                         "\xE9\xA3\x8E\xE5\x90\x91");
        p_label = s_wind_text(p_weather->data.wind_direction);
        if (p_label == NULL) { return 0U; }
        (void)s_draw_text(p_canvas, 140U, 94U, 184U, p_label);
        (void)s_draw_text(p_canvas, 190U, 94U, 226U,
                         "\xE9\xA3\x8E\xE9\x80\x9F");
        s_format_tenths(value, (int16_t)p_weather->data.wind_speed_tenths);
        (void)s_draw_text(p_canvas, 230U, 94U, 288U, value);

        (void)s_draw_text(p_canvas, 8U, 112U, 72U,
                         "\xE6\x9B\xB4\xE6\x96\xB0\xE6\x97\xB6\xE9\x97\xB4");
        source_month = (uint8_t)((p_weather->data.source_time[5] - '0') * 10 +
                                  p_weather->data.source_time[6] - '0');
        source_day = (uint8_t)((p_weather->data.source_time[8] - '0') * 10 +
                                p_weather->data.source_time[9] - '0');
        s_two_digits(text, source_month);
        text[2] = '-';
        s_two_digits(&text[3], source_day);
        text[5] = ' ';
        text[6] = p_weather->data.source_time[11];
        text[7] = p_weather->data.source_time[12];
        text[8] = ':';
        text[9] = p_weather->data.source_time[14];
        text[10] = p_weather->data.source_time[15];
        text[11] = '\0';
        (void)s_draw_text(p_canvas, 78U, 112U, 220U, text);
    } else {
        s_weather_icon(p_canvas, 0U, 8, 27);
        (void)s_draw_text(p_canvas, 56U, 38U, 168U,
                         "\xE7\xAD\x89\xE5\xBE\x85\xE5\xA4\xA9\xE6\xB0\x94\xE6\x95\xB0\xE6\x8D\xAE");
        s_large_placeholder(p_canvas);
        (void)s_draw_text(p_canvas, 8U, 72U, 44U,
                         "\xE6\x9C\x80\xE9\xAB\x98");
        (void)s_draw_text(p_canvas, 48U, 72U, 148U, "--.-");
        (void)s_draw_text(p_canvas, 158U, 72U, 194U,
                         "\xE6\x9C\x80\xE4\xBD\x8E");
        (void)s_draw_text(p_canvas, 198U, 72U, 288U, "--.-");
        (void)s_draw_text(p_canvas, 8U, 94U, 44U,
                         "\xE6\xB9\xBF\xE5\xBA\xA6");
        (void)s_draw_text(p_canvas, 48U, 94U, 92U, "--");
        (void)s_draw_text(p_canvas, 100U, 94U, 136U,
                         "\xE9\xA3\x8E\xE5\x90\x91");
        (void)s_draw_text(p_canvas, 140U, 94U, 184U, "--");
        (void)s_draw_text(p_canvas, 190U, 94U, 226U,
                         "\xE9\xA3\x8E\xE9\x80\x9F");
        (void)s_draw_text(p_canvas, 230U, 94U, 288U, "--.-");
        (void)s_draw_text(p_canvas, 8U, 112U, 72U,
                         "\xE6\x9B\xB4\xE6\x96\xB0\xE6\x97\xB6\xE9\x97\xB4");
        (void)s_draw_text(p_canvas, 78U, 112U, 220U, "-- --:--");
    }
    s_line(p_canvas, 8, 68, 288, 68);
    s_line(p_canvas, 8, 91, 288, 91);
    s_line(p_canvas, 8, 110, 288, 110);
    return p_canvas->valid;
}

static uint8_t s_draw_page(uint8_t *p_frame, size_t length,
                           const weather_snapshot_t *p_weather,
                           const clock_view_model_t *p_clock, uint8_t monochrome)
{
    weather_canvas_t canvas;
    size_t required = monochrome != 0U ? EPD_CLOCK_VIEW_BW_BYTES :
                                        EPD_CLOCK_VIEW_GRAY4_BYTES;
    if ((p_frame == NULL) || (length < required) || (p_weather == NULL) ||
        (p_clock == NULL) || (p_clock->format != CLOCK_FORMAT_12 &&
                             p_clock->format != CLOCK_FORMAT_24)) { return 0U; }
    memset(p_frame, monochrome != 0U ? 0xFF : 0x00, required);
    canvas.p_frame = p_frame;
    canvas.monochrome = monochrome;
    canvas.valid = 1U;
    if (!s_draw(&canvas, p_weather, p_clock)) { return 0U; }
    return canvas.valid;
}

uint8_t epd_weather_view_draw_bw(uint8_t *p_frame, size_t length,
                                 const weather_snapshot_t *p_weather,
                                 const clock_view_model_t *p_clock)
{
    return s_draw_page(p_frame, length, p_weather, p_clock, 1U);
}

uint8_t epd_weather_view_draw_gray4(uint8_t *p_frame, size_t length,
                                    const weather_snapshot_t *p_weather,
                                    const clock_view_model_t *p_clock)
{
    return s_draw_page(p_frame, length, p_weather, p_clock, 0U);
}
