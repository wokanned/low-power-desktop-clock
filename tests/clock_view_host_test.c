/* Host checks exercise the production C rasterizer, not a parallel mock UI. */
#include "../app/epd_clock_view.h"
#include "../resources/clock_gray_assets.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_BYTES 32U
#define FRAME_BYTES EPD_CLOCK_VIEW_GRAY4_BYTES
#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        s_failures++; \
    } \
} while (0)

static uint8_t s_guarded[FRAME_BYTES + 2U * GUARD_BYTES];
static uint8_t s_previous[FRAME_BYTES];
static uint8_t *s_frame = &s_guarded[GUARD_BYTES];
static unsigned long s_checks;
static unsigned int s_failures;

static clock_view_model_t normal_model(void)
{
    clock_view_model_t model;
    memset(&model, 0, sizeof(model));
    model.calendar.year = 2026U;
    model.calendar.month = 10U;
    model.calendar.day = 18U;
    model.calendar.weekday = 7U;
    model.calendar.hour = 21U;
    model.calendar.minute = 30U;
    model.calendar_state = CLOCK_DATA_READY;
    model.format = CLOCK_FORMAT_24;
    model.temperature.state = CLOCK_DATA_READY;
    model.temperature.value = 246;
    model.battery.state = CLOCK_DATA_READY;
    model.battery.value = 82;
    model.wifi.state = CLOCK_DATA_READY;
    model.wifi.value = 2;
    model.memory[CLOCK_MEMORY_SRAM].state = CLOCK_DATA_READY;
    model.memory[CLOCK_MEMORY_SRAM].value = 42;
    model.memory[CLOCK_MEMORY_IFLASH].state = CLOCK_DATA_READY;
    model.memory[CLOCK_MEMORY_IFLASH].value = 31;
    model.memory[CLOCK_MEMORY_W25Q].state = CLOCK_DATA_UNAVAILABLE;
    return model;
}

static void reset_guards(void)
{
    memset(s_guarded, 0xA5, sizeof(s_guarded));
}

static void check_guards(void)
{
    size_t i;
    for (i = 0U; i < GUARD_BYTES; i++) {
        CHECK(s_guarded[i] == 0xA5U);
        CHECK(s_guarded[GUARD_BYTES + FRAME_BYTES + i] == 0xA5U);
    }
}

/* Independently decode native row/pair packing into accepted landscape view. */
static uint8_t frame_pixel(const uint8_t *p_frame, uint16_t x, uint16_t y)
{
    size_t row = 295U - x;
    size_t column = 127U - y;
    uint8_t packed = p_frame[row * 32U + column / 4U];
    return (uint8_t)((packed / (1U << (2U * (3U - column % 4U)))) % 4U);
}

static uint8_t logical_pixel(uint16_t x, uint16_t y)
{
    return frame_pixel(s_frame, x, y);
}

static const clock_gray_glyph_t *font_glyph(clock_gray_font_id_t font_id, char code)
{
    const clock_gray_font_t *p_font = &g_clock_gray_fonts[font_id];
    uint8_t i;
    for (i = 0U; i < p_font->count; i++) {
        if (p_font->p_glyphs[i].code == (uint8_t)code) {
            return &p_font->p_glyphs[i];
        }
    }
    return NULL;
}

static uint8_t glyph_pixel(const clock_gray_font_t *p_font,
                           const clock_gray_glyph_t *p_glyph, uint16_t x, uint16_t y)
{
    uint16_t row_bytes = (uint16_t)((p_glyph->width + 3U) / 4U);
    return (uint8_t)((p_font->p_data[p_glyph->offset + y * row_bytes + x / 4U] >>
                     (6U - 2U * (x % 4U))) & 3U);
}

static uint8_t frame_regions_equal(uint16_t left, uint16_t top,
                                   uint16_t right, uint16_t bottom)
{
    uint16_t x;
    uint16_t y;
    for (y = top; y < bottom; y++) {
        for (x = left; x < right; x++) {
            if (logical_pixel(x, y) != frame_pixel(s_previous, x, y)) {
                return 0U;
            }
        }
    }
    return 1U;
}

