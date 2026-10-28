#include "stm32_gdey029t94.h"
#include "stm32_debug_uart.h"
#include "spi.h"
#include "usart.h"
#include "cmsis_os2.h"
#include "semphr.h"
#include "task.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Model the asynchronous boundary and real device/UART adapters together.
   Hardware flags, IRQ masking, and buffer use are checked independently of
   the platform's private bookkeeping. This is not a peripheral timing model. */
GPIO_TypeDef g_test_gpio;
SPI_HandleTypeDef hspi2;
UART_HandleTypeDef huart2;
static SPI_TypeDef s_spi_registers;
static DMA_Channel_TypeDef s_dma_registers;
static DMA_HandleTypeDef s_dma;
static DMA_HandleTypeDef s_uart_dma;
static epd_gdey029t94_io_t s_io;
static stm32_gdey029t94_context_t s_context;
static SemaphoreHandle_t s_spi_semaphore;
static SemaphoreHandle_t s_uart_semaphore;
static uint32_t s_critical;
static uint32_t s_primask;
static uint32_t s_basepri;
static uint32_t s_ipsr;
static BaseType_t s_scheduler = taskSCHEDULER_RUNNING;
static uint32_t s_fail_semaphore;
static uint32_t s_creations;
static uint32_t s_spi_starts;
static uint32_t s_spi_aborts;
static uint32_t s_dma_aborts;
static uint32_t s_resets;
static uint32_t s_uart_tx_starts;
static uint32_t s_uart_rx_aborts;
static uint32_t s_notifications;
static uint32_t s_tick;
static uint32_t s_spi_irq_enabled = 1U;
static uint32_t s_dma_irq_enabled = 1U;
static uint32_t s_uart_irq_enabled = 1U;
static uint32_t s_early_pending;
static HAL_StatusTypeDef s_start_status = HAL_OK;
static uint8_t *s_rx_byte;
static uint8_t *s_inflight;
static uint16_t s_inflight_size;
static uint8_t s_snapshot[EPD_GDEY029T94_GRAY4_SIZE];
static uint8_t s_dc;
static uint8_t s_cs = 1U;
static uint8_t s_activation;
static uint8_t s_hold_busy;
static uint32_t s_busy_remaining;
static uint32_t s_busy_reads;
static epd_gdey029t94_t *s_observed_device;
static uint32_t s_expected_refreshes;
static uint32_t s_checked_transfers;
static TickType_t s_last_spi_wait;
static uint32_t s_recovery_reentries;
static HAL_StatusTypeDef s_dma_abort_status = HAL_OK;

typedef enum {
    TX_COMPLETE, TX_EARLY, TX_TIMEOUT, TX_DMA_ERROR,
    TX_ERROR_LATE_COMPLETE, TX_SPI_ERROR, TX_FOREIGN, TX_RX_ONLY,
    TX_UART_CONCURRENT, TX_REENTER
} tx_mode_t;
static tx_mode_t s_mode;
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *p_spi);
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *p_spi);
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *p_uart);
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *p_uart);

static void check_buffer(void)
{
    if (s_inflight != NULL) {
        assert(memcmp(s_inflight, s_snapshot, s_inflight_size) == 0);
        s_checked_transfers++;
    }
}

static void complete_spi(void)
{
    check_buffer();
    s_dma_registers.CCR &= ~DMA_CCR_EN;
    s_spi_registers.CR2 &= ~SPI_CR2_TXDMAEN;
    hspi2.State = HAL_SPI_STATE_READY;
    s_inflight = NULL;
    s_ipsr = 1U;
    HAL_SPI_TxCpltCallback(&hspi2);
    s_ipsr = 0U;
}

static void fail_spi(void)
{
    check_buffer();
    /* HAL SPI_DMAError clears request and marks READY before notifying the
       application. Leave channel EN set to exercise explicit stop cleanup. */
    s_spi_registers.CR2 &= ~SPI_CR2_TXDMAEN;
    hspi2.State = HAL_SPI_STATE_READY;
    hspi2.ErrorCode = HAL_SPI_ERROR_DMA;
    s_ipsr = 1U;
    HAL_SPI_ErrorCallback(&hspi2);
    s_ipsr = 0U;
}

static void notify_rx(void) { s_notifications++; }

