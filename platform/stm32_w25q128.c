#include "stm32_w25q128.h"
#include "spi.h"
#include "gpio.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "cmsis_os.h"
#include <stddef.h>

#define STM32_W25Q128_SPI_TIMEOUT_MS 100U
#define STM32_W25Q128_LOCK_TIMEOUT_MS 1000U

typedef struct {
    SPI_HandleTypeDef *p_spi;
    GPIO_TypeDef *p_cs_port;
    uint16_t cs_pin;
    SemaphoreHandle_t mutex;
} stm32_w25q128_bus_t;

static StaticSemaphore_t s_mutex_control;
static stm32_w25q128_bus_t s_bus;

static int32_t s_transaction(void *p_handle, const uint8_t *p_tx,
                             uint32_t tx_len, uint8_t *p_rx, uint32_t rx_len)
{
    stm32_w25q128_bus_t *p_bus = (stm32_w25q128_bus_t *)p_handle;
    HAL_StatusTypeDef result = HAL_OK;

    if (p_bus == NULL || p_bus->p_spi == NULL || p_bus->p_cs_port == NULL ||
        tx_len > UINT16_MAX || rx_len > UINT16_MAX ||
        (tx_len != 0U && p_tx == NULL) || (rx_len != 0U && p_rx == NULL)) {
        return -1;
    }
    HAL_GPIO_WritePin(p_bus->p_cs_port, p_bus->cs_pin, GPIO_PIN_RESET);
    if (tx_len != 0U) {
        result = HAL_SPI_Transmit(p_bus->p_spi, (uint8_t *)p_tx, (uint16_t)tx_len,
                                  STM32_W25Q128_SPI_TIMEOUT_MS);
    }
    if (result == HAL_OK && rx_len != 0U) {
        result = HAL_SPI_Receive(p_bus->p_spi, p_rx, (uint16_t)rx_len,
                                 STM32_W25Q128_SPI_TIMEOUT_MS);
    }
    HAL_GPIO_WritePin(p_bus->p_cs_port, p_bus->cs_pin, GPIO_PIN_SET);
    if (result != HAL_OK) {
        (void)HAL_SPI_Abort(p_bus->p_spi);
    }
    return result == HAL_OK ? 0 : -1;
}

static void s_delay_ms(void *p_handle, uint32_t delay_ms)
{
    uint32_t ticks;
    (void)p_handle;
    /* Add one tick so even a call just before a tick meets the minimum delay. */
    ticks = (uint32_t)(((uint64_t)delay_ms * configTICK_RATE_HZ + 999U) / 1000U);
    (void)osDelay(ticks + 1U);
}

static uint32_t s_tick_ms(void *p_handle)
{
    (void)p_handle;
    return HAL_GetTick();
}

static int32_t s_lock(void *p_handle)
{
    stm32_w25q128_bus_t *p_bus = (stm32_w25q128_bus_t *)p_handle;
    if (__get_IPSR() != 0U || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        p_bus == NULL || p_bus->mutex == NULL ||
        xSemaphoreTake(p_bus->mutex, pdMS_TO_TICKS(STM32_W25Q128_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return -1;
    }
    return 0;
}

static void s_unlock(void *p_handle)
{
    stm32_w25q128_bus_t *p_bus = (stm32_w25q128_bus_t *)p_handle;
    if (p_bus != NULL && p_bus->mutex != NULL) {
        (void)xSemaphoreGive(p_bus->mutex);
    }
}

int32_t stm32_w25q128_bind(w25q128_ctx_t *p_ctx)
{
    if (p_ctx == NULL || __get_IPSR() != 0U ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        return -1;
    }
    if (s_bus.mutex == NULL) {
        s_bus.mutex = xSemaphoreCreateMutexStatic(&s_mutex_control);
    }
    if (s_bus.mutex == NULL) {
        return -1;
    }
    s_bus.p_spi = &hspi1;
    s_bus.p_cs_port = CS1_GPIO_Port;
    s_bus.cs_pin = CS1_Pin;
    HAL_GPIO_WritePin(s_bus.p_cs_port, s_bus.cs_pin, GPIO_PIN_SET);
    p_ctx->transaction = s_transaction;
    p_ctx->delay_ms = s_delay_ms;
    p_ctx->tick_ms = s_tick_ms;
    p_ctx->lock = s_lock;
    p_ctx->unlock = s_unlock;
    p_ctx->p_handle = &s_bus;
    return 0;
}