static void write_preview(const char *p_directory, const char *p_name)
{
    char path[4096];
    FILE *p_file;
    uint16_t x;
    uint16_t y;
    int result;
    result = snprintf(path, sizeof(path), "%s/%s.pgm", p_directory, p_name);
    CHECK(result > 0 && (size_t)result < sizeof(path));
    p_file = fopen(path, "wb");
    CHECK(p_file != NULL);
    if (p_file == NULL) {
        return;
    }
    (void)fprintf(p_file, "P5\n296 128\n255\n");
    for (y = 0U; y < 128U; y++) {
        for (x = 0U; x < 296U; x++) {
            (void)fputc(255U - logical_pixel(x, y) * 85U, p_file);
        }
    }
    CHECK(ferror(p_file) == 0);
    CHECK(fclose(p_file) == 0);
}

static void test_inputs(void)
{
    clock_view_model_t model = normal_model();
    reset_guards();
    CHECK(epd_clock_view_draw_gray4(NULL, FRAME_BYTES, &model) == 0U);
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, NULL) == 0U);
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES - 1U, &model) == 0U);
    CHECK(s_frame[0] == 0xA5U && s_frame[FRAME_BYTES - 1U] == 0xA5U);
    model.memory_mode = (clock_memory_mode_t)CLOCK_MEMORY_COUNT;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 0U);
    model.memory_mode = CLOCK_MEMORY_SRAM;
    model.format = (clock_format_t)0;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 0U);
    check_guards();
}

static void test_fonts_and_orientation(void)
{
    clock_view_model_t model = normal_model();
    unsigned int colors = 0U;
    uint16_t x;
    uint16_t y;
    uint8_t i;
    uint8_t j;
    const clock_gray_font_t *p_font;
    const clock_gray_glyph_t *p_glyph;
    uint32_t required;
    reset_guards();
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    for (y = 0U; y < 128U; y++) {
        for (x = 0U; x < 296U; x++) {
            colors |= 1U << logical_pixel(x, y);
        }
    }
    CHECK(colors == 15U);
    for (y = 10U; y < 118U; y++) {
        CHECK(logical_pixel(160U, y) == 2U);
        CHECK(logical_pixel(228U, y) == 1U);
    }
    CHECK(logical_pixel(0U, 0U) == 0U);
    CHECK(logical_pixel(295U, 0U) == 0U);
    CHECK(logical_pixel(0U, 127U) == 0U);
    CHECK(logical_pixel(295U, 127U) == 0U);
    CHECK(logical_pixel(8U, 26U) == 0U);
    /* First landscape divider sample addresses native row135, column117. */
    CHECK(((s_frame[135U * 32U + 29U] >> 4U) & 3U) == 2U);
    for (i = 0U; i < CLOCK_FONT_COUNT; i++) {
        p_font = &g_clock_gray_fonts[i];
        CHECK(p_font->count > 0U);
        for (j = 0U; j < p_font->count; j++) {
            p_glyph = &p_font->p_glyphs[j];
            CHECK(p_glyph->advance > 0U);
            CHECK(p_glyph->y_offset >= -2);
            required = (uint32_t)((p_glyph->width + 3U) / 4U) * p_glyph->height;
            if (j + 1U < p_font->count) {
                CHECK(p_glyph->offset + required == p_font->p_glyphs[j + 1U].offset);
            }
        }
    }
    check_guards();
}

