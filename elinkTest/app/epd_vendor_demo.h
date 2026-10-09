#ifndef EPD_VENDOR_DEMO_H
#define EPD_VENDOR_DEMO_H
#include "../device/epd_gdey029t94.h"
#define EPD_VENDOR_DEMO_OFF          0
#define EPD_VENDOR_DEMO_FULL         1
#define EPD_VENDOR_DEMO_GRAY4        2
#define EPD_VENDOR_DEMO_PARTIAL_TIME 3
#define EPD_VENDOR_DEMO_PARTIAL_FULL 4
#define EPD_VENDOR_DEMO_ROTATE_180   5
#define EPD_VENDOR_DEMO_GUI_342      6
#ifndef EPD_VENDOR_DEMO_MODE
#define EPD_VENDOR_DEMO_MODE EPD_VENDOR_DEMO_FULL
#endif
#if (EPD_VENDOR_DEMO_MODE < 0) || (EPD_VENDOR_DEMO_MODE > 6)
#error Invalid EPD_VENDOR_DEMO_MODE
#endif

typedef struct {
    uint32_t stage;
    uint32_t completed_pages;
    uint32_t completed_batches;
    uint32_t cleanup_refreshes;
    epd_gdey029t94_status_t status;
    uint8_t completed;
    uint8_t recovered_to_clock;
} epd_vendor_demo_state_t;
extern volatile epd_vendor_demo_state_t epd_vendor_demo_state;

/* Native two-axis pixel transform; format is 1 or 2 bits/pixel.
   Source/destination must be distinct, dimensions byte aligned. */
void epd_vendor_image_rotate180(uint8_t *destination, const uint8_t *source,
                                uint16_t width, uint16_t height, uint8_t bpp);
void epd_vendor_gui_page(uint8_t *frame, uint8_t page);
/* Device/IO/frame are caller-owned. Success leaves the controller asleep;
   caller reinitializes BW296 before entering the clock. */
epd_gdey029t94_status_t epd_vendor_demo_run(
    epd_gdey029t94_t *device, const epd_gdey029t94_io_t *io, uint8_t *frame);
#endif
