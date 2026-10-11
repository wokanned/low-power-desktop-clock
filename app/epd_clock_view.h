#ifndef EPD_CLOCK_VIEW_H
#define EPD_CLOCK_VIEW_H

#include <stdint.h>

/* Landscape layout; Font24 contains the complete 17 x 24 ASCII cells. */
#define EPD_CLOCK_VIEW_X                    12U
#define EPD_CLOCK_VIEW_Y                    45U
#define EPD_CLOCK_VIEW_SCALE                2U
#define EPD_CLOCK_VIEW_FONT_WIDTH           17U
#define EPD_CLOCK_VIEW_FONT_HEIGHT          24U
#define EPD_CLOCK_VIEW_SECONDS_NATIVE_X     32U
#define EPD_CLOCK_VIEW_SECONDS_NATIVE_Y     12U
#define EPD_CLOCK_VIEW_SECONDS_NATIVE_WIDTH 56U
#define EPD_CLOCK_VIEW_SECONDS_NATIVE_HEIGHT 68U

/* The caller supplies a native 128 x 296, MSB-first, 4736-byte buffer. */
void epd_clock_view_draw(uint8_t *frame,
                         uint8_t hours, uint8_t minutes, uint8_t seconds);
void epd_clock_view_draw_orientation(uint8_t *frame);
void epd_clock_view_draw_demo(uint8_t *frame);

#endif
