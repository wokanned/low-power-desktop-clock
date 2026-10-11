#include "GUI_Paint.h"

#include <stddef.h>
#include <string.h>

/* Adapted from the vendor monochrome GUI. No display or MCU operations here. */
PAINT Paint;

static UBYTE paint_valid_rotation(UWORD rotation)
{
    return (UBYTE)((rotation == ROTATE_0) || (rotation == ROTATE_90) ||
                   (rotation == ROTATE_180) || (rotation == ROTATE_270));
}

static void paint_update_dimensions(void)
{
    if ((Paint.Rotate == ROTATE_0) || (Paint.Rotate == ROTATE_180)) {
        Paint.Width = Paint.WidthMemory;
        Paint.Height = Paint.HeightMemory;
    } else {
        Paint.Width = Paint.HeightMemory;
        Paint.Height = Paint.WidthMemory;
    }
}

void Paint_NewImage(UBYTE *image, UWORD Width, UWORD Height,
                    UWORD Rotate, UWORD Color)
{
    memset(&Paint, 0, sizeof(Paint));
    if ((image == NULL) || (Width == 0U) || (Height == 0U) ||
        (paint_valid_rotation(Rotate) == 0U)) {
        return;
    }

    Paint.Image = image;
    Paint.WidthMemory = Width;
    Paint.HeightMemory = Height;
    Paint.WidthByte = (UWORD)(((uint32_t)Width + 7U) / 8U);
    Paint.HeightByte = Height;
    Paint.Color = Color;
    Paint.Rotate = Rotate;
    Paint.Mirror = MIRROR_NONE;
    paint_update_dimensions();
}

void Paint_SelectImage(UBYTE *image)
{
    Paint.Image = image;
}

void Paint_SetRotate(UWORD Rotate)
{
    if (paint_valid_rotation(Rotate) != 0U) {
        Paint.Rotate = Rotate;
        paint_update_dimensions();
    }
}

void Paint_SetMirroring(UBYTE mirror)
{
    if (mirror <= (UBYTE)MIRROR_ORIGIN) {
        Paint.Mirror = mirror;
    }
}

void Paint_SetPixel(UWORD Xpoint, UWORD Ypoint, UWORD Color)
{
    uint32_t x;
    uint32_t y;
    uint32_t address;
    UBYTE mask;

    if ((Paint.Image == NULL) || (Xpoint >= Paint.Width) ||
        (Ypoint >= Paint.Height)) {
        return;
    }

    switch (Paint.Rotate) {
    case ROTATE_0:
        x = Xpoint;
        y = Ypoint;
        break;
    case ROTATE_90:
        x = (uint32_t)Paint.WidthMemory - 1U - Ypoint;
        y = Xpoint;
        break;
    case ROTATE_180:
        x = (uint32_t)Paint.WidthMemory - 1U - Xpoint;
        y = (uint32_t)Paint.HeightMemory - 1U - Ypoint;
        break;
    case ROTATE_270:
        x = Ypoint;
        y = (uint32_t)Paint.HeightMemory - 1U - Xpoint;
        break;
    default:
        return;
    }

    switch (Paint.Mirror) {
    case MIRROR_NONE:
        break;
    case MIRROR_HORIZONTAL:
        x = (uint32_t)Paint.WidthMemory - 1U - x;
        break;
    case MIRROR_VERTICAL:
        y = (uint32_t)Paint.HeightMemory - 1U - y;
        break;
    case MIRROR_ORIGIN:
        x = (uint32_t)Paint.WidthMemory - 1U - x;
        y = (uint32_t)Paint.HeightMemory - 1U - y;
        break;
    default:
        return;
    }

    if ((x >= Paint.WidthMemory) || (y >= Paint.HeightMemory)) {
        return;
    }
    address = (y * Paint.WidthByte) + (x / 8U);
    mask = (UBYTE)(0x80U >> (x % 8U));
    if (Color == BLACK) {
        Paint.Image[address] &= (UBYTE)(~mask);
    } else {
        Paint.Image[address] |= mask;
    }
}

static void paint_pixel_signed(int32_t x, int32_t y, UWORD color)
{
    if ((x >= 0) && (y >= 0) && ((uint32_t)x < Paint.Width) &&
        ((uint32_t)y < Paint.Height)) {
        Paint_SetPixel((UWORD)x, (UWORD)y, color);
    }
}

