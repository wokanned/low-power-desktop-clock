#ifndef EPD_BRINGUP_H
#define EPD_BRINGUP_H

#include "../device/epd_gdey029t94.h"

#ifdef __cplusplus
extern "C" {
#endif

epd_gdey029t94_status_t epd_bringup_run_once(void);
epd_gdey029t94_status_t epd_bringup_run_clock(void);
epd_gdey029t94_status_t epd_bringup_get_last_status(void);

#ifdef __cplusplus
}
#endif

#endif