static void test_small_font_readability(void)
{
    static const clock_gray_font_id_t fonts[] = {
        CLOCK_FONT_DATE, CLOCK_FONT_LABEL, CLOCK_FONT_VALUE, CLOCK_FONT_WIFI
    };
    const clock_gray_font_t *p_font;
    const clock_gray_glyph_t *p_glyph;
    size_t font_index;
    uint16_t x;
    uint16_t y;
    uint16_t row_bytes;
    uint16_t black_pixels;
    uint8_t glyph_index;
    uint8_t icon_index;
    uint8_t color;
    const clock_gray_icon_t *p_icon;
    for (font_index = 0U; font_index < sizeof(fonts) / sizeof(fonts[0]); font_index++) {
        p_font = &g_clock_gray_fonts[fonts[font_index]];
        for (glyph_index = 0U; glyph_index < p_font->count; glyph_index++) {
            p_glyph = &p_font->p_glyphs[glyph_index];
            row_bytes = (uint16_t)((p_glyph->width + 3U) / 4U);
            black_pixels = 0U;
            if (p_glyph->code != (uint8_t)' ') {
                CHECK(p_glyph->width > 0U && p_glyph->height > 0U);
                CHECK(p_glyph->advance >= p_glyph->width + 1U);
            }
            for (y = 0U; y < p_glyph->height; y++) {
                for (x = 0U; x < p_glyph->width; x++) {
                    color = (uint8_t)((p_font->p_data[p_glyph->offset +
                        y * row_bytes + x / 4U] >> (6U - 2U * (x % 4U))) & 3U);
                    CHECK(color == 0U || color == 3U);
                    black_pixels = (uint16_t)(black_pixels + (color == 3U ? 1U : 0U));
                }
            }
            CHECK(p_glyph->code == (uint8_t)' ' || black_pixels > 0U);
        }
    }
    for (icon_index = 0U; icon_index < CLOCK_ICON_COUNT; icon_index++) {
        if (icon_index == CLOCK_ICON_MOON) {
            continue;
        }
        p_icon = &g_clock_gray_icons[icon_index];
        row_bytes = (uint16_t)((p_icon->width + 3U) / 4U);
        black_pixels = 0U;
        for (y = 0U; y < p_icon->height; y++) {
            for (x = 0U; x < p_icon->width; x++) {
                color = (uint8_t)((p_icon->p_data[y * row_bytes + x / 4U] >>
                                  (6U - 2U * (x % 4U))) & 3U);
                CHECK(color == 0U || color == 3U);
                black_pixels = (uint16_t)(black_pixels + (color == 3U ? 1U : 0U));
            }
        }
        CHECK(black_pixels > 0U);
    }
}

static void test_main_font_cells(void)
{
    const clock_gray_font_t *p_font = &g_clock_gray_fonts[CLOCK_FONT_MAIN];
    const clock_gray_glyph_t *p_glyph;
    uint16_t y;
    uint16_t x;
    uint16_t black_pixels;
    uint8_t i;
    uint8_t cell_width;
    for (i = 0U; i < p_font->count; i++) {
        p_glyph = &p_font->p_glyphs[i];
        cell_width = p_glyph->code == (uint8_t)':' ? 12U : 34U;
        CHECK(p_glyph->width == cell_width);
        CHECK(p_glyph->advance == cell_width);
        black_pixels = 0U;
        for (y = 0U; y < p_glyph->height; y++) {
            CHECK(glyph_pixel(p_font, p_glyph, 0U, y) == 0U);
            CHECK(glyph_pixel(p_font, p_glyph, (uint16_t)(cell_width - 1U), y) == 0U);
            for (x = 0U; x < p_glyph->width; x++) {
                black_pixels = (uint16_t)(black_pixels +
                    (glyph_pixel(p_font, p_glyph, x, y) == 3U ? 1U : 0U));
            }
        }
        CHECK(black_pixels > 0U);
    }
    CHECK(4U * 34U + 12U == 148U);
}

static uint8_t main_cell_matches(char code, uint16_t left)
{
    const clock_gray_font_t *p_font = &g_clock_gray_fonts[CLOCK_FONT_MAIN];
    const clock_gray_glyph_t *p_glyph = font_glyph(CLOCK_FONT_MAIN, code);
    uint16_t x;
    uint16_t y;
    int16_t glyph_y;
    uint8_t expected;
    if (p_glyph == NULL) {
        return 0U;
    }
    for (y = 24U; y < 96U; y++) {
        glyph_y = (int16_t)y - 28 - p_glyph->y_offset;
        for (x = 0U; x < p_glyph->width; x++) {
            expected = 0U;
            if ((glyph_y >= 0) && (glyph_y < p_glyph->height)) {
                expected = glyph_pixel(p_font, p_glyph, x, (uint16_t)glyph_y);
            }
            if (logical_pixel((uint16_t)(left + x), y) != expected) {
                return 0U;
            }
        }
    }
    return 1U;
}