void Paint_Clear(UWORD Color)
{
    if (Paint.Image != NULL) {
        memset(Paint.Image, (UBYTE)Color,
               (size_t)Paint.WidthByte * Paint.HeightByte);
    }
}

void Paint_ClearWindows(UWORD Xstart, UWORD Ystart, UWORD Xend,
                        UWORD Yend, UWORD Color)
{
    uint32_t x;
    uint32_t y;
    uint32_t right = Xend;
    uint32_t bottom = Yend;

    if (right > Paint.Width) {
        right = Paint.Width;
    }
    if (bottom > Paint.Height) {
        bottom = Paint.Height;
    }
    for (y = Ystart; y < bottom; y++) {
        for (x = Xstart; x < right; x++) {
            Paint_SetPixel((UWORD)x, (UWORD)y, Color);
        }
    }
}

static void paint_point_signed(int32_t x, int32_t y, UWORD color,
                                DOT_PIXEL size, DOT_STYLE style)
{
    int32_t dx;
    int32_t dy;
    int32_t start;
    int32_t end;

    if ((size < DOT_PIXEL_1X1) || (size > DOT_PIXEL_8X8)) {
        return;
    }
    if (style == DOT_FILL_AROUND) {
        start = 1 - (int32_t)size;
        end = (int32_t)size;
    } else if (style == DOT_FILL_RIGHTUP) {
        start = 0;
        end = (int32_t)size;
    } else {
        return;
    }
    for (dy = start; dy < end; dy++) {
        for (dx = start; dx < end; dx++) {
            paint_pixel_signed(x + dx, y + dy, color);
        }
    }
}

void Paint_DrawPoint(UWORD Xpoint, UWORD Ypoint, UWORD Color,
                     DOT_PIXEL Dot_Pixel, DOT_STYLE Dot_FillWay)
{
    paint_point_signed(Xpoint, Ypoint, Color, Dot_Pixel, Dot_FillWay);
}

static void paint_line_signed(int32_t x, int32_t y,
                               int32_t xend, int32_t yend, UWORD color,
                               LINE_STYLE style, DOT_PIXEL size)
{
    int32_t dx = (xend >= x) ? (xend - x) : (x - xend);
    int32_t dy = (yend >= y) ? (y - yend) : (yend - y);
    int32_t sx = (x < xend) ? 1 : -1;
    int32_t sy = (y < yend) ? 1 : -1;
    int32_t error = dx + dy;
    int32_t twice_error;
    uint32_t dotted_count = 0U;

    for (;;) {
        dotted_count++;
        paint_point_signed(x, y,
            ((style == LINE_STYLE_DOTTED) && ((dotted_count % 3U) == 0U))
                ? IMAGE_BACKGROUND : color,
            size, DOT_STYLE_DFT);
        if ((x == xend) && (y == yend)) {
            break;
        }
        twice_error = 2 * error;
        if (twice_error >= dy) {
            error += dy;
            x += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y += sy;
        }
    }
}

void Paint_DrawLine(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend,
                    UWORD Color, LINE_STYLE Line_Style, DOT_PIXEL Dot_Pixel)
{
    if (Paint.Image != NULL) {
        paint_line_signed(Xstart, Ystart, Xend, Yend,
                          Color, Line_Style, Dot_Pixel);
    }
}

void Paint_DrawRectangle(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend,
                         UWORD Color, DRAW_FILL Filled, DOT_PIXEL Dot_Pixel)
{
    int32_t left = (Xstart <= Xend) ? Xstart : Xend;
    int32_t right = (Xstart <= Xend) ? Xend : Xstart;
    int32_t top = (Ystart <= Yend) ? Ystart : Yend;
    int32_t bottom = (Ystart <= Yend) ? Yend : Ystart;
    int32_t y;
    int32_t margin;

    if (Paint.Image == NULL) {
        return;
    }
    if (Filled == DRAW_FILL_FULL) {
        if ((Dot_Pixel < DOT_PIXEL_1X1) || (Dot_Pixel > DOT_PIXEL_8X8)) {
            return;
        }
        margin = (int32_t)Dot_Pixel - 1;
        if (right > ((int32_t)Paint.Width - 1 + margin)) {
            right = (int32_t)Paint.Width - 1 + margin;
        }
        if (bottom > ((int32_t)Paint.Height - 1 + margin)) {
            bottom = (int32_t)Paint.Height - 1 + margin;
        }
        if ((left > right) || (top > bottom)) {
            return;
        }
        for (y = top; y <= bottom; y++) {
            paint_line_signed(left, y, right, y,
                              Color, LINE_STYLE_SOLID, Dot_Pixel);
        }
    } else {
        paint_line_signed(left, top, right, top,
                          Color, LINE_STYLE_SOLID, Dot_Pixel);
        paint_line_signed(left, bottom, right, bottom,
                          Color, LINE_STYLE_SOLID, Dot_Pixel);
        paint_line_signed(left, top, left, bottom,
                          Color, LINE_STYLE_SOLID, Dot_Pixel);
        paint_line_signed(right, top, right, bottom,
                          Color, LINE_STYLE_SOLID, Dot_Pixel);
    }
}

