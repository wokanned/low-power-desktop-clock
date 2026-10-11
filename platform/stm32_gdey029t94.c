#include "stm32_gdey029t94.h"

#include "main.h"
#include "spi.h"
#include "cmsis_os2.h"

#include <stddef.h>

#define STM32_EPD_SPI_MAX_TRANSFER       0xFFFFU
#define STM32_EPD_MS_PER_SECOND          1000U
#define STM32_EPD_MAX_DELAY_TICKS        0xFFFFFFFEU
/* Engineering settling margin; verify the switched rail on the target board. */
#define STM32_EPD_POWER_SETTLE_MS        10U
/* Project power-tree convention in docs/totalPLAN.md: SY6288 EN is active low. */
#define STM32_EPD_POWER_ON_LEVEL         GPIO_PIN_RESET

static void stm32_epd_write_pin(const stm32_gdey029t94_pin_t *p_pin,
                                uint8_t level)
{
    HAL_GPIO_WritePin((GPIO_TypeDef *)p_pin->port, p_pin->pin,
                      (level != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static int32_t stm32_epd_spi_write(void *handle,
                                  const uint8_t *data,
                                  uint32_t length,
                                  uint32_t timeout_ms)
{
    stm32_gdey029t94_context_t *p_context;
    SPI_HandleTypeDef *p_spi;

    if ((handle == NULL) || (data == NULL) || (length == 0U) ||
        (length > STM32_EPD_SPI_MAX_TRANSFER) ||
        (timeout_ms == HAL_MAX_DELAY)) {
        return -1;
    }
    p_context = (stm32_gdey029t94_context_t *)handle;
    p_spi = (SPI_HandleTypeDef *)p_context->spi_handle;
    if (p_spi == NULL) {
        return -1;
    }
    /* Synchronous transfer: the driver also passes stack-resident commands.
       Only the display task owns SPI2 and the controller transaction state. */
    if (HAL_SPI_Transmit(p_spi, (uint8_t *)data, (uint16_t)length,
                         timeout_ms) != HAL_OK) {
        return -1;
    }
    return 0;
}

static int32_t stm32_epd_read_busy(void *handle, uint8_t *busy_level)
{
    stm32_gdey029t94_context_t *p_context;

    if ((handle == NULL) || (busy_level == NULL)) {
        return -1;
    }
    p_context = (stm32_gdey029t94_context_t *)handle;
    /* The tested GDEY029T94 driver uses a high-active BUSY signal. */
    *busy_level = (HAL_GPIO_ReadPin((GPIO_TypeDef *)p_context->busy.port,
                                   p_context->busy.pin) == GPIO_PIN_SET)
                  ? 1U : 0U;
    return 0;
}

static void stm32_epd_set_cs(void *handle, uint8_t level)
{
    stm32_gdey029t94_context_t *p_context;
    p_context = (stm32_gdey029t94_context_t *)handle;
    stm32_epd_write_pin(&p_context->cs, level);
}

static void stm32_epd_set_dc(void *handle, uint8_t level)
{
    stm32_gdey029t94_context_t *p_context;
    p_context = (stm32_gdey029t94_context_t *)handle;
    stm32_epd_write_pin(&p_context->dc, level);
}

static void stm32_epd_set_reset(void *handle, uint8_t level)
{
    stm32_gdey029t94_context_t *p_context;
    p_context = (stm32_gdey029t94_context_t *)handle;
    stm32_epd_write_pin(&p_context->reset, level);
}

static void stm32_epd_delay_ms(void *handle, uint32_t delay_ms)
{
    uint64_t ticks;
    (void)handle;

    if (delay_ms == 0U) {
        return;
    }
    if ((osKernelGetState() == osKernelRunning) && (__get_IPSR() == 0U)) {
        /* Add a tick to preserve the minimum hardware delay when called just
           before a tick boundary. BUSY polling must let the debug task run. */
        ticks = (((uint64_t)delay_ms * osKernelGetTickFreq()) +
                 STM32_EPD_MS_PER_SECOND - 1U) / STM32_EPD_MS_PER_SECOND;
        ticks++;
        if (ticks > STM32_EPD_MAX_DELAY_TICKS) {
            ticks = STM32_EPD_MAX_DELAY_TICKS;
        }
        (void)osDelay((uint32_t)ticks);
    } else {
        /* Supports initialization before the scheduler. Never call from ISR. */
        HAL_Delay(delay_ms);
    }
}

static uint32_t stm32_epd_get_tick_ms(void *handle)
{
    (void)handle;
    /* HAL milliseconds come from TIM17; FreeRTOS owns SysTick. */
    return HAL_GetTick();
}

epd_gdey029t94_status_t stm32_gdey029t94_make_io(
    epd_gdey029t94_io_t *io,
    stm32_gdey029t94_context_t *context)
{
    if ((io == NULL) || (context == NULL)) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    /* Main-board netlist: H8.7=SCK/PB10, H8.6=MOSI/PC3.
       The display adapter swaps these positions: H1.6=SCK, H1.7=SDI. */
    context->spi_handle = &hspi2;
    context->cs.port = CS2_GPIO_Port;
    context->cs.pin = CS2_Pin;
    context->dc.port = DC_GPIO_Port;
    context->dc.pin = DC_Pin;
    context->reset.port = RES_GPIO_Port;
    context->reset.pin = RES_Pin;
    context->busy.port = BUSY_GPIO_Port;
    context->busy.pin = BUSY_Pin;
    context->power.port = ELINKEN_GPIO_Port;
    context->power.pin = ELINKEN_Pin;

    io->spi_write = stm32_epd_spi_write;
    io->read_busy = stm32_epd_read_busy;
    io->set_cs = stm32_epd_set_cs;
    io->set_dc = stm32_epd_set_dc;
    io->set_reset = stm32_epd_set_reset;
    io->delay_ms = stm32_epd_delay_ms;
    io->get_tick_ms = stm32_epd_get_tick_ms;
    io->handle = context;
    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t stm32_gdey029t94_prepare(
    stm32_gdey029t94_context_t *context)
{
    if ((context == NULL) || (context->spi_handle == NULL) ||
        (context->cs.port == NULL) || (context->dc.port == NULL) ||
        (context->reset.port == NULL) || (context->busy.port == NULL) ||
        (context->power.port == NULL)) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    stm32_epd_set_cs(context, 1U);
    stm32_epd_set_dc(context, 0U);
    stm32_epd_set_reset(context, 0U);
    HAL_GPIO_WritePin((GPIO_TypeDef *)context->power.port, context->power.pin,
                      STM32_EPD_POWER_ON_LEVEL);
    stm32_epd_delay_ms(context, STM32_EPD_POWER_SETTLE_MS);
    return EPD_GDEY029T94_OK;
}