static void test_main_cell_layout(void)
{
    clock_view_model_t model = normal_model();
    uint16_t x;
    uint16_t y;
    model.calendar.hour = 4U;
    model.calendar.minute = 44U;
    reset_guards();
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    CHECK(main_cell_matches('0', 8U));
    CHECK(main_cell_matches('4', 42U));
    CHECK(main_cell_matches(':', 76U));
    CHECK(main_cell_matches('4', 88U));
    CHECK(main_cell_matches('4', 122U));
    for (y = 24U; y < 96U; y++) {
        for (x = 156U; x < 160U; x++) {
            CHECK(logical_pixel(x, y) == 0U);
        }
    }
    model.calendar_state = CLOCK_DATA_UNAVAILABLE;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    CHECK(main_cell_matches('-', 8U));
    CHECK(main_cell_matches('-', 42U));
    CHECK(main_cell_matches(':', 76U));
    CHECK(main_cell_matches('-', 88U));
    CHECK(main_cell_matches('-', 122U));
    check_guards();
}

static void test_minute_alignment(void)
{
    clock_view_model_t model = normal_model();
    uint8_t format;
    uint8_t minute;
    uint8_t hour;
    reset_guards();
    for (format = 0U; format < 2U; format++) {
        model.format = format == 0U ? CLOCK_FORMAT_24 : CLOCK_FORMAT_12;
        for (minute = 0U; minute < 60U; minute++) {
            model.calendar.minute = minute;
            model.calendar.hour = 0U;
            CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
            memcpy(s_previous, s_frame, FRAME_BYTES);
            for (hour = 1U; hour < 24U; hour++) {
                model.calendar.hour = hour;
                CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
                CHECK(frame_regions_equal(88U, 24U, 156U, 96U));
            }
        }
    }
    check_guards();
}

static void test_date_without_calendar_icon(void)
{
    static const char text[] = "2026-10-18 SUN";
    const clock_gray_font_t *p_font = &g_clock_gray_fonts[CLOCK_FONT_DATE];
    const clock_gray_glyph_t *p_glyph;
    clock_view_model_t model = normal_model();
    uint8_t expected[22U][160U];
    uint16_t left = 8U;
    uint16_t x;
    uint16_t y;
    int16_t glyph_y;
    uint8_t i;
    uint8_t first_column_ink = 0U;
    memset(expected, 0, sizeof(expected));
    for (i = 0U; text[i] != '\0'; i++) {
        p_glyph = font_glyph(CLOCK_FONT_DATE, text[i]);
        CHECK(p_glyph != NULL);
        if (p_glyph == NULL) {
            return;
        }
        for (y = 0U; y < p_glyph->height; y++) {
            glyph_y = (int16_t)(102 + p_glyph->y_offset + y - 98);
            CHECK(glyph_y >= 0 && glyph_y < 22);
            CHECK(left + p_glyph->width <= 156U);
            if ((glyph_y < 0) || (glyph_y >= 22) ||
                (left + p_glyph->width > 156U)) {
                return;
            }
            for (x = 0U; x < p_glyph->width; x++) {
                expected[glyph_y][left + x] = glyph_pixel(p_font, p_glyph, x, y);
            }
        }
        left = (uint16_t)(left + p_glyph->advance);
    }
    reset_guards();
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    for (y = 98U; y < 120U; y++) {
        for (x = 0U; x < 160U; x++) {
            CHECK(logical_pixel(x, y) == expected[y - 98U][x]);
        }
        first_column_ink = (uint8_t)(first_column_ink | logical_pixel(8U, y));
    }
    CHECK(first_column_ink != 0U);
    check_guards();
}