static void paint_horizontal_span(int32_t left, int32_t right,
                                   int32_t y, UWORD color)
{
    int32_t x;

    if ((y < 0) || ((uint32_t)y >= Paint.Height)) {
        return;
    }
    if (left < 0) {
        left = 0;
    }
    if (right >= Paint.Width) {
        right = (int32_t)Paint.Width - 1;
    }
    for (x = left; x <= right; x++) {
        paint_pixel_signed(x, y, color);
    }
}

void Paint_DrawCircle(UWORD X_Center, UWORD Y_Center, UWORD Radius,
                      UWORD Color, DRAW_FILL Draw_Fill, DOT_PIXEL Dot_Pixel)
{
    int32_t x = 0;
    int32_t y = Radius;
    int32_t error = 1 - (int32_t)Radius;
    int32_t cx = X_Center;
    int32_t cy = Y_Center;

    if (Paint.Image == NULL) {
        return;
    }
    while (x <= y) {
        if (Draw_Fill == DRAW_FILL_FULL) {
            paint_horizontal_span(cx - y, cx + y, cy + x, Color);
            paint_horizontal_span(cx - y, cx + y, cy - x, Color);
            paint_horizontal_span(cx - x, cx + x, cy + y, Color);
            paint_horizontal_span(cx - x, cx + x, cy - y, Color);
        } else {
            paint_point_signed(cx + x, cy + y, Color, Dot_Pixel, DOT_STYLE_DFT);
            paint_point_signed(cx - x, cy + y, Color, Dot_Pixel, DOT_STYLE_DFT);
            paint_point_signed(cx + x, cy - y, Color, Dot_Pixel, DOT_STYLE_DFT);
            paint_point_signed(cx - x, cy - y, Color, Dot_Pixel, DOT_STYLE_DFT);
            paint_point_signed(cx + y, cy + x, Color, Dot_Pixel, DOT_STYLE_DFT);
            paint_point_signed(cx - y, cy + x, Color, Dot_Pixel, DOT_STYLE_DFT);
            paint_point_signed(cx + y, cy - x, Color, Dot_Pixel, DOT_STYLE_DFT);
            paint_point_signed(cx - y, cy - x, Color, Dot_Pixel, DOT_STYLE_DFT);
        }
        x++;
        if (error < 0) {
            error += (2 * x) + 1;
        } else {
            y--;
            error += (2 * (x - y)) + 1;
        }
    }
}

static UBYTE paint_valid_font(const sFONT *font)
{
    uint32_t row_bytes;
    uint32_t glyph_bytes;

    if ((font == NULL) || (font->table == NULL) || (font->Width == 0U) ||
        (font->Height == 0U)) {
        return 0U;
    }
    row_bytes = ((uint32_t)font->Width + 7U) / 8U;
    glyph_bytes = row_bytes * font->Height;
    return (UBYTE)(glyph_bytes <= (UINT32_MAX / FONT_ASCII_COUNT));
}

