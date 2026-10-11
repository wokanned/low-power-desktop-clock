#ifndef EPD_GUI_PAINT_H
#define EPD_GUI_PAINT_H

#include "../fonts/fonts.h"
#include "../fonts/fonts_cn.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t UDOUBLE;

typedef struct {
    UBYTE *Image;
    UWORD Width;
    UWORD Height;
    UWORD WidthMemory;
    UWORD HeightMemory;
    UWORD Color;
    UWORD Rotate;
    UWORD Mirror;
    UWORD WidthByte;
    UWORD HeightByte;
} PAINT;

extern PAINT Paint;

#define ROTATE_0   0U
#define ROTATE_90  90U
#define ROTATE_180 180U
#define ROTATE_270 270U

typedef enum {
    MIRROR_NONE = 0x00,
    MIRROR_HORIZONTAL = 0x01,
    MIRROR_VERTICAL = 0x02,
    MIRROR_ORIGIN = 0x03
} MIRROR_IMAGE;
#define MIRROR_IMAGE_DFT MIRROR_NONE

#define WHITE 0xFFU
#define BLACK 0x00U
#define RED BLACK
#define IMAGE_BACKGROUND WHITE
#define FONT_FOREGROUND BLACK
#define FONT_BACKGROUND WHITE

typedef enum {
    DOT_PIXEL_1X1 = 1,
    DOT_PIXEL_2X2,
    DOT_PIXEL_3X3,
    DOT_PIXEL_4X4,
    DOT_PIXEL_5X5,
    DOT_PIXEL_6X6,
    DOT_PIXEL_7X7,
    DOT_PIXEL_8X8
} DOT_PIXEL;
#define DOT_PIXEL_DFT DOT_PIXEL_1X1

typedef enum {
    DOT_FILL_AROUND = 1,
    DOT_FILL_RIGHTUP
} DOT_STYLE;
#define DOT_STYLE_DFT DOT_FILL_AROUND

typedef enum {
    LINE_STYLE_SOLID = 0,
    LINE_STYLE_DOTTED
} LINE_STYLE;

typedef enum {
    DRAW_FILL_EMPTY = 0,
    DRAW_FILL_FULL
} DRAW_FILL;

typedef struct {
    UWORD Year;
    UBYTE Month;
    UBYTE Day;
    UBYTE Hour;
    UBYTE Min;
    UBYTE Sec;
} PAINT_TIME;

/* A single caller owns the canvas. The image must hold WidthByte * Height bytes. */
void Paint_NewImage(UBYTE *image, UWORD Width, UWORD Height,
                    UWORD Rotate, UWORD Color);
void Paint_SelectImage(UBYTE *image);
void Paint_SetRotate(UWORD Rotate);
void Paint_SetMirroring(UBYTE mirror);
void Paint_SetPixel(UWORD Xpoint, UWORD Ypoint, UWORD Color);
void Paint_Clear(UWORD Color);
/* Clear window end coordinates are exclusive. */
void Paint_ClearWindows(UWORD Xstart, UWORD Ystart, UWORD Xend,
                        UWORD Yend, UWORD Color);

/* Geometry uses inclusive endpoints. AROUND keeps the vendor 2*N-1 span. */
void Paint_DrawPoint(UWORD Xpoint, UWORD Ypoint, UWORD Color,
                     DOT_PIXEL Dot_Pixel, DOT_STYLE Dot_FillWay);
void Paint_DrawLine(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend,
                    UWORD Color, LINE_STYLE Line_Style, DOT_PIXEL Dot_Pixel);
void Paint_DrawRectangle(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend,
                         UWORD Color, DRAW_FILL Filled, DOT_PIXEL Dot_Pixel);
void Paint_DrawCircle(UWORD X_Center, UWORD Y_Center, UWORD Radius,
                      UWORD Color, DRAW_FILL Draw_Fill, DOT_PIXEL Dot_Pixel);

/* WHITE background is transparent, as in the vendor GUI. Clear before redraw. */
void Paint_DrawChar(UWORD Xstart, UWORD Ystart, char Ascii_Char,
                    const sFONT *Font, UWORD Color_Background,
                    UWORD Color_Foreground);
/* Explicit GB2312 bytes, no transcoding or terminator requirement. */
void Paint_DrawString_CN(UWORD x, UWORD y, const uint8_t *gb2312,
                          uint16_t length, const cFONT *font,
                          UWORD background, UWORD foreground);
void Paint_DrawCharScaled(UWORD Xstart, UWORD Ystart, char Ascii_Char,
                          const sFONT *Font, UWORD Color_Background,
                          UWORD Color_Foreground, UBYTE scale);
/* Text wraps horizontally; it stops at the bottom instead of overwriting. */
void Paint_DrawString_EN(UWORD Xstart, UWORD Ystart, const char *pString,
                         const sFONT *Font, UWORD Color_Background,
                         UWORD Color_Foreground);
void Paint_DrawString_ENScaled(UWORD Xstart, UWORD Ystart,
                               const char *pString, const sFONT *Font,
                               UWORD Color_Background, UWORD Color_Foreground,
                               UBYTE scale);
void Paint_DrawNum(UWORD Xpoint, UWORD Ypoint, int32_t Number,
                   const sFONT *Font, UWORD Color_Background,
                   UWORD Color_Foreground);
void Paint_DrawTime(UWORD Xstart, UWORD Ystart, const PAINT_TIME *pTime,
                    const sFONT *Font, UWORD Color_Background,
                    UWORD Color_Foreground);
/* Copies native packed RAM bytes; rotation and mirroring do not apply. */
void Paint_DrawBitMap(const unsigned char *image_buffer);

#ifdef __cplusplus
}
#endif

#endif
