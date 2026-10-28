#ifndef STM32_GDEY029T94_H
#define STM32_GDEY029T94_H

#include "../device/epd_gdey029t94.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *port;
    uint16_t pin;
} stm32_gdey029t94_pin_t;

typedef struct {
    /* Each callback consumes the board resources passed through io.handle. */
    void *spi_handle;
    stm32_gdey029t94_pin_t cs;
    stm32_gdey029t94_pin_t dc;
    stm32_gdey029t94_pin_t reset;
    stm32_gdey029t94_pin_t busy;
} stm32_gdey029t94_context_t;

/* Bind SPI2 and the DeskClock main-board netlist pins. Does not perform IO.
   SPI writes use DMA with a private completion semaphore and return only after
   completion or confirmed stop. Call only from displayTask with RTOS running. */
epd_gdey029t94_status_t stm32_gdey029t94_make_io(
    epd_gdey029t94_io_t *io,
    stm32_gdey029t94_context_t *context);

/* Call after make_io and before controller init. CS inactive, reset low.
   The display supply is continuous; no power-enable GPIO is accessed. */
epd_gdey029t94_status_t stm32_gdey029t94_prepare(
    stm32_gdey029t94_context_t *context);

#ifdef __cplusplus
}
#endif

#endif
