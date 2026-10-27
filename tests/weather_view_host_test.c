#include "../app/epd_weather_view.h"
#include "../app/epd_clock_view.h"
#include "../app/weather.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t s_bw_frame[EPD_CLOCK_VIEW_BW_BYTES];
static uint8_t s_gray_frame[EPD_CLOCK_VIEW_GRAY4_BYTES];

static uint32_t frame_hash(const uint8_t *p_frame, size_t length)
{
    uint32_t hash = 2166136261UL;
    size_t i;
    for (i = 0U; i < length; i++) { hash = (hash ^ p_frame[i]) * 16777619UL; }
    return hash;
}

static void write_preview(const char *p_path, const uint8_t *p_frame)
{
    FILE *p_file;
    uint16_t x;
    uint16_t y;
    uint16_t native_x;
    uint16_t native_y;
    size_t offset;
    uint8_t shift;
    uint8_t pixel;
    p_file = fopen(p_path, "wb");
    assert(p_file != NULL);
    assert(fprintf(p_file, "P5\n296 128\n255\n") > 0);
    for (y = 0U; y < 128U; y++) {
        for (x = 0U; x < 296U; x++) {
            native_x = (uint16_t)(127U - y);
            native_y = (uint16_t)(295U - x);
            offset = (size_t)native_y * 16U + native_x / 8U;
            shift = (uint8_t)(7U - native_x % 8U);
            pixel = (p_frame[offset] & (1U << shift)) != 0U ? 255U : 0U;
            assert(fwrite(&pixel, 1U, 1U, p_file) == 1U);
        }
    }
    assert(fclose(p_file) == 0);
}

static void check_gray4(void)
{
    uint16_t x;
    uint16_t y;
    uint8_t value;
    for (y = 0U; y < 128U; y++) {
        for (x = 0U; x < 296U; x++) {
            uint16_t native_x = (uint16_t)(127U - y);
            uint16_t native_y = (uint16_t)(295U - x);
            size_t offset = (size_t)native_y * 32U + native_x / 4U;
            uint8_t shift = (uint8_t)(6U - 2U * (native_x % 4U));
            value = (uint8_t)((s_gray_frame[offset] >> shift) & 3U);
            assert(value == 0U || value == 3U);
        }
    }
}

static uint8_t frame_black(const uint8_t *p_frame, uint8_t gray4,
                           uint16_t x, uint16_t y)
{
    uint16_t native_x = (uint16_t)(127U - y);
    uint16_t native_y = (uint16_t)(295U - x);
    size_t offset;
    uint8_t shift;
    if (gray4 != 0U) {
        offset = (size_t)native_y * 32U + native_x / 4U;
        shift = (uint8_t)(6U - 2U * (native_x % 4U));
        return (uint8_t)(((p_frame[offset] >> shift) & 3U) == 3U);
    }
    offset = (size_t)native_y * 16U + native_x / 8U;
    shift = (uint8_t)(7U - native_x % 8U);
    return (uint8_t)((p_frame[offset] & (1U << shift)) == 0U);
}

static void check_celsius_unit(const uint8_t *p_frame, uint8_t gray4,
                               int16_t temperature, uint16_t left)
{
    /* Independent visual reference: a small degree ring followed by a C.
     * The left stroke is present and the right side stays open in the C. */
    static const char s_reference[24][18] = {
        ".................", ".................",
        "..#..............", ".###.............",
        "##.##............", ".###.............",
        "..#..............", ".................",
        ".......######....", ".......######....",
        ".....##......##..", ".....##......##..",
        ".....##..........", ".....##..........",
        ".....##..........", ".....##..........",
        ".....##..........", ".....##..........",
        ".....##......##..", ".....##......##..",
        ".......######....", ".......######....",
        ".................", "................."
    };
    uint16_t row;
    uint16_t col;
    uint8_t expected;
    uint8_t actual;
    for (row = 0U; row < 24U; row++) {
        for (col = 0U; col < 17U; col++) {
            expected = (uint8_t)(s_reference[row][col] == '#');
            actual = frame_black(p_frame, gray4, (uint16_t)(left + col),
                                  (uint16_t)(37U + row));
            if (expected != actual) {
                fprintf(stderr, "Celsius reference mismatch: temp=%d %s at (%u,%u)\n",
                    (int)temperature, gray4 != 0U ? "GRAY4" : "BW",
                    (unsigned)(left + col), (unsigned)(37U + row));
            }
            assert(expected == actual);
        }
    }
}

