#include "../app/epd_clock_view.h"
#include "../resources/clock_gray_assets.h"
#include "../gui/GUI_Paint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_BYTES 32U
#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static uint8_t s_guarded[EPD_CLOCK_VIEW_GRAY4_BYTES + 2U * GUARD_BYTES];
static uint8_t *s_frame = &s_guarded[GUARD_BYTES];
static unsigned long s_checks;

static uint8_t pixel(uint16_t x, uint16_t y)
{
    size_t native_row = 295U - x;
    uint16_t native_column = (uint16_t)(127U - y);
    return (uint8_t)((s_frame[native_row * 32U + native_column / 4U] >>
                     (6U - 2U * (native_column % 4U))) & 3U);
}

static const clock_gray_glyph_t *glyph(char code)
{
    const clock_gray_font_t *p_font = &g_clock_gray_fonts[CLOCK_FONT_MAIN];
    uint8_t i;
    for (i = 0U; i < p_font->count; i++) {
        if (p_font->p_glyphs[i].code == (uint8_t)code) {
            return &p_font->p_glyphs[i];
        }
    }
    return NULL;
}

static uint8_t glyph_pixel(const clock_gray_glyph_t *p_glyph, uint16_t x, uint16_t y)
{
    const clock_gray_font_t *p_font = &g_clock_gray_fonts[CLOCK_FONT_MAIN];
    uint16_t row_bytes = (uint16_t)((p_glyph->width + 3U) / 4U);
    return (uint8_t)((p_font->p_data[p_glyph->offset + y * row_bytes + x / 4U] >>
                     (6U - 2U * (x % 4U))) & 3U);
}

static void check_pixels(void)
{
    static const char s_digits[] = "0689";
    const clock_gray_glyph_t *p_glyph;
    PAINT paint_before;
    uint16_t x;
    uint16_t y;
    uint16_t base;
    uint16_t cursor;
    uint8_t color;
    uint8_t digit;
    uint8_t expected;
    uint32_t gray_pixels = 0U;
    size_t i;
    memset(s_guarded, 0xA5, sizeof(s_guarded));
    CHECK(epd_clock_view_draw_gray4_diagnostic(NULL, EPD_CLOCK_VIEW_GRAY4_BYTES) == 0U);
    CHECK(epd_clock_view_draw_gray4_diagnostic(s_frame, EPD_CLOCK_VIEW_GRAY4_BYTES - 1U) == 0U);
    for (i = 0U; i < sizeof(s_guarded); i++) {
        CHECK(s_guarded[i] == 0xA5U);
    }
    memcpy(&paint_before, &Paint, sizeof(Paint));
    CHECK(epd_clock_view_draw_gray4_diagnostic(s_frame, EPD_CLOCK_VIEW_GRAY4_BYTES) == 1U);
    CHECK(memcmp(&paint_before, &Paint, sizeof(Paint)) == 0);
    for (i = 0U; i < GUARD_BYTES; i++) {
        CHECK(s_guarded[i] == 0xA5U);
        CHECK(s_guarded[GUARD_BYTES + EPD_CLOCK_VIEW_GRAY4_BYTES + i] == 0xA5U);
    }
    for (color = 0U; color < 4U; color++) {
        base = (uint16_t)(8U + 72U * color);
        for (y = 4U; y < 22U; y++) {
            for (x = 0U; x < 64U; x++) {
                expected = (x == 0U || x == 63U || y == 4U || y == 21U) ? 3U : color;
                CHECK(pixel((uint16_t)(base + x), y) == expected);
            }
        }
        for (x = 0U; x < 16U; x++) {
            CHECK(pixel((uint16_t)(base + x), 112U) == color);
        }
        for (y = 0U; y < 14U; y++) {
            CHECK(pixel((uint16_t)(base + 18U), (uint16_t)(111U + y)) == color);
            CHECK(pixel((uint16_t)(base + 24U + y), (uint16_t)(111U + y)) == color);
        }
        CHECK(pixel((uint16_t)(base + 51U), 112U) == color);
    }
    cursor = 0U;
    for (digit = 0U; digit < sizeof(s_digits) - 1U; digit++) {
        p_glyph = glyph(s_digits[digit]);
        CHECK(p_glyph != NULL);
        CHECK(p_glyph->advance == 34U);
        for (y = 0U; y < 56U; y++) {
            for (x = 0U; x < p_glyph->width; x++) {
                expected = 0U;
                if (y >= (uint16_t)p_glyph->y_offset &&
                    y < p_glyph->height + (uint16_t)p_glyph->y_offset) {
                    expected = glyph_pixel(p_glyph, x, (uint16_t)(y - p_glyph->y_offset));
                }
                CHECK(pixel((uint16_t)(152U + cursor + x), (uint16_t)(49U + y)) == expected);
                CHECK(pixel((uint16_t)(8U + cursor + x), (uint16_t)(49U + y)) ==
                      (expected >= 2U ? 3U : 0U));
                if (expected == 1U || expected == 2U) { gray_pixels++; }
            }
        }
        cursor = (uint16_t)(cursor + p_glyph->advance);
    }
    CHECK(cursor == 136U);
    CHECK(gray_pixels > 0U);
    CHECK(pixel(0U, 0U) == 3U);
    CHECK(pixel(1U, 1U) == 3U);
    CHECK(pixel(293U, 0U) == 3U);
    CHECK(pixel(294U, 1U) == 0U);
    CHECK(pixel(295U, 2U) == 3U);
    CHECK(pixel(0U, 127U) == 3U);
    CHECK(pixel(1U, 126U) == 3U);
    CHECK(pixel(293U, 125U) == 3U);
    CHECK(pixel(294U, 126U) == 0U);
}

static void write_preview(const char *p_directory)
{
    char path[4096];
    FILE *p_file;
    uint16_t x;
    uint16_t y;
    int result;
    result = snprintf(path, sizeof(path), "%s/clock-gray-diagnostic.pgm", p_directory);
    CHECK(result > 0 && (size_t)result < sizeof(path));
    p_file = fopen(path, "wb");
    CHECK(p_file != NULL);
    (void)fprintf(p_file, "P5\n296 128\n255\n");
    for (y = 0U; y < 128U; y++) {
        for (x = 0U; x < 296U; x++) {
            (void)fputc(255U - 85U * pixel(x, y), p_file);
        }
    }
    CHECK(ferror(p_file) == 0);
    CHECK(fclose(p_file) == 0);
}

int main(int argc, char **argv)
{
    check_pixels();
    if (argc > 1) { write_preview(argv[1]); }
    printf("PASS: %lu diagnostic pixel checks: levels, native glyph comparison, "
           "lines, circle, orientation, guards and independent Paint\n", s_checks);
    return 0;
}