static void receive_text(const char *p_text)
{
    assert(s_uart_irq_enabled != 0U && s_critical == 0U && s_primask == 0U);
    while (*p_text != '\0') {
        assert(s_rx_byte != NULL && huart2.RxState == 1U);
        *s_rx_byte = (uint8_t)*p_text++;
        huart2.RxState = 0U;
        s_ipsr = 1U;
        HAL_UART_RxCpltCallback(&huart2);
        s_ipsr = 0U;
    }
}

static void expect_rx(const char *p_text)
{
    uint8_t byte;
    uint32_t errors;
    while (*p_text != '\0') {
        assert(stm32_debug_uart_receive(&byte, &errors));
        assert(errors == 0U && byte == (uint8_t)*p_text++);
    }
    assert(!stm32_debug_uart_receive(&byte, &errors) && errors == 0U);
}

void test_enter_critical(void) { s_critical++; }
void test_exit_critical(void)
{
    assert(s_critical != 0U);
    s_critical--;
    if (s_critical == 0U && s_early_pending != 0U) {
        s_early_pending = 0U;
        complete_spi();
    }
}
uint32_t __get_PRIMASK(void) { return s_primask; }
uint32_t __get_BASEPRI(void) { return s_basepri; }
uint32_t __get_IPSR(void) { return s_ipsr; }
void __disable_irq(void) { s_primask = 1U; }
void __set_PRIMASK(uint32_t state) { s_primask = state; }
BaseType_t xTaskGetSchedulerState(void) { return s_scheduler; }
TickType_t xTaskGetTickCount(void) { return s_tick; }
uint32_t HAL_GetTick(void) { return s_tick; }
void HAL_Delay(uint32_t delay) { s_tick += delay; }
osKernelState_t osKernelGetState(void)
{ return s_scheduler == taskSCHEDULER_RUNNING ? osKernelRunning : osKernelReady; }
uint32_t osKernelGetTickFreq(void) { return configTICK_RATE_HZ; }
int32_t osDelay(uint32_t ticks)
{
    assert(s_critical == 0U && s_primask == 0U && s_cs != 0U);
    s_tick += ticks;
    return 0;
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *p_control)
{
    s_creations++;
    if (s_fail_semaphore != 0U) { s_fail_semaphore = 0U; return NULL; }
    p_control->count = 0U;
    if (s_spi_semaphore == NULL) { s_spi_semaphore = p_control; }
    else { assert(p_control != s_spi_semaphore); s_uart_semaphore = p_control; }
    return p_control;
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t *p_woken)
{
    assert(semaphore != NULL && s_ipsr != 0U && s_critical == 0U);
    semaphore->count = 1U;
    *p_woken = pdTRUE;
    return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait)
{
    static const uint8_t uart_frame[] = {0x12U, 0x34U};
    SPI_HandleTypeDef foreign;
    uint8_t nested_data = 0xFFU;
    if (wait != 0U) {
        assert(s_critical == 0U && s_primask == 0U && s_basepri == 0U);
        if (semaphore == s_uart_semaphore) {
            assert(semaphore != s_spi_semaphore);
            huart2.gState = HAL_UART_STATE_READY;
            s_ipsr = 1U;
            HAL_UART_TxCpltCallback(&huart2);
            s_ipsr = 0U;
        } else {
            assert(semaphore == s_spi_semaphore);
            s_last_spi_wait = wait;
            check_buffer();
            if (semaphore->count == 0U) {
                switch (s_mode) {
                case TX_COMPLETE: complete_spi(); break;
                case TX_EARLY: assert(0); break;
                case TX_DMA_ERROR: fail_spi(); break;
                case TX_ERROR_LATE_COMPLETE:
                    fail_spi(); complete_spi(); break;
                case TX_SPI_ERROR:
                    hspi2.ErrorCode = HAL_SPI_ERROR_MODF;
                    s_ipsr = 1U;
                    HAL_SPI_ErrorCallback(&hspi2);
                    s_ipsr = 0U;
                    break;
                case TX_FOREIGN:
                    memset(&foreign, 0, sizeof(foreign));
                    s_ipsr = 1U;
                    HAL_SPI_TxCpltCallback(&foreign);
                    HAL_SPI_ErrorCallback(&foreign);
                    s_ipsr = 0U;
                    break;
                case TX_RX_ONLY: receive_text("RX ONLY\n"); break;
                case TX_UART_CONCURRENT:
                    receive_text("DURING DMA\n");
                    assert(stm32_debug_uart_send(NULL, uart_frame, sizeof(uart_frame)));
                    assert(semaphore->count == 0U);
                    complete_spi();
                    break;
                case TX_REENTER:
                    assert(s_io.spi_write(s_io.handle, &nested_data, 1U, 50U) != 0);
                    complete_spi();
                    break;
                case TX_TIMEOUT: break;
                }
            }
        }
    }
    if (semaphore->count != 0U) { semaphore->count = 0U; return pdTRUE; }
    s_tick += wait;
    return pdFALSE;
}
void HAL_NVIC_SetPriority(IRQn_Type irq, uint32_t priority, uint32_t subpriority)
{ assert(irq == USART2_IRQn && priority == 5U && subpriority == 0U); }
void HAL_NVIC_EnableIRQ(IRQn_Type irq)
{
    if (irq == SPI2_IRQn) { s_spi_irq_enabled = 1U; }
    else if (irq == DMA1_Channel5_IRQn) { s_dma_irq_enabled = 1U; }
    else { assert(irq == USART2_IRQn); s_uart_irq_enabled = 1U; }
}
void HAL_NVIC_DisableIRQ(IRQn_Type irq)
{
    assert(irq != USART2_IRQn);
    if (irq == SPI2_IRQn) { s_spi_irq_enabled = 0U; }
    else { assert(irq == DMA1_Channel5_IRQn); s_dma_irq_enabled = 0U; }
}
void HAL_NVIC_ClearPendingIRQ(IRQn_Type irq)
{ assert(irq == SPI2_IRQn || irq == DMA1_Channel5_IRQn); }
void test_clear_dma_flags(DMA_HandleTypeDef *p_dma, uint32_t flags)
{
    assert(p_dma == &s_dma && flags == DMA_FLAG_GL5);
    assert((s_dma_registers.CCR & (DMA_CCR_EN | DMA_IT_TC | DMA_IT_HT | DMA_IT_TE)) == 0U);
}
void test_force_reset(void)
{
    assert(s_critical == 0U && s_uart_irq_enabled != 0U);
    assert((s_dma_registers.CCR & DMA_CCR_EN) == 0U);
    s_resets++;
    memset(&s_spi_registers, 0, sizeof(s_spi_registers));
}
void test_release_reset(void) { assert(s_resets != 0U); }
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *p_spi)
{
    assert(p_spi == &hspi2 && s_critical == 0U);
    p_spi->State = HAL_SPI_STATE_READY;
    p_spi->ErrorCode = HAL_SPI_ERROR_NONE;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Transmit_DMA(SPI_HandleTypeDef *p_spi, uint8_t *p_data, uint16_t size)
{
    assert(p_spi == &hspi2 && size != 0U && size <= sizeof(s_snapshot));
    assert(s_inflight == NULL && (s_dma_registers.CCR & DMA_CCR_EN) == 0U);
    assert(s_spi_irq_enabled != 0U && s_dma_irq_enabled != 0U);
    s_spi_starts++;
    if (s_start_status != HAL_OK) { return s_start_status; }
    s_inflight = p_data;
    s_inflight_size = size;
    memcpy(s_snapshot, p_data, size);
    s_dma_registers.CCR = DMA_CCR_EN | DMA_IT_TC | DMA_IT_TE;
    s_dma.State = HAL_DMA_STATE_BUSY;
    s_spi_registers.CR1 |= SPI_CR1_SPE;
    s_spi_registers.CR2 |= SPI_CR2_TXDMAEN | SPI_IT_ERR;
    p_spi->State = HAL_SPI_STATE_BUSY_TX;
    if (s_cs == 0U && s_dc == 0U && size == 1U && p_data[0] == 0x20U) {
        s_activation = 1U;
        s_busy_remaining = 3U;
    }
    if (s_mode == TX_EARLY) {
        if (s_critical != 0U) { s_early_pending = 1U; }
        else { complete_spi(); }
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *p_spi)
{
    uint8_t nested = 0x55U;
    assert(p_spi == &hspi2 && s_critical == 0U && s_primask == 0U);
    assert(s_spi_irq_enabled == 0U && s_dma_irq_enabled == 0U);
    check_buffer();
    s_spi_aborts++;
    receive_text("RECOVER\n");
    /* A preempting task cannot start a second transfer while recovery owns
       SPI2. Even an already queued callback cannot release the next wait. */
    assert(s_io.spi_write(s_io.handle, &nested, 1U, 50U) != 0);
    s_recovery_reentries++;
    s_ipsr = 1U;
    HAL_SPI_TxCpltCallback(&hspi2);
    HAL_SPI_ErrorCallback(&hspi2);
    s_ipsr = 0U;
    assert(s_spi_semaphore->count == 0U);
    /* A timeout abort is allowed to report failure. The adapter must still
       forcibly stop channel/request and reset just this SPI peripheral. */
    return HAL_ERROR;
}
HAL_StatusTypeDef HAL_DMA_Abort(DMA_HandleTypeDef *p_dma)
{
    assert(p_dma == &s_dma && s_critical == 0U);
    s_dma_aborts++;
    if (s_dma_abort_status == HAL_OK) { s_dma_registers.CCR &= ~DMA_CCR_EN; }
    else { assert((s_dma_registers.CCR & DMA_CCR_EN) == 0U); }
    p_dma->State = HAL_DMA_STATE_READY;
    s_inflight = NULL;
    return s_dma_abort_status;
}
void HAL_GPIO_WritePin(GPIO_TypeDef *p_port, uint16_t pin, GPIO_PinState state)
{
    assert(p_port == &g_test_gpio);
    if (pin == DC_Pin) { s_dc = (uint8_t)state; }
    if (pin == CS2_Pin) {
        if (state == GPIO_PIN_SET) {
            assert(s_inflight == NULL && (s_dma_registers.CCR & DMA_CCR_EN) == 0U);
        }
        s_cs = (uint8_t)state;
    }
}
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *p_port, uint16_t pin)
{
    assert(p_port == &g_test_gpio && pin == BUSY_Pin);
    if (s_activation == 0U) { return GPIO_PIN_RESET; }
    assert(s_inflight == NULL && s_cs != 0U);
    s_busy_reads++;
    if (s_observed_device != NULL) {
        assert(s_observed_device->full_refreshes == s_expected_refreshes);
        assert(s_observed_device->background_valid == 0U);
    }
    if (s_hold_busy != 0U) { return GPIO_PIN_SET; }
    if (s_busy_remaining != 0U) { s_busy_remaining--; return GPIO_PIN_SET; }
    s_activation = 0U;
    return GPIO_PIN_RESET;
}
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *p_uart, const uint8_t *p_data, uint16_t size)
{
    assert(p_uart == &huart2 && p_data != NULL && size != 0U);
    s_uart_tx_starts++;
    p_uart->gState = HAL_UART_STATE_BUSY_TX;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef *p_uart)
{ assert(p_uart == &huart2); p_uart->gState = HAL_UART_STATE_READY; return HAL_OK; }
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *p_uart, uint8_t *p_data, uint16_t size)
{
    assert(p_uart == &huart2 && p_data != NULL && size == 1U);
    s_rx_byte = p_data;
    p_uart->RxState = 1U;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *p_uart)
{ assert(p_uart == &huart2); s_uart_rx_aborts++; p_uart->RxState = 0U; return HAL_OK; }
void test_uart_clear(UART_HandleTypeDef *p_uart, uint32_t flags)
{ assert(p_uart == &huart2 && flags == 15U); }

