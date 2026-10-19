#include "epd_clock_view.h"

#include "../device/epd_gdey029t94.h"
#include "../gui/GUI_Paint.h"
#include "../resources/clock_gray_assets.h"

#include <string.h>

#define CLOCK_GRAY_NATIVE_ROW_BYTES 32U
#define CLOCK_GRAY_BLACK             3U
#define CLOCK_GRAY_DIVIDER           1U
#define CLOCK_GRAY_MAIN_DIVIDER      2U
#define CLOCK_GRAY_STATUS_X        168U
#define CLOCK_GRAY_STATUS_RIGHT    228U
#define CLOCK_GRAY_MEMORY_X        236U
#define CLOCK_GRAY_CONTENT_RIGHT   288U
#define CLOCK_GRAY_FAULT_Y          29U
#define CLOCK_GRAY_FAULT_ROW        14U

typedef struct {
    uint8_t *p_frame;
    uint8_t valid;
} clock_gray_canvas_t;

static void clock_gray_pixel(clock_gray_canvas_t *p_canvas,
                             int16_t x, int16_t y, uint8_t color)
{
    uint16_t native_x;
    uint16_t native_y;
    uint8_t shift;
    size_t offset;
    if ((x < 0) || (y < 0) || (x >= (int16_t)EPD_CLOCK_VIEW_WIDTH) ||
        (y >= (int16_t)EPD_CLOCK_VIEW_HEIGHT)) {
        p_canvas->valid = 0U;
        return;
    }
    native_x = (uint16_t)(127 - y);
    native_y = (uint16_t)(295 - x);
    offset = (size_t)native_y * CLOCK_GRAY_NATIVE_ROW_BYTES + native_x / 4U;
    shift = (uint8_t)(6U - 2U * (native_x % 4U));
    p_canvas->p_frame[offset] = (uint8_t)((p_canvas->p_frame[offset] &
                               (uint8_t)~(3U << shift)) | (color << shift));
}

static void clock_gray_rectangle(clock_gray_canvas_t *p_canvas,
                                 uint16_t x, uint16_t y,
                                 uint16_t width, uint16_t height, uint8_t color)
{
    uint16_t i;
    uint16_t j;
    for (i = 0U; i < height; i++) {
        for (j = 0U; j < width; j++) {
            clock_gray_pixel(p_canvas, (int16_t)(x + j), (int16_t)(y + i), color);
        }
    }
}

static const clock_gray_glyph_t *clock_gray_glyph(const clock_gray_font_t *p_font,
                                                 char code)
{
    uint8_t i;
    for (i = 0U; i < p_font->count; i++) {
        if (p_font->p_glyphs[i].code == (uint8_t)code) {
            return &p_font->p_glyphs[i];
        }
    }
    return 0;
}

static uint16_t clock_gray_text_width(clock_gray_font_id_t font_id,
                                      const char *p_text)
{
    const clock_gray_font_t *p_font = &g_clock_gray_fonts[font_id];
    const clock_gray_glyph_t *p_glyph;
    uint16_t advance = 0U;
    uint16_t width = 0U;
    while (*p_text != '\0') {
        p_glyph = clock_gray_glyph(p_font, *p_text++);
        if (p_glyph == 0) {
            return 0xFFFFU;
        }
        width = (uint16_t)(advance + p_glyph->width);
        advance = (uint16_t)(advance + p_glyph->advance);
    }
    return width;
}

static void clock_gray_bitmap(clock_gray_canvas_t *p_canvas,
                              const uint8_t *p_data, uint16_t x, int16_t y,
                              uint8_t width, uint8_t height)
{
    uint8_t i;
    uint8_t j;
    uint8_t color;
    uint8_t row_bytes = (uint8_t)((width + 3U) / 4U);
    for (i = 0U; i < height; i++) {
        for (j = 0U; j < width; j++) {
            color = (uint8_t)((p_data[(uint16_t)i * row_bytes + j / 4U] >>
                              (6U - 2U * (j % 4U))) & 3U);
            if (color != 0U) {
                clock_gray_pixel(p_canvas, (int16_t)(x + j), (int16_t)(y + i), color);
            }
        }
    }
}

