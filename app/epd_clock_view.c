#include "epd_clock_view.h"

#include "../device/epd_gdey029t94.h"
#include "../gui/GUI_Paint.h"

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
