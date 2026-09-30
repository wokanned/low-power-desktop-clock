#include "stm32_gdey029t94.h"

#include "../Core/Inc/main.h"
#include "../Core/Inc/spi.h"

#include <stddef.h>

/*
 * 平台适配层：
 * 将通用墨水屏回调接口转换为 STM32 HAL 调用。
 * 所有 MCU 句柄和 CubeMX 生成的引脚符号都集中在本文件。
 */
static int32_t stm32_epd_spi_write(void *handle,
                                   const uint8_t *data,
                                   uint32_t length,
                                   uint32_t timeout_ms)
{
    stm32_gdey029t94_context_t *context;
    SPI_HandleTypeDef *spi_handle;

    if ((handle == NULL) || (data == NULL) ||
        (length == 0U) || (length > 0xFFFFU)) {
        return -1;
    }

    context = (stm32_gdey029t94_context_t *)handle;
    spi_handle = (SPI_HandleTypeDef *)context->spi_handle;
    if (spi_handle == NULL) {
        return -1;
    }

    /* 通用驱动使用 uint32_t 长度，HAL SPI 接口使用 uint16_t。 */
    if (HAL_SPI_Transmit(spi_handle,
                         (uint8_t *)data,
                         (uint16_t)length,
                         timeout_ms) != HAL_OK) {
        return -1;
    }

    return 0;
}

static int32_t stm32_epd_read_busy(void *handle, uint8_t *busy_level)
{
    (void)handle;

    if (busy_level == NULL) {
        return -1;
    }

    /* 屏幕 BUSY 为高电平有效，设备层只接收统一的 0/1 状态。 */
    *busy_level =
        (HAL_GPIO_ReadPin(BUSY_GPIO_Port, BUSY_Pin) == GPIO_PIN_SET) ? 1U : 0U;

    return 0;
}

static void stm32_epd_set_cs(void *handle, uint8_t level)
{
    (void)handle;
    HAL_GPIO_WritePin(CS_GPIO_Port,
                      CS_Pin,
                      (level != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void stm32_epd_set_dc(void *handle, uint8_t level)
{
    (void)handle;
    HAL_GPIO_WritePin(D_C_GPIO_Port,
                      D_C_Pin,
                      (level != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void stm32_epd_set_reset(void *handle, uint8_t level)
{
    (void)handle;
    HAL_GPIO_WritePin(RES_GPIO_Port,
                      RES_Pin,
                      (level != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void stm32_epd_delay_ms(void *handle, uint32_t delay_ms)
{
    (void)handle;
    HAL_Delay(delay_ms);
}

static uint32_t stm32_epd_get_tick_ms(void *handle)
{
    (void)handle;
    return HAL_GetTick();
}

epd_gdey029t94_status_t stm32_gdey029t94_make_io(
    epd_gdey029t94_io_t *io,
    stm32_gdey029t94_context_t *context)
{
    if ((io == NULL) || (context == NULL)) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    /* 所有板级连线绑定集中在一个构造函数中。 */
    context->spi_handle = &hspi2;

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
