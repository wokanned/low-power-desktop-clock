#ifndef EPD_BRINGUP_H
#define EPD_BRINGUP_H

#include "../device/epd_gdey029t94.h"
#include "epd_vendor_demo.h"

/* Select one mode here or override it with a compiler definition. */
#ifndef EPD_GUI_DEMO
#define EPD_GUI_DEMO 0U
#endif
#ifndef EPD_CLOCK_DIAGNOSTIC_PATTERN
#define EPD_CLOCK_DIAGNOSTIC_PATTERN 0U
#endif
#ifndef EPD_CLOCK_DIAGNOSTIC_FORCE_FULL
#define EPD_CLOCK_DIAGNOSTIC_FORCE_FULL 0U
#endif
#if ((EPD_GUI_DEMO != 0) + (EPD_CLOCK_DIAGNOSTIC_PATTERN != 0) + \
     (EPD_CLOCK_DIAGNOSTIC_FORCE_FULL != 0)) > 1
#error Select only one EPD diagnostic or GUI demo mode
#endif
#if (EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF) && \
    (EPD_GUI_DEMO || EPD_CLOCK_DIAGNOSTIC_PATTERN || EPD_CLOCK_DIAGNOSTIC_FORCE_FULL)
#error Vendor demo requires all legacy diagnostic modes OFF
#endif

#ifdef __cplusplus
extern "C" {
#endif

epd_gdey029t94_status_t epd_bringup_run_once(void);
/* Run only from the single display task after the RTOS scheduler starts. */
epd_gdey029t94_status_t epd_bringup_run_clock(void);
epd_gdey029t94_status_t epd_bringup_get_last_status(void);
uint32_t epd_bringup_get_completed_frames(void);

#ifdef __cplusplus
}
#endif

#endif
