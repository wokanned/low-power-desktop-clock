#ifndef EPD_CLOCK_VIEW_H
#define EPD_CLOCK_VIEW_H

#include <stdint.h>
#include <stddef.h>
#include "clock_model.h"

#define EPD_CLOCK_VIEW_GRAY4_BYTES 9472U
#define EPD_CLOCK_VIEW_WIDTH       296U
#define EPD_CLOCK_VIEW_HEIGHT      128U

/* Pure renderer; native 128x296, MSB-first 2bpp, 0=white and 3=black.
 * Returns 1 on success, 0 for invalid buffers/models or layout overflow.
 * Does not access the controller, the RTOS, or the global vendor Paint. */
uint8_t epd_clock_view_draw_gray4(uint8_t *frame, size_t length,
                                const clock_view_model_t *p_model);

/* Legacy BW diagnostics only; retained for imported vendor compatibility. */
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
