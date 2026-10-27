#ifndef EPD_WEATHER_VIEW_H
#define EPD_WEATHER_VIEW_H

#include "clock_model.h"
#include "weather.h"
#include <stddef.h>
#include <stdint.h>

/* Pure native 296x128 monochrome or 2bpp weather-page rasterizer. */
uint8_t epd_weather_view_draw_bw(uint8_t *p_frame, size_t length,
                                 const weather_snapshot_t *p_weather,
                                 const clock_view_model_t *p_clock);
uint8_t epd_weather_view_draw_gray4(uint8_t *p_frame, size_t length,
                                    const weather_snapshot_t *p_weather,
                                    const clock_view_model_t *p_clock);

#endif