static int32_t send_stack(tx_mode_t mode)
{
    uint8_t data[64];
    uint32_t i;
    int32_t result;
    for (i = 0U; i < sizeof(data); i++) { data[i] = (uint8_t)(i ^ 0xA5U); }
    s_mode = mode;
    result = s_io.spi_write(s_io.handle, data, sizeof(data), 50U);
    assert(s_inflight == NULL && (s_dma_registers.CCR & DMA_CCR_EN) == 0U);
    assert(s_critical == 0U && s_uart_irq_enabled != 0U);
    assert(s_spi_irq_enabled != 0U && s_dma_irq_enabled != 0U);
    memset(data, 0xFF, sizeof(data));
    return result;
}

static void test_arguments(void)
{
    uint8_t byte = 0U;
    SPI_HandleTypeDef foreign;
    memset(&foreign, 0, sizeof(foreign));
    assert(s_io.spi_write(NULL, &byte, 1U, 50U) != 0);
    assert(s_io.spi_write(s_io.handle, NULL, 1U, 50U) != 0);
    assert(s_io.spi_write(s_io.handle, &byte, 0U, 50U) != 0);
    assert(s_io.spi_write(s_io.handle, &byte, 65536U, 50U) != 0);
    assert(s_io.spi_write(s_io.handle, &byte, 1U, 0U) != 0);
    assert(s_io.spi_write(s_io.handle, &byte, 1U, HAL_MAX_DELAY) != 0);
    s_context.spi_handle = NULL;
    assert(s_io.spi_write(s_io.handle, &byte, 1U, 50U) != 0);
    s_context.spi_handle = &foreign;
    assert(s_io.spi_write(s_io.handle, &byte, 1U, 50U) != 0);
    s_context.spi_handle = &hspi2;
    hspi2.hdmatx = NULL;
    assert(s_io.spi_write(s_io.handle, &byte, 1U, 50U) != 0);
    hspi2.hdmatx = &s_dma;
    s_ipsr = 1U;
    assert(s_io.spi_write(s_io.handle, &byte, 1U, 50U) != 0);
    s_ipsr = 0U;
    s_primask = 1U;
    assert(s_io.spi_write(s_io.handle, &byte, 1U, 50U) != 0);
    assert(s_primask == 1U);
    s_primask = 0U;
    s_basepri = 0x50U;
    assert(s_io.spi_write(s_io.handle, &byte, 1U, 50U) != 0);
    assert(s_basepri == 0x50U);
    s_basepri = 0U;
    s_scheduler = taskSCHEDULER_NOT_STARTED;
    assert(s_io.spi_write(s_io.handle, &byte, 1U, 50U) != 0);
    s_scheduler = taskSCHEDULER_SUSPENDED;
    assert(s_io.spi_write(s_io.handle, &byte, 1U, 50U) != 0);
    s_scheduler = taskSCHEDULER_RUNNING;
    assert(s_spi_starts == 0U && s_creations == 0U);
    complete_spi(); /* An inactive callback cannot satisfy the first send. */
    s_fail_semaphore = 1U;
    assert(send_stack(TX_COMPLETE) != 0);
    assert(s_spi_starts == 0U && s_creations == 1U);
}

