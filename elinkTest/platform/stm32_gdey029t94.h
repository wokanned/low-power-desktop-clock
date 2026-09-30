#ifndef STM32_GDEY029T94_H
#define STM32_GDEY029T94_H

#include "../device/epd_gdey029t94.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 板级上下文会随回调传回通用设备驱动。 */
    void *spi_handle;
} stm32_gdey029t94_context_t;

epd_gdey029t94_status_t stm32_gdey029t94_make_io(
    epd_gdey029t94_io_t *io,
    stm32_gdey029t94_context_t *context);

#ifdef __cplusplus
}
#endif

#endif