static void clock_gray_text(clock_gray_canvas_t *p_canvas,
                            clock_gray_font_id_t font_id,
                            uint16_t x, uint16_t y, uint16_t right,
                            const char *p_text)
{
    const clock_gray_font_t *p_font = &g_clock_gray_fonts[font_id];
    const clock_gray_glyph_t *p_glyph;
    uint16_t width = clock_gray_text_width(font_id, p_text);
    if ((width == 0xFFFFU) || (x + width > right)) {
        p_canvas->valid = 0U;
        return;
    }
    while (*p_text != '\0') {
        p_glyph = clock_gray_glyph(p_font, *p_text++);
        clock_gray_bitmap(p_canvas, &p_font->p_data[p_glyph->offset], x,
                          (int16_t)(y + p_glyph->y_offset),
                          p_glyph->width, p_glyph->height);
        x = (uint16_t)(x + p_glyph->advance);
    }
}

static void clock_gray_text_right(clock_gray_canvas_t *p_canvas,
                                  clock_gray_font_id_t font_id,
                                  uint16_t right, uint16_t y,
                                  const char *p_text)
{
    uint16_t width = clock_gray_text_width(font_id, p_text);
    if ((width == 0xFFFFU) || (width > right)) {
        p_canvas->valid = 0U;
        return;
    }
    clock_gray_text(p_canvas, font_id, (uint16_t)(right - width), y, right, p_text);
}

static void clock_gray_icon(clock_gray_canvas_t *p_canvas,
                            clock_gray_icon_id_t icon_id, uint16_t x, uint16_t y)
{
    const clock_gray_icon_t *p_icon = &g_clock_gray_icons[icon_id];
    clock_gray_bitmap(p_canvas, p_icon->p_data, x, (int16_t)y,
                      p_icon->width, p_icon->height);
}

static void clock_gray_two_digits(char *p_text, uint8_t value)
{
    p_text[0] = (char)('0' + value / 10U);
    p_text[1] = (char)('0' + value % 10U);
}

