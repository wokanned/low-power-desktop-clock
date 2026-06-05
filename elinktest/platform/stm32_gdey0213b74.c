#include "stm32_gdey0213b74.h"

/*
 * Platform layer for the STM32L4 board used by elinktest.
 * This file is the only place where HAL handles, CubeMX pin names, and retry
 * policy are tied to the generic device-layer callback table.
 */

#define STM32_GDEY0213B74_SPI_TIMEOUT_MS 100U
#define STM32_GDEY0213B74_SPI_BAUDRATE_PRESCALER SPI_BAUDRATEPRESCALER_256

static int32_t stm32_gdey0213b74_configure_spi(SPI_HandleTypeDef *spi_handle)
{
    if (spi_handle == 0)
    {
        return -1;
    }

    if ((spi_handle->Init.BaudRatePrescaler == STM32_GDEY0213B74_SPI_BAUDRATE_PRESCALER) &&
        (spi_handle->Init.NSSPMode == SPI_NSS_PULSE_DISABLE))
    {
        return 0;
    }

    if (HAL_SPI_DeInit(spi_handle) != HAL_OK)
    {
        return -1;
    }

    spi_handle->Init.BaudRatePrescaler = STM32_GDEY0213B74_SPI_BAUDRATE_PRESCALER;
    spi_handle->Init.NSSPMode = SPI_NSS_PULSE_DISABLE;

    if (HAL_SPI_Init(spi_handle) != HAL_OK)
    {
        return -1;
    }

    return 0;
}

/*
 * Send one SPI transaction.
 * CS is toggled by the device layer, so each call here corresponds to exactly
 * one byte or one command with CS already held low by the caller.
 */
static int32_t stm32_gdey0213b74_spi_write(void *handle, const uint8_t *data, uint16_t length)
{
    HAL_StatusTypeDef hal_status;
    stm32_gdey0213b74_bus_t *bus;

    bus = (stm32_gdey0213b74_bus_t *)handle;
    if ((bus == 0) || (bus->spi_handle == 0) || ((data == 0) && (length > 0U)))
    {
        return -1;
    }

    if (length == 0U)
    {
        return 0;
    }

    hal_status = HAL_SPI_Transmit(bus->spi_handle, (uint8_t *)data, length, bus->spi_timeout_ms);
    if (hal_status == HAL_OK)
    {
        return 0;
    }

    return -1;
}

/* BUSY is sampled through GPIO so the device layer can stay HAL-free. */
static int32_t stm32_gdey0213b74_read_busy(void *handle, uint8_t *busy_level)
{
    stm32_gdey0213b74_bus_t *bus;

    bus = (stm32_gdey0213b74_bus_t *)handle;
    if ((bus == 0) || (busy_level == 0))
    {
        return -1;
    }

    *busy_level = (HAL_GPIO_ReadPin(bus->busy_port, bus->busy_pin) == GPIO_PIN_SET) ? 1U : 0U;
    return 0;
}

/* These three helpers isolate the board-specific control pins behind callbacks. */
static void stm32_gdey0213b74_set_cs(void *handle, uint8_t level)
{
    stm32_gdey0213b74_bus_t *bus;

    bus = (stm32_gdey0213b74_bus_t *)handle;
    if (bus == 0)
    {
        return;
    }

    HAL_GPIO_WritePin(bus->cs_port, bus->cs_pin, (level != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void stm32_gdey0213b74_set_dc(void *handle, uint8_t level)
{
    stm32_gdey0213b74_bus_t *bus;

    bus = (stm32_gdey0213b74_bus_t *)handle;
    if (bus == 0)
    {
        return;
    }

    HAL_GPIO_WritePin(bus->dc_port, bus->dc_pin, (level != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void stm32_gdey0213b74_set_reset(void *handle, uint8_t level)
{
    stm32_gdey0213b74_bus_t *bus;

    bus = (stm32_gdey0213b74_bus_t *)handle;
    if (bus == 0)
    {
        return;
    }

    HAL_GPIO_WritePin(bus->reset_port, bus->reset_pin, (level != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* Delay comes from HAL today, but the device layer only sees a callback. */
static void stm32_gdey0213b74_delay_ms(void *handle, uint32_t delay_ms)
{
    (void)handle;
    HAL_Delay(delay_ms);
}

/* Tick callback is used both for BUSY timeout protection and for demo text. */
static uint32_t stm32_gdey0213b74_get_tick_ms(void *handle)
{
    (void)handle;
    return HAL_GetTick();
}

/*
 * Gather all CubeMX-generated resources in one struct.
 * A later board migration usually only needs to touch this function and the
 * matching header, while the device layer remains unchanged.
 */
int32_t stm32_gdey0213b74_bus_init(stm32_gdey0213b74_bus_t *bus)
{
    if (bus == 0)
    {
        return -1;
    }

    bus->spi_handle = &hspi2;
    bus->busy_port = BUSY_GPIO_Port;
    bus->busy_pin = BUSY_Pin;
    bus->reset_port = RES_GPIO_Port;
    bus->reset_pin = RES_Pin;
    bus->dc_port = DC_GPIO_Port;
    bus->dc_pin = DC_Pin;
    bus->cs_port = CS2_GPIO_Port;
    bus->cs_pin = CS2_Pin;
    bus->spi_timeout_ms = STM32_GDEY0213B74_SPI_TIMEOUT_MS;

    /*
     * Drive the display control pins to a safe idle state before touching the
     * SPI peripheral.  CubeMX GPIO init leaves CS low; de-initializing SPI
     * with CS low would expose the panel to random noise on floating MOSI/SCK.
     */
    HAL_GPIO_WritePin(bus->cs_port, bus->cs_pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(bus->dc_port, bus->dc_pin, GPIO_PIN_RESET);

    /*
     * The vendor demo uses a slow software SPI and frames every byte with CS.
     * Start the hardware SPI probe from the same conservative timing envelope
     * to reduce the chance of corrupted panel RAM writes during bring-up.
     */
    if (stm32_gdey0213b74_configure_spi(bus->spi_handle) != 0)
    {
        return -1;
    }

    return 0;
}

/*
 * Export the STM32 bus description as the generic callback table expected by
 * the device layer.
 */
int32_t stm32_gdey0213b74_fill_io(const stm32_gdey0213b74_bus_t *bus, epd_gdey0213b74_io_t *io)
{
    if ((bus == 0) || (io == 0) || (bus->spi_handle == 0))
    {
        return -1;
    }

    io->spi_write = stm32_gdey0213b74_spi_write;
    io->read_busy = stm32_gdey0213b74_read_busy;
    io->set_cs = stm32_gdey0213b74_set_cs;
    io->set_dc = stm32_gdey0213b74_set_dc;
    io->set_reset = stm32_gdey0213b74_set_reset;
    io->delay_ms = stm32_gdey0213b74_delay_ms;
    io->get_tick_ms = stm32_gdey0213b74_get_tick_ms;
    io->handle = (void *)bus;

    return 0;
}