static void check_temperature_units(weather_snapshot_t *p_weather,
                                     const clock_view_model_t *p_clock)
{
    static const struct {
        int16_t temperature;
        uint16_t unit_left;
    } s_cases[] = {
        {235, 241U}, {-235, 253U}, {999, 241U}, {-999, 253U},
        {0, 222U}, {99, 222U}, {-1, 234U}
    };
    size_t i;
    int16_t original_temperature = p_weather->data.temperature_tenths;
    for (i = 0U; i < sizeof(s_cases) / sizeof(s_cases[0]); i++) {
        p_weather->data.temperature_tenths = s_cases[i].temperature;
        assert(epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame),
                                        p_weather, p_clock));
        check_celsius_unit(s_bw_frame, 0U, s_cases[i].temperature,
                            s_cases[i].unit_left);
        assert(epd_weather_view_draw_gray4(s_gray_frame, sizeof(s_gray_frame),
                                           p_weather, p_clock));
        check_celsius_unit(s_gray_frame, 1U, s_cases[i].temperature,
                            s_cases[i].unit_left);
        check_gray4();
    }
    p_weather->data.temperature_tenths = original_temperature;
}

int main(int argc, char **argv)
{
    static const char *const lines[2] = {
        "WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,2,235,180,270,65,2,35,2026-10-26T10:30",
        "WEATHER \xE6\xB9\x9B\xE6\xB1\x9F,13,-999,-999,999,100,8,999,2026-10-26T10:30"
    };
    clock_view_model_t clock;
    weather_data_t data;
    weather_snapshot_t weather;
    uint32_t hashes[14];
    uint32_t hash;
    uint32_t i;

    memset(&clock, 0, sizeof(clock));
    clock.calendar_state = CLOCK_DATA_READY;
    clock.calendar = (clock_calendar_t){2026U, 10U, 27U, 2U, 0U, 5U, 0U};
    clock.format = CLOCK_FORMAT_12;
    memset(&weather, 0, sizeof(weather));
    assert(weather_parse_line(lines[0], &data));
    weather.data = data;
    weather.available = 1U;
    weather.fresh = 1U;
    check_temperature_units(&weather, &clock);
    for (i = 0U; i < 14U; i++) {
        weather.data.condition = (uint8_t)i;
        if (!epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame), &weather, &clock)) {
            fprintf(stderr, "weather renderer rejected condition %lu\n", (unsigned long)i);
            return 1;
        }
        hashes[i] = frame_hash(s_bw_frame, sizeof(s_bw_frame));
        for (hash = 0U; hash < i; hash++) { assert(hashes[hash] != hashes[i]); }
    }
    assert(weather_parse_line(lines[1], &data));
    weather.data = data;
    weather.available = 1U;
    weather.fresh = 0U;
    weather.expired = 1U;
    assert(epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame), &weather, &clock));
    weather.data.temperature_tenths = 999;
    assert(epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame), &weather, &clock));
    weather.data.temperature_tenths = -999;
    assert(epd_weather_view_draw_gray4(s_gray_frame, sizeof(s_gray_frame), &weather, &clock));
    check_gray4();
    assert(!epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame) - 1U, &weather, &clock));
    weather.data.condition = 14U;
    assert(!epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame), &weather, &clock));
    weather.data.condition = 13U;
    weather.available = 0U;
    weather.expired = 0U;
    assert(epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame), &weather, &clock));
    clock.calendar_state = CLOCK_DATA_ERROR;
    clock.format = CLOCK_FORMAT_24;
    assert(epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame), &weather, &clock));
    if (argc > 1) {
        assert(weather_parse_line(lines[0], &data));
        memset(&weather, 0, sizeof(weather));
        weather.data = data;
        weather.available = 1U;
        weather.fresh = 1U;
        clock.calendar_state = CLOCK_DATA_READY;
        clock.calendar.hour = 10U;
        clock.calendar.minute = 30U;
        assert(epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame), &weather, &clock));
        write_preview(argv[1], s_bw_frame);
    }
    if (argc > 2) {
        assert(weather_parse_line(lines[1], &weather.data));
        weather.expired = 1U;
        weather.fresh = 0U;
        assert(epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame), &weather, &clock));
        write_preview(argv[2], s_bw_frame);
    }
    if (argc > 3) {
        weather.data.temperature_tenths = 999;
        assert(epd_weather_view_draw_bw(s_bw_frame, sizeof(s_bw_frame), &weather, &clock));
        write_preview(argv[3], s_bw_frame);
    }
    puts("PASS: weather C renderer covers 14 conditions, extrema, waiting, expiry and 1/2bpp; Celsius references pass");
    return 0;
}