void Paint_DrawCharScaled(UWORD Xstart, UWORD Ystart, char Ascii_Char,
                          const sFONT *Font, UWORD Color_Background,
                          UWORD Color_Foreground, UBYTE scale)
{
    uint32_t row;
    uint32_t column;
    uint32_t sx;
    uint32_t sy;
    uint32_t row_bytes;
    uint32_t glyph_offset;
    uint32_t px;
    uint32_t py;
    UBYTE ascii = (UBYTE)Ascii_Char;
    UBYTE bit;
    UWORD color;

    if ((Paint.Image == NULL) || (scale == 0U) ||
        (paint_valid_font(Font) == 0U) || (Xstart >= Paint.Width) ||
        (Ystart >= Paint.Height)) {
        return;
    }
    if ((ascii < FONT_ASCII_FIRST) || (ascii > FONT_ASCII_LAST)) {
        ascii = (UBYTE)'?';
    }
    row_bytes = ((uint32_t)Font->Width + 7U) / 8U;
    glyph_offset = ((uint32_t)ascii - FONT_ASCII_FIRST) *
                   Font->Height * row_bytes;
    for (row = 0U; row < Font->Height; row++) {
        py = (uint32_t)Ystart + row * scale;
        if (py >= Paint.Height) {
            break;
        }
        for (column = 0U; column < Font->Width; column++) {
            px = (uint32_t)Xstart + column * scale;
            if (px >= Paint.Width) {
                break;
            }
            bit = (UBYTE)(Font->table[glyph_offset + row * row_bytes +
                                     column / 8U] &
                          (UBYTE)(0x80U >> (column % 8U)));
            if ((bit == 0U) && (Color_Background == FONT_BACKGROUND)) {
                continue;
            }
            color = (bit != 0U) ? Color_Foreground : Color_Background;
            for (sy = 0U; (sy < scale) && ((py + sy) < Paint.Height); sy++) {
                for (sx = 0U; (sx < scale) && ((px + sx) < Paint.Width); sx++) {
                    Paint_SetPixel((UWORD)(px + sx), (UWORD)(py + sy), color);
                }
            }
        }
    }
}

void Paint_DrawChar(UWORD Xstart, UWORD Ystart, char Ascii_Char,
                    const sFONT *Font, UWORD Color_Background,
                    UWORD Color_Foreground)
{
    Paint_DrawCharScaled(Xstart, Ystart, Ascii_Char, Font,
                         Color_Background, Color_Foreground, 1U);
}

void Paint_DrawString_ENScaled(UWORD Xstart, UWORD Ystart,
                               const char *pString, const sFONT *Font,
                               UWORD Color_Background, UWORD Color_Foreground,
                               UBYTE scale)
{
    uint32_t x = Xstart;
    uint32_t y = Ystart;
    uint32_t width;
    uint32_t height;

    if ((Paint.Image == NULL) || (pString == NULL) || (scale == 0U) ||
        (paint_valid_font(Font) == 0U) || (Xstart >= Paint.Width) ||
        (Ystart >= Paint.Height)) {
        return;
    }
    width = (uint32_t)Font->Width * scale;
    height = (uint32_t)Font->Height * scale;
    if (width > ((uint32_t)Paint.Width - Xstart)) {
        return;
    }
    while (*pString != '\0') {
        if ((x + width) > Paint.Width) {
            x = Xstart;
            y += height;
        }
        if ((y + height) > Paint.Height) {
            break;
        }
        Paint_DrawCharScaled((UWORD)x, (UWORD)y, *pString, Font,
                             Color_Background, Color_Foreground, scale);
        pString++;
        x += width;
    }
}

void Paint_DrawString_EN(UWORD Xstart, UWORD Ystart, const char *pString,
                         const sFONT *Font, UWORD Color_Background,
                         UWORD Color_Foreground)
{
    Paint_DrawString_ENScaled(Xstart, Ystart, pString, Font,
                              Color_Background, Color_Foreground, 1U);
}

void Paint_DrawNum(UWORD Xpoint, UWORD Ypoint, int32_t Number,
                   const sFONT *Font, UWORD Color_Background,
                   UWORD Color_Foreground)
{
    char text[12];
    uint32_t magnitude;
    uint32_t cursor = sizeof(text) - 1U;

    text[cursor] = '\0';
    magnitude = (Number < 0) ? (0U - (uint32_t)Number) : (uint32_t)Number;
    do {
        text[--cursor] = (char)('0' + (magnitude % 10U));
        magnitude /= 10U;
    } while (magnitude != 0U);
    if (Number < 0) {
        text[--cursor] = '-';
    }
    Paint_DrawString_EN(Xpoint, Ypoint, &text[cursor], Font,
                        Color_Background, Color_Foreground);
}

