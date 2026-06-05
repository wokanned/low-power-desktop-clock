#ifndef STM32_GDEY0213B74_H
#define STM32_GDEY0213B74_H

#include "../Core/Inc/main.h"
#include "../Core/Inc/spi.h"
#include "../device/epd_gdey0213b74.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    SPI_HandleTypeDef *spi_handle;
    GPIO_TypeDef *busy_port;
    uint16_t busy_pin;
    GPIO_TypeDef *reset_port;
    uint16_t reset_pin;
    GPIO_TypeDef *dc_port;
    uint16_t dc_pin;
    GPIO_TypeDef *cs_port;
    uint16_t cs_pin;
    uint32_t spi_timeout_ms;
} stm32_gdey0213b74_bus_t;

/* Populate the STM32 bus description from the current CubeMX-generated resources. */
int32_t stm32_gdey0213b74_bus_init(stm32_gdey0213b74_bus_t *bus);

/* Convert the STM32 bus description into the device-layer injected I/O table. */
int32_t stm32_gdey0213b74_fill_io(const stm32_gdey0213b74_bus_t *bus, epd_gdey0213b74_io_t *io);

#ifdef __cplusplus
}
#endif

#endif