static void test_times(void)
{
    clock_view_model_t model = normal_model();
    uint16_t minute;
    uint8_t mode;
    uint8_t format;
    reset_guards();
    for (format = 0U; format < 2U; format++) {
        model.format = format == 0U ? CLOCK_FORMAT_24 : CLOCK_FORMAT_12;
        for (mode = 0U; mode < CLOCK_MEMORY_COUNT; mode++) {
            model.memory_mode = (clock_memory_mode_t)mode;
            for (minute = 0U; minute < 1440U; minute++) {
                model.calendar.hour = (uint8_t)(minute / 60U);
                model.calendar.minute = (uint8_t)(minute % 60U);
                CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
                if (minute > 0U) {
                    CHECK(memcmp(s_frame, s_previous, FRAME_BYTES) != 0);
                }
                memcpy(s_previous, s_frame, FRAME_BYTES);
                model.calendar.second = 59U;
                CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
                CHECK(memcmp(s_frame, s_previous, FRAME_BYTES) == 0);
                model.calendar.second = 0U;
            }
            check_guards();
        }
    }
}

static void test_dates(void)
{
    clock_view_model_t model = normal_model();
    uint32_t dates = 0U;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    reset_guards();
    for (year = 2000U; year <= 2099U; year++) {
        model.calendar.year = year;
        for (month = 1U; month <= 12U; month++) {
            model.calendar.month = month;
            for (day = 1U; day <= 31U; day++) {
                model.calendar.day = day;
                model.calendar.weekday = clock_calendar_weekday(year, month, day);
                if (clock_calendar_valid(&model.calendar)) {
                    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
                    dates++;
                }
            }
        }
    }
    CHECK(dates == 36525U);
    check_guards();
}

static void test_missing_and_ranges(void)
{
    static const int32_t temperatures[] = {INT_MIN, -1000, -999, -400, -1, 0,
                                          1, 246, 999, 1250, 9999, 10000, INT_MAX};
    static const int32_t percents[] = {INT_MIN, -1, 0, 1, 99, 100, 101, INT_MAX};
    static const int32_t hours[] = {INT_MIN, -1, 0, 1, 9, 10, 98, 99, 100, INT_MAX};
    clock_view_model_t model = normal_model();
    size_t i;
    uint8_t state;
    reset_guards();
    for (state = CLOCK_DATA_UNAVAILABLE; state <= CLOCK_DATA_ERROR; state++) {
        model.calendar_state = (clock_data_state_t)state;
        model.temperature.state = (clock_data_state_t)state;
        model.battery.state = (clock_data_state_t)state;
        model.wifi.state = (clock_data_state_t)state;
        model.memory[CLOCK_MEMORY_SRAM].state = (clock_data_state_t)state;
        CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    }
    model = normal_model();
    for (i = 0U; i < sizeof(temperatures) / sizeof(temperatures[0]); i++) {
        model.temperature.value = temperatures[i];
        CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    }
    for (i = 0U; i < sizeof(percents) / sizeof(percents[0]); i++) {
        model.battery.value = percents[i];
        model.memory[CLOCK_MEMORY_SRAM].value = percents[i];
        CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    }
    for (i = 0U; i < sizeof(hours) / sizeof(hours[0]); i++) {
        model.wifi.value = hours[i];
        CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    }
    check_guards();
}

static void all_faults(clock_view_model_t *p_model)
{
    uint8_t i;
    clock_fault_category_t category;
    p_model->faults.active_categories = CLOCK_CATEGORY_COUNT;
    for (i = 0U; i < CLOCK_FAULT_SOURCE_COUNT; i++) {
        category = clock_fault_category((clock_fault_source_t)i);
        p_model->faults.source[i].active_mask = CLOCK_FAULT_TIMEOUT;
        p_model->faults.source[i].first_mask = CLOCK_FAULT_TIMEOUT;
        p_model->faults.category[category].source_bits |= 1UL << i;
        p_model->faults.category[category].first_time = p_model->calendar;
        p_model->faults.category[category].time_valid = 1U;
    }
}