static char *clock_gray_unsigned(char *p_text, uint32_t value)
{
    char reversed[10];
    uint8_t count = 0U;
    do {
        reversed[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) {
        *p_text++ = reversed[--count];
    }
    *p_text = '\0';
    return p_text;
}

static void clock_gray_time(char text[6], uint8_t hour, uint8_t minute)
{
    clock_gray_two_digits(text, hour);
    text[2] = ':';
    clock_gray_two_digits(&text[3], minute);
    text[5] = '\0';
}

static void clock_gray_date(char text[16], const clock_calendar_t *p_calendar)
{
    text[0] = '2';
    text[1] = '0';
    clock_gray_two_digits(&text[2], (uint8_t)(p_calendar->year % 100U));
    text[4] = '-';
    clock_gray_two_digits(&text[5], p_calendar->month);
    text[7] = '-';
    clock_gray_two_digits(&text[8], p_calendar->day);
    text[10] = ' ';
    memcpy(&text[11], clock_weekday_label(p_calendar->weekday), 3U);
    text[14] = '\0';
}

static void clock_gray_percent(char text[16], const clock_field_t *p_field)
{
    char *p_end;
    if ((p_field->state != CLOCK_DATA_READY) ||
        (p_field->value < 0) || (p_field->value > 100)) {
        strcpy(text, "N/A");
        return;
    }
    p_end = clock_gray_unsigned(text, (uint32_t)p_field->value);
    *p_end++ = '%';
    *p_end = '\0';
}

static void clock_gray_temperature(char text[16], const clock_field_t *p_field)
{
    uint32_t magnitude;
    char *p_end = text;
    if ((p_field->state != CLOCK_DATA_READY) ||
        (p_field->value < -999) || (p_field->value > 9999)) {
        strcpy(text, "N/A");
        return;
    }
    magnitude = (uint32_t)p_field->value;
    if (p_field->value < 0) {
        *p_end++ = '-';
        magnitude = (uint32_t)(-p_field->value);
    }
    p_end = clock_gray_unsigned(p_end, magnitude / 10U);
    *p_end++ = '.';
    *p_end++ = (char)('0' + magnitude % 10U);
    *p_end++ = 'C';
    *p_end = '\0';
    if (clock_gray_text_width(CLOCK_FONT_VALUE, text) > 60U) {
        strcpy(text, "N/A");
    }
}

static void clock_gray_wifi(char text[16], const clock_field_t *p_field)
{
    char *p_end;
    if ((p_field->state != CLOCK_DATA_READY) || (p_field->value < 0)) {
        strcpy(text, "N/A");
    } else if (p_field->value > 99) {
        strcpy(text, "99h+");
    } else {
        p_end = clock_gray_unsigned(text, (uint32_t)p_field->value);
        strcpy(p_end, "h ago");
    }
}

static void clock_gray_normal(clock_gray_canvas_t *p_canvas,
                              const clock_view_model_t *p_model)
{
    static const char *const s_memory_labels[CLOCK_MEMORY_COUNT] = {
        "SRAM", "IFLASH", "W25Q"
    };
    char text[16];
    uint8_t hour = p_model->calendar.hour;
    uint8_t calendar_ready = (uint8_t)((p_model->calendar_state == CLOCK_DATA_READY) &&
                                      clock_calendar_valid(&p_model->calendar));
    clock_gray_icon(p_canvas, CLOCK_ICON_MOON, 8U, 6U);
    clock_gray_rectangle(p_canvas, 32U, 14U,
                         p_model->format == CLOCK_FORMAT_12 ? 80U : 112U, 1U, 1U);
    if (calendar_ready != 0U) {
        if (p_model->format == CLOCK_FORMAT_12) {
            clock_gray_text(p_canvas, CLOCK_FONT_LABEL, 120U, 7U, 152U,
                             hour >= 12U ? "PM" : "AM");
            hour = (uint8_t)(hour % 12U);
            if (hour == 0U) {
                hour = 12U;
            }
        }
        clock_gray_time(text, hour, p_model->calendar.minute);
    } else {
        strcpy(text, "--:--");
    }
    clock_gray_text(p_canvas, CLOCK_FONT_MAIN, 8U, 28U, 156U, text);
    if (calendar_ready != 0U) {
        clock_gray_date(text, &p_model->calendar);
    } else {
        strcpy(text, "N/A");
    }
    clock_gray_text(p_canvas, CLOCK_FONT_DATE, 8U, 102U, 156U, text);
    clock_gray_rectangle(p_canvas, 160U, 10U, 1U, 108U, CLOCK_GRAY_MAIN_DIVIDER);
    clock_gray_rectangle(p_canvas, 228U, 10U, 1U, 108U, CLOCK_GRAY_DIVIDER);
    clock_gray_rectangle(p_canvas, 168U, 64U, 120U, 1U, CLOCK_GRAY_DIVIDER);
    clock_gray_icon(p_canvas, CLOCK_ICON_THERMOMETER, 168U, 12U);
    clock_gray_icon(p_canvas, CLOCK_ICON_BATTERY, 236U, 12U);
    if ((p_model->battery.state == CLOCK_DATA_READY) &&
        (p_model->battery.value > 0) && (p_model->battery.value <= 100)) {
        clock_gray_rectangle(p_canvas, 239U, 19U,
                             (uint16_t)((p_model->battery.value + 9) / 10),
                             5U, CLOCK_GRAY_BLACK);
    }
    clock_gray_icon(p_canvas, CLOCK_ICON_WIFI, 168U, 76U);
    clock_gray_icon(p_canvas, CLOCK_ICON_CIRCLE, 234U, 78U);
    clock_gray_text(p_canvas, CLOCK_FONT_LABEL, 188U, 15U, 228U, "TEMP");
    clock_gray_text(p_canvas, CLOCK_FONT_LABEL, 256U, 15U, 288U, "BAT");
    clock_gray_text(p_canvas, CLOCK_FONT_LABEL, 188U, 79U, 228U, "WIFI");
    clock_gray_text(p_canvas, CLOCK_FONT_LABEL, 250U, 79U, 292U,
                     s_memory_labels[p_model->memory_mode]);
    clock_gray_temperature(text, &p_model->temperature);
    clock_gray_text(p_canvas, CLOCK_FONT_VALUE, 168U, 34U, 228U, text);
    clock_gray_percent(text, &p_model->battery);
    clock_gray_text(p_canvas, CLOCK_FONT_VALUE, 236U, 34U, 288U, text);
    clock_gray_wifi(text, &p_model->wifi);
    clock_gray_text(p_canvas, strcmp(text, "N/A") == 0 ? CLOCK_FONT_VALUE : CLOCK_FONT_WIFI,
                     168U, 98U, 228U, text);
    clock_gray_percent(text, &p_model->memory[p_model->memory_mode]);
    clock_gray_text(p_canvas, CLOCK_FONT_VALUE, 236U, 98U, 288U, text);
}

static void clock_gray_fault_detail(char text[64],
                                    const clock_fault_snapshot_t *p_faults,
                                    uint32_t source_bits)
{
    char *p_end = text;
    uint8_t i;
    uint8_t count = 0U;
    clock_fault_source_t first = CLOCK_FAULT_RTC;
    text[0] = '\0';
    for (i = 0U; i < CLOCK_FAULT_SOURCE_COUNT; i++) {
        if ((source_bits & (1UL << i)) != 0U) {
            if (count != 0U) {
                *p_end++ = '+';
            } else {
                first = (clock_fault_source_t)i;
            }
            strcpy(p_end, clock_fault_source_label((clock_fault_source_t)i));
            p_end += strlen(p_end);
            count++;
        }
    }
    if (count == 1U) {
        strcpy(text, clock_fault_reason_label(p_faults->source[first].first_mask));
    } else if (count == 0U) {
        strcpy(text, "FAULT");
    }
}

static void clock_gray_errors(clock_gray_canvas_t *p_canvas,
                              const clock_view_model_t *p_model)
{
    char text[64];
    char *p_end;
    const clock_fault_category_record_t *p_category;
    uint8_t i;
    uint8_t row = 0U;
    uint8_t count = 0U;
    for (i = 0U; i < CLOCK_CATEGORY_COUNT; i++) {
        if (p_model->faults.category[i].source_bits != 0U) {
            count++;
        }
    }
    if ((p_model->calendar_state == CLOCK_DATA_READY) &&
        clock_calendar_valid(&p_model->calendar)) {
        clock_gray_time(text, p_model->calendar.hour, p_model->calendar.minute);
    } else {
        strcpy(text, "--:--");
    }
    clock_gray_text(p_canvas, CLOCK_FONT_HEADER_TIME, 8U, 5U, 53U, text);
    if ((p_model->calendar_state == CLOCK_DATA_READY) &&
        clock_calendar_valid(&p_model->calendar)) {
        clock_gray_date(text, &p_model->calendar);
    } else {
        strcpy(text, "N/A");
    }
    clock_gray_text(p_canvas, CLOCK_FONT_HEADER_DATE, 55U, 9U, 220U, text);
    strcpy(text, "ERRORS ");
    p_end = clock_gray_unsigned(&text[7], count);
    *p_end = '\0';
    clock_gray_text_right(p_canvas, CLOCK_FONT_LABEL, 288U, 9U, text);
    clock_gray_rectangle(p_canvas, 8U, 26U, 280U, 1U, CLOCK_GRAY_DIVIDER);
    for (i = 0U; i < CLOCK_CATEGORY_COUNT; i++) {
        p_category = &p_model->faults.category[i];
        if (p_category->source_bits == 0U) {
            continue;
        }
        clock_gray_text(p_canvas, CLOCK_FONT_ERROR, 8U,
                         (uint16_t)(CLOCK_GRAY_FAULT_Y + row * CLOCK_GRAY_FAULT_ROW),
                         77U, clock_fault_category_label((clock_fault_category_t)i));
        clock_gray_fault_detail(text, &p_model->faults, p_category->source_bits);
        clock_gray_text(p_canvas, CLOCK_FONT_ERROR, 80U,
                         (uint16_t)(CLOCK_GRAY_FAULT_Y + row * CLOCK_GRAY_FAULT_ROW),
                         244U, text);
        if ((p_category->time_valid != 0U) &&
            (p_category->first_time.hour < 24U) &&
            (p_category->first_time.minute < 60U)) {
            clock_gray_time(text, p_category->first_time.hour,
                            p_category->first_time.minute);
        } else {
            strcpy(text, "--:--");
        }
        clock_gray_text_right(p_canvas, CLOCK_FONT_ERROR, 288U,
                               (uint16_t)(CLOCK_GRAY_FAULT_Y + row * CLOCK_GRAY_FAULT_ROW),
                               text);
        row++;
    }
}

uint8_t epd_clock_view_draw_gray4(uint8_t *frame, size_t length,
                                const clock_view_model_t *p_model)
{
    clock_gray_canvas_t canvas;
    if ((frame == 0) || (length < EPD_CLOCK_VIEW_GRAY4_BYTES) || (p_model == 0) ||
        ((uint32_t)p_model->memory_mode >= CLOCK_MEMORY_COUNT) ||
        ((p_model->format != CLOCK_FORMAT_12) && (p_model->format != CLOCK_FORMAT_24))) {
        return 0U;
    }
    canvas.p_frame = frame;
    canvas.valid = 1U;
    memset(frame, 0, EPD_CLOCK_VIEW_GRAY4_BYTES);
    if (p_model->faults.active_categories != 0U) {
        clock_gray_errors(&canvas, p_model);
    } else {
        clock_gray_normal(&canvas, p_model);
    }
    return canvas.valid;
}

/* Single application-owned canvas. Drawing never sends screen commands. */
static void epd_clock_view_begin(uint8_t *frame)
{
    Paint_NewImage(frame, EPD_GDEY029T94_WIDTH_PIXELS,
                   EPD_GDEY029T94_HEIGHT_PIXELS, ROTATE_270, WHITE);
    Paint_SetMirroring(MIRROR_HORIZONTAL);
    Paint_Clear(WHITE);
}

void epd_clock_view_draw(uint8_t *frame,
                         uint8_t hours, uint8_t minutes, uint8_t seconds)
{
    char text[9];

    if ((frame == 0) || (hours > 23U) ||
        (minutes > 59U) || (seconds > 59U)) {
        return;
    }
    text[0] = (char)('0' + hours / 10U);
    text[1] = (char)('0' + hours % 10U);
    text[2] = ':';
    text[3] = (char)('0' + minutes / 10U);
    text[4] = (char)('0' + minutes % 10U);
    text[5] = ':';
    text[6] = (char)('0' + seconds / 10U);
    text[7] = (char)('0' + seconds % 10U);
    text[8] = '\0';

    epd_clock_view_begin(frame);
    Paint_DrawString_ENScaled(EPD_CLOCK_VIEW_X, EPD_CLOCK_VIEW_Y,
                              text, &Font24, WHITE, BLACK,
                              EPD_CLOCK_VIEW_SCALE);
}

static void epd_clock_view_corner_marks(void)
{
    Paint_DrawRectangle(0U, 0U, 5U, 5U, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);
    Paint_DrawRectangle(290U, 0U, 295U, 5U, BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_1X1);
    Paint_DrawLine(0U, 122U, 5U, 127U, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    Paint_DrawLine(0U, 127U, 5U, 122U, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    Paint_DrawLine(283U, 123U, 295U, 123U, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    Paint_DrawLine(291U, 119U, 295U, 123U, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    Paint_DrawLine(291U, 127U, 295U, 123U, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    /* logical y=7/8 maps across a native byte boundary. */
    Paint_DrawPoint(148U, 7U, BLACK, DOT_PIXEL_1X1, DOT_FILL_AROUND);
    Paint_DrawPoint(148U, 8U, BLACK, DOT_PIXEL_1X1, DOT_FILL_AROUND);
}

void epd_clock_view_draw_orientation(uint8_t *frame)
{
    if (frame == 0) {
        return;
    }
    epd_clock_view_begin(frame);
    epd_clock_view_corner_marks();
    Paint_DrawString_EN(8U, 20U, "TOP  L -> R", &Font16, WHITE, BLACK);
    Paint_DrawString_EN(8U, 56U, "0123456789", &Font24, WHITE, BLACK);
    Paint_DrawString_EN(8U, 100U, "BOTTOM", &Font16, WHITE, BLACK);
}

void epd_clock_view_draw_demo(uint8_t *frame)
{
    if (frame == 0) {
        return;
    }
    epd_clock_view_begin(frame);
    epd_clock_view_corner_marks();
    Paint_DrawString_EN(8U, 8U, "Font8: ABC xyz 0123 ~", &Font8, WHITE, BLACK);
    Paint_DrawString_EN(8U, 20U, "Font12: ABC xyz 0123", &Font12, WHITE, BLACK);
    Paint_DrawString_EN(8U, 36U, "Font16: ABC 0123", &Font16, WHITE, BLACK);
    Paint_DrawString_EN(8U, 56U, "Font20: 0123", &Font20, WHITE, BLACK);
    Paint_DrawString_EN(8U, 80U, "0123456789", &Font24, WHITE, BLACK);
    Paint_DrawString_EN(8U, 112U, "TOP / BOTTOM   L -> R", &Font8, WHITE, BLACK);
    Paint_DrawRectangle(224U, 12U, 284U, 46U, BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_1X1);
    Paint_DrawLine(230U, 29U, 278U, 29U, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    Paint_DrawLine(270U, 21U, 278U, 29U, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    Paint_DrawLine(270U, 37U, 278U, 29U, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    Paint_DrawCircle(254U, 82U, 20U, BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_1X1);
    Paint_DrawCircle(254U, 82U, 7U, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);
}
