#ifndef EPD_GDEY0213B74_H
#define EPD_GDEY0213B74_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_GDEY0213B74_RAM_WIDTH_PIXELS          128U
#define EPD_GDEY0213B74_RAM_HEIGHT_PIXELS         250U
#define EPD_GDEY0213B74_VISIBLE_SHORT_SIDE_PIXELS 122U
#define EPD_GDEY0213B74_RAM_WIDTH_BYTES           (EPD_GDEY0213B74_RAM_WIDTH_PIXELS / 8U)
#define EPD_GDEY0213B74_FRAMEBUFFER_SIZE_BYTES    (EPD_GDEY0213B74_RAM_WIDTH_BYTES * EPD_GDEY0213B74_RAM_HEIGHT_PIXELS)
#define EPD_GDEY0213B74_CANVAS_WIDTH_PIXELS       EPD_GDEY0213B74_RAM_HEIGHT_PIXELS
#define EPD_GDEY0213B74_CANVAS_HEIGHT_PIXELS      EPD_GDEY0213B74_VISIBLE_SHORT_SIDE_PIXELS
#define EPD_GDEY0213B74_BUSY_TIMEOUT_MS           15000U

typedef enum
{
    EPD_GDEY0213B74_OK = 0,
    EPD_GDEY0213B74_ERR_PARAM = -1,
    EPD_GDEY0213B74_ERR_IO = -2,
    EPD_GDEY0213B74_ERR_TIMEOUT = -3
} epd_gdey0213b74_status_t;

typedef struct
{
    int32_t (*spi_write)(void *handle, const uint8_t *data, uint16_t length);
    int32_t (*read_busy)(void *handle, uint8_t *busy_level);
    void (*set_cs)(void *handle, uint8_t level);
    void (*set_dc)(void *handle, uint8_t level);
    void (*set_reset)(void *handle, uint8_t level);
    void (*delay_ms)(void *handle, uint32_t delay_ms);
    uint32_t (*get_tick_ms)(void *handle);
    void *handle;
} epd_gdey0213b74_io_t;

typedef struct
{
    epd_gdey0213b74_io_t io;
    uint8_t *framebuffer;
    uint32_t framebuffer_size;
} epd_gdey0213b74_t;

/* Bind the injected platform I/O functions and the caller-owned frame buffer. */
int32_t epd_gdey0213b74_bind(epd_gdey0213b74_t *display,
                             const epd_gdey0213b74_io_t *io,
                             uint8_t *framebuffer,
                             uint32_t framebuffer_size);

/* Run the panel initialization sequence defined by the datasheet. */
int32_t epd_gdey0213b74_panel_init(epd_gdey0213b74_t *display);

/* Write the full frame buffer to controller RAM and trigger one refresh. */
int32_t epd_gdey0213b74_display_frame(epd_gdey0213b74_t *display);

/* Put the panel into deep sleep after the image has been latched. */
int32_t epd_gdey0213b74_enter_deep_sleep(epd_gdey0213b74_t *display);

/* Fill the whole caller-owned frame buffer with one byte pattern. */
void epd_gdey0213b74_clear_framebuffer(epd_gdey0213b74_t *display, uint8_t color);

/* Draw one logical pixel using the same rotated memory layout as the vendor demo. */
void epd_gdey0213b74_set_pixel(epd_gdey0213b74_t *display, uint16_t x_point, uint16_t y_point, uint8_t black);

/* Draw a filled rectangle in logical landscape coordinates. */
void epd_gdey0213b74_fill_rect(epd_gdey0213b74_t *display,
                               uint16_t x_start,
                               uint16_t y_start,
                               uint16_t width,
                               uint16_t height,
                               uint8_t black);

#ifdef __cplusplus
}
#endif

#endif