static void test_dma_and_rx(void)
{
    uint8_t byte = 0xAAU;
    uint32_t before;
    uint32_t tick;
    uint32_t notifications;
    assert(send_stack(TX_COMPLETE) == 0);
    assert(send_stack(TX_EARLY) == 0);
    assert(s_creations == 2U && s_resets == 0U);
    s_mode = TX_COMPLETE;
    assert(s_io.spi_write(s_io.handle, &byte, 1U, UINT32_MAX - 1U) == 0);
    assert(s_last_spi_wait == UINT32_MAX - 1U);
    assert(s_io.spi_write(s_io.handle, &byte, 1U, 1U) == 0);
    assert(s_last_spi_wait == 1U);
    receive_text("BUFFERED\n");
    before = s_spi_aborts;
    assert(send_stack(TX_TIMEOUT) != 0);
    assert(s_spi_aborts == before + 1U && s_resets != 0U);
    expect_rx("BUFFERED\nRECOVER\n");
    complete_spi(); /* Stale completion must not leak to the next timeout. */
    assert(send_stack(TX_TIMEOUT) != 0);
    expect_rx("RECOVER\n");
    assert(send_stack(TX_COMPLETE) == 0);
    s_start_status = HAL_BUSY;
    assert(send_stack(TX_COMPLETE) != 0);
    expect_rx("RECOVER\n");
    s_start_status = HAL_ERROR;
    assert(send_stack(TX_COMPLETE) != 0);
    expect_rx("RECOVER\n");
    s_start_status = HAL_OK;
    tick = s_tick;
    assert(send_stack(TX_DMA_ERROR) != 0);
    assert(s_tick == tick); /* Error wakes immediately instead of waiting 50ms. */
    expect_rx("RECOVER\n");
    assert(send_stack(TX_ERROR_LATE_COMPLETE) != 0);
    expect_rx("RECOVER\n");
    assert(send_stack(TX_SPI_ERROR) != 0);
    expect_rx("RECOVER\n");
    s_dma_abort_status = HAL_ERROR; /* Already-READY HAL still requires EN=0. */
    assert(send_stack(TX_DMA_ERROR) != 0);
    expect_rx("RECOVER\n");
    s_dma_abort_status = HAL_OK;
    assert(send_stack(TX_COMPLETE) == 0);
    assert(send_stack(TX_FOREIGN) != 0);
    expect_rx("RECOVER\n");
    notifications = s_notifications;
    assert(send_stack(TX_RX_ONLY) != 0);
    expect_rx("RX ONLY\nRECOVER\n");
    assert(s_notifications == notifications + 16U);
    assert(send_stack(TX_UART_CONCURRENT) == 0);
    expect_rx("DURING DMA\n");
    assert(s_uart_tx_starts == 1U && s_creations == 3U);
    before = s_spi_starts;
    assert(send_stack(TX_REENTER) == 0);
    assert(s_spi_starts == before + 1U);
    assert(s_uart_rx_aborts == 1U && huart2.RxState == 1U);
    assert(s_spi_aborts == s_resets && s_dma_aborts >= s_spi_aborts);
    assert(s_recovery_reentries == s_spi_aborts);
    puts("PASS: SPI DMA buffer lifetime, early/late completion, timeout/start/error/reentry; independent UART TX/RX");
}