void Paint_DrawTime(UWORD Xstart, UWORD Ystart, const PAINT_TIME *pTime,
                    const sFONT *Font, UWORD Color_Background,
                    UWORD Color_Foreground)
{
    uint32_t dx;
    uint32_t positions[8];
    char text[8];
    uint32_t i;

    if ((pTime == NULL) || (paint_valid_font(Font) == 0U) ||
        (pTime->Hour > 23U) || (pTime->Min > 59U) || (pTime->Sec > 59U)) {
        return;
    }
    dx = Font->Width;
    positions[0] = 0U;
    positions[1] = dx;
    positions[2] = dx + dx / 4U + dx / 2U;
    positions[3] = dx * 2U + dx / 2U;
    positions[4] = dx * 3U + dx / 2U;
    positions[5] = dx * 4U + dx / 2U - dx / 4U;
    positions[6] = dx * 5U;
    positions[7] = dx * 6U;
    text[0] = (char)('0' + pTime->Hour / 10U);
    text[1] = (char)('0' + pTime->Hour % 10U);
    text[2] = ':';
    text[3] = (char)('0' + pTime->Min / 10U);
    text[4] = (char)('0' + pTime->Min % 10U);
    text[5] = ':';
    text[6] = (char)('0' + pTime->Sec / 10U);
    text[7] = (char)('0' + pTime->Sec % 10U);
    for (i = 0U; i < 8U; i++) {
        if (((uint32_t)Xstart + positions[i]) < Paint.Width) {
            Paint_DrawChar((UWORD)((uint32_t)Xstart + positions[i]), Ystart,
                           text[i], Font, Color_Background, Color_Foreground);
        }
    }
}

void Paint_DrawBitMap(const unsigned char *image_buffer)
{
    if ((Paint.Image != NULL) && (image_buffer != NULL)) {
        memmove(Paint.Image, image_buffer,
                (size_t)Paint.WidthByte * Paint.HeightByte);
    }
}

void Paint_DrawString_CN(UWORD x, UWORD y, const uint8_t *gb2312,
                          uint16_t length, const cFONT *font,
                          UWORD background, UWORD foreground)
{
    uint32_t cursor = x;
    uint32_t offset = 0U;
    uint32_t row;
    uint32_t col;
    uint32_t stride;
    uint16_t entry;
    uint16_t advance;
    uint8_t first;
    uint8_t second;
    const CH_CN *glyph;
    if ((Paint.Image == NULL) || (gb2312 == NULL) || (font == NULL) ||
        (font->table == NULL) || (font->Width == 0U) || (font->Height == 0U) ||
        (font->ASCII_Width == 0U) || (font->Width > 32U) ||
        (font->Height > 41U) || (x >= Paint.Width) || (y >= Paint.Height)) return;
    stride = ((uint32_t)font->Width + 7U) / 8U;
    while ((offset < length) && (cursor < Paint.Width)) {
        first = gb2312[offset++];
        second = 0U;
        advance = (first < 0x80U) ? font->ASCII_Width : font->Width;
        if ((first >= 0x80U) && (offset < length) &&
            (gb2312[offset] >= 0xA1U) && (gb2312[offset] <= 0xFEU)) {
            second = gb2312[offset++];
        }
        glyph = NULL;
        for (entry = 0U; entry < font->size; entry++) {
            if ((font->table[entry].index[0] == first) &&
                (font->table[entry].index[1] == second) &&
                ((first < 0x80U) || ((first >= 0xA1U) && (first <= 0xF7U) &&
                                        (second != 0U)))) {
                glyph = &font->table[entry];
                break;
            }
        }
        for (row = 0U; (row < font->Height) && ((uint32_t)y + row < Paint.Height); row++) {
            for (col = 0U; (col < advance) && (cursor + col < Paint.Width); col++) {
                uint8_t ink;
                if (glyph != NULL) {
                    ink = (uint8_t)((col < font->Width) &&
                        ((glyph->matrix[row * stride + col / 8U] &
                          (0x80U >> (col % 8U))) != 0U));
                } else {
                    ink = (uint8_t)((col == 0U) || (col + 1U == advance) ||
                                    (row == 0U) || (row + 1U == font->Height));
                }
                if (ink != 0U) {
                    Paint_SetPixel((UWORD)(cursor + col), (UWORD)(y + row), foreground);
                } else if (background != WHITE) {
                    Paint_SetPixel((UWORD)(cursor + col), (UWORD)(y + row), background);
                }
            }
        }
        cursor += advance;
    }
}