static void test_faults(void)
{
    static const uint32_t reasons[] = {0U, CLOCK_FAULT_IO, CLOCK_FAULT_CONFIGURATION,
        CLOCK_FAULT_TIMEOUT, CLOCK_FAULT_ID, CLOCK_FAULT_VERIFY, CLOCK_FAULT_START,
        CLOCK_FAULT_TX, CLOCK_FAULT_RX, CLOCK_FAULT_OVERFLOW, 0xFFFFFFFFUL};
    clock_view_model_t model = normal_model();
    uint8_t i;
    uint8_t reason;
    uint16_t minute;
    clock_fault_category_t category;
    reset_guards();
    all_faults(&model);
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    for (minute = 0U; minute < 1440U; minute++) {
        model.calendar.hour = (uint8_t)(minute / 60U);
        model.calendar.minute = (uint8_t)(minute % 60U);
        CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    }
    for (i = 0U; i < CLOCK_FAULT_SOURCE_COUNT; i++) {
        for (reason = 0U; reason < sizeof(reasons) / sizeof(reasons[0]); reason++) {
            model = normal_model();
            category = clock_fault_category((clock_fault_source_t)i);
            model.faults.active_categories = 1U;
            model.faults.category[category].source_bits = 1UL << i;
            model.faults.source[i].first_mask = reasons[reason];
            CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
        }
    }
    all_faults(&model);
    model.calendar_state = CLOCK_DATA_ERROR;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    for (i = 0U; i < CLOCK_CATEGORY_COUNT; i++) {
        model.faults.category[i].time_valid = 0U;
    }
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    check_guards();
}

static void export_previews(const char *p_directory)
{
    clock_view_model_t model = normal_model();
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-sram");
    model.memory_mode = CLOCK_MEMORY_IFLASH;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-iflash");
    model.memory_mode = CLOCK_MEMORY_W25Q;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-w25q");
    model = normal_model();
    model.calendar.hour = 11U;
    model.calendar.minute = 11U;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-11-11");
    model.calendar.hour = 4U;
    model.calendar.minute = 44U;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-04-44");
    model.calendar.hour = 23U;
    model.calendar.minute = 59U;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-23-59");
    model = normal_model();
    model.calendar.hour = 4U;
    model.calendar.minute = 44U;
    model.format = CLOCK_FORMAT_12;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-04-44-am");
    model = normal_model();
    model.calendar.year = 2000U;
    model.calendar.month = 1U;
    model.calendar.day = 1U;
    model.calendar.weekday = 6U;
    model.calendar.hour = 0U;
    model.calendar.minute = 1U;
    model.temperature.state = CLOCK_DATA_UNAVAILABLE;
    model.battery.state = CLOCK_DATA_UNAVAILABLE;
    model.wifi.state = CLOCK_DATA_UNAVAILABLE;
    model.memory[CLOCK_MEMORY_SRAM].value = 27;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-board-reference");
    model.calendar.year = 2088U;
    model.calendar.month = 8U;
    model.calendar.day = 4U;
    model.calendar.weekday = 3U;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-longest-date");
    memset(&model, 0, sizeof(model));
    model.format = CLOCK_FORMAT_24;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-missing");
    model = normal_model();
    all_faults(&model);
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-seven-errors");
    model.calendar_state = CLOCK_DATA_ERROR;
    model.faults.category[CLOCK_CATEGORY_RTC].time_valid = 0U;
    CHECK(epd_clock_view_draw_gray4(s_frame, FRAME_BYTES, &model) == 1U);
    write_preview(p_directory, "clock-gray-rtc-unavailable");
    check_guards();
}

int main(int argc, char **argv)
{
    test_inputs();
    test_fonts_and_orientation();
    test_small_font_readability();
    test_main_font_cells();
    test_main_cell_layout();
    test_minute_alignment();
    test_date_without_calendar_icon();
    test_times();
    test_dates();
    test_missing_and_ranges();
    test_faults();
    if (argc > 1) {
        export_previews(argv[1]);
    }
    if (s_failures != 0U) {
        fprintf(stderr, "%u failures across %lu checks\n", s_failures, s_checks);
        return 1;
    }
    printf("PASS: gray4 clock view (%lu checks): 1440 times x 2 formats x 3 memory "
           "modes, all 36525 dates, no seconds, unavailable/bounds, all 7 fault "
           "categories, native mapping, four levels, solid small glyphs, fixed main "
           "cells, aligned minutes, date without calendar icon, spacing, guards\n", s_checks);
    return 0;
}