static void test_busy_completion(void)
{
    static epd_gdey029t94_t device;
    static uint8_t image[EPD_GDEY029T94_FRAME_BUFFER_SIZE];
    static uint8_t gray[EPD_GDEY029T94_GRAY4_SIZE];
    uint32_t before;
    uint32_t tick;
    memset(&device, 0, sizeof(device));
    memset(image, 0xAA, sizeof(image));
    memset(gray, 0x66, sizeof(gray));
    device.io = s_io;
    device.initialized = 1U;
    device.profile = EPD_PROFILE_BW_296;
    s_mode = TX_COMPLETE;
    s_observed_device = &device;
    s_expected_refreshes = 0U;
    tick = s_tick;
    assert(epd_gdey029t94_full_refresh(&device, image, sizeof(image)) == EPD_GDEY029T94_OK);
    assert(s_busy_reads >= 4U && s_tick > tick);
    assert(device.full_refreshes == 1U && device.background_valid != 0U);
    assert(memcmp(device.background_image, image, sizeof(image)) == 0);

    /* All SPI transfers succeed while BUSY never releases. No image or stats
       commit may escape before the separate panel refresh completes. */
    device.background_valid = 0U;
    s_expected_refreshes = 1U;
    s_hold_busy = 1U;
    tick = s_tick;
    assert(epd_gdey029t94_full_refresh(&device, image, sizeof(image)) == EPD_GDEY029T94_ERROR_BUSY_TIMEOUT);
    assert(s_tick - tick >= 10000U && device.full_refreshes == 1U);
    assert(device.initialized == 0U && device.background_valid == 0U);
    s_hold_busy = 0U;
    s_activation = 0U;
    device.initialized = 1U;
    device.profile = EPD_PROFILE_GRAY4;
    before = s_checked_transfers;
    assert(epd_gdey029t94_gray4_refresh(&device, gray, sizeof(gray)) == EPD_GDEY029T94_OK);
    assert(device.full_refreshes == 2U && s_checked_transfers - before >= 148U);
    s_observed_device = NULL;
    puts("PASS: real BW/gray4 driver retains stack chunks; DMA completion still waits for BUSY and timeout blocks commit");
}

int main(void)
{
    hspi2.Instance = &s_spi_registers;
    hspi2.hdmatx = &s_dma;
    s_dma.Instance = &s_dma_registers;
    huart2.hdmatx = &s_uart_dma;
    assert(stm32_gdey029t94_make_io(&s_io, &s_context) == EPD_GDEY029T94_OK);
    assert(stm32_gdey029t94_prepare(&s_context) == EPD_GDEY029T94_OK);
    assert(stm32_debug_uart_rx_start(notify_rx));
    test_arguments();
    test_dma_and_rx();
    test_busy_completion();
    assert(s_critical == 0U && s_primask == 0U && s_basepri == 0U);
    assert(s_inflight == NULL && s_uart_rx_aborts == 1U);
    puts("PASS: EPD SPI DMA host suite");
    return 0;
}
