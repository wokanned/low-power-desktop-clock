#include "stm32_gdey029t94.h"

#include "main.h"
#include "spi.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stddef.h>

#define STM32_EPD_SPI_MAX_TRANSFER       0xFFFFU
#define STM32_EPD_MS_PER_SECOND          1000U
#define STM32_EPD_MAX_DELAY_TICKS        0xFFFFFFFEU

static StaticSemaphore_t s_tx_semaphore_control;
static SemaphoreHandle_t s_tx_semaphore;
static volatile uint8_t s_tx_active;
static volatile uint8_t s_tx_waiting;
static volatile uint8_t s_tx_complete;
static volatile uint8_t s_tx_failed;

static void stm32_epd_spi_recover(SPI_HandleTypeDef *p_spi)
{
    /* Only the SPI2/DMA1 Channel5 IRQs are masked by the caller. HAL abort
       may poll FIFO/BSY; keep the RTOS tick and USART2 RX interrupts running. */
    (void)HAL_SPI_Abort(p_spi);

    /* SPI_DMAError clears TXDMAEN before calling ErrorCallback, so SPI abort
       alone can skip the DMA channel. Stop it even if HAL already says READY. */
    CLEAR_BIT(p_spi->Instance->CR2, SPI_CR2_TXDMAEN);
    __HAL_DMA_DISABLE_IT(p_spi->hdmatx, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE);
    __HAL_DMA_DISABLE(p_spi->hdmatx);
    (void)HAL_DMA_Abort(p_spi->hdmatx);
    __HAL_DMA_CLEAR_FLAG(p_spi->hdmatx,
                         __HAL_DMA_GET_GI_FLAG_INDEX(p_spi->hdmatx));
    __HAL_SPI_DISABLE(p_spi);

    /* Flush any partially shifted byte/FIFO after a failed transaction.
       Reset only SPI2: DMA1 Channel7 and USART2 RX remain independent. */
    __HAL_RCC_SPI2_FORCE_RESET();
    __HAL_RCC_SPI2_RELEASE_RESET();
    (void)HAL_SPI_Init(p_spi);
    HAL_NVIC_ClearPendingIRQ(DMA1_Channel5_IRQn);
    HAL_NVIC_ClearPendingIRQ(SPI2_IRQn);
}

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
    HAL_StatusTypeDef result;
    BaseType_t completed;
    uint64_t ticks;
    uint8_t success;

    if ((handle == NULL) || (data == NULL) || (length == 0U) ||
        (length > STM32_EPD_SPI_MAX_TRANSFER) ||
        (timeout_ms == 0U) || (timeout_ms == HAL_MAX_DELAY) ||
        (__get_IPSR() != 0U) || (__get_PRIMASK() != 0U) ||
        (__get_BASEPRI() != 0U) ||
        (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)) {
        return -1;
    }
    p_context = (stm32_gdey029t94_context_t *)handle;
    p_spi = (SPI_HandleTypeDef *)p_context->spi_handle;
    if ((p_spi != &hspi2) || (p_spi->hdmatx == NULL)) {
        return -1;
    }

    ticks = (((uint64_t)timeout_ms * configTICK_RATE_HZ) +
             STM32_EPD_MS_PER_SECOND - 1U) / STM32_EPD_MS_PER_SECOND;
    if (ticks > STM32_EPD_MAX_DELAY_TICKS) {
        ticks = STM32_EPD_MAX_DELAY_TICKS;
    }

    /* Single owner: displayTask. This synchronous contract retains stack
       commands and gray4 encoding blocks until wire completion or stop.
       A private semaphore never consumes RX/page/queue task notifications. */
    taskENTER_CRITICAL();
    if (s_tx_active != 0U) {
        taskEXIT_CRITICAL();
        return -1;
    }
    if (s_tx_semaphore == NULL) {
        s_tx_semaphore = xSemaphoreCreateBinaryStatic(&s_tx_semaphore_control);
    }
    if (s_tx_semaphore == NULL) {
        taskEXIT_CRITICAL();
        return -1;
    }
    (void)xSemaphoreTake(s_tx_semaphore, 0U);
    s_tx_complete = 0U;
    s_tx_failed = 0U;
    s_tx_active = 1U;
    s_tx_waiting = 1U;
    result = HAL_SPI_Transmit_DMA(p_spi, (uint8_t *)data, (uint16_t)length);
    taskEXIT_CRITICAL();

    completed = pdFALSE;
    if (result == HAL_OK) {
        completed = xSemaphoreTake(s_tx_semaphore, (TickType_t)ticks);
    }
    taskENTER_CRITICAL();
    success = (uint8_t)((completed == pdTRUE) && (s_tx_complete != 0U) &&
                        (s_tx_failed == 0U));
    s_tx_waiting = 0U;
    if (success == 0U) {
        /* Freeze only this peripheral's callbacks while aborting outside
           the critical section. Keep ownership until recovery has finished. */
        HAL_NVIC_DisableIRQ(DMA1_Channel5_IRQn);
        HAL_NVIC_DisableIRQ(SPI2_IRQn);
    }
    taskEXIT_CRITICAL();

    if (success == 0U) {
        stm32_epd_spi_recover(p_spi);
    }
    taskENTER_CRITICAL();
    s_tx_active = 0U;
    if (success == 0U) {
        HAL_NVIC_EnableIRQ(SPI2_IRQn);
        HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);
    }
    taskEXIT_CRITICAL();
    return (success != 0U) ? 0 : -1;
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *p_spi)
{
    BaseType_t higher_priority = pdFALSE;
    if ((p_spi != &hspi2) || (s_tx_waiting == 0U)) { return; }
    /* Normal-mode HAL completion has drained TX FIFO and waited for BSY=0.
       Panel refresh completion is still the device driver's BUSY wait. */
    s_tx_complete = 1U;
    (void)xSemaphoreGiveFromISR(s_tx_semaphore, &higher_priority);
    portYIELD_FROM_ISR(higher_priority);
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *p_spi)
{
    BaseType_t higher_priority = pdFALSE;
    if ((p_spi != &hspi2) || (s_tx_waiting == 0U)) { return; }
    /* Failure stays latched even if a completion was already pending. */
    s_tx_failed = 1U;
    (void)xSemaphoreGiveFromISR(s_tx_semaphore, &higher_priority);
    portYIELD_FROM_ISR(higher_priority);
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
        (context->reset.port == NULL) || (context->busy.port == NULL)) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    stm32_epd_set_cs(context, 1U);
    stm32_epd_set_dc(context, 0U);
    stm32_epd_set_reset(context, 0U);
    return EPD_GDEY029T94_OK;
}
