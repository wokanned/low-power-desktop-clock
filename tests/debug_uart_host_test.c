#include "stm32_debug_uart.h"
#include "usart.h"
#include "semphr.h"
#include "task.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

UART_HandleTypeDef huart2;
static DMA_HandleTypeDef s_tx_dma;
static uint8_t *s_receive;
static uint32_t s_primask;
static uint32_t s_basepri;
static uint32_t s_ipsr;
static uint32_t s_critical_depth;
static uint32_t s_critical_basepri;
static uint32_t s_pending_early_complete;
static uint32_t s_notifications;
static uint32_t s_abort_count;
static uint32_t s_fail_rearm;
static BaseType_t s_scheduler = taskSCHEDULER_RUNNING;
static uint32_t s_fail_semaphore;
static uint32_t s_semaphore_creations;
static uint32_t s_tx_starts;
static uint32_t s_tx_aborts;
static uint32_t s_dma_active;
static HAL_StatusTypeDef s_start_status = HAL_OK;
static const uint8_t *s_expected_tx;
static uint16_t s_expected_length;
typedef enum {
    TX_COMPLETE,
    TX_COMPLETE_EARLY,
    TX_TIMEOUT,
    TX_DMA_ERROR,
    TX_DMA_ERROR_LATE_COMPLETE,
    TX_RX_ERROR_COMPLETE,
    TX_MIXED_ERROR,
    TX_RX_DURING_WAIT,
    TX_RX_RECOVER_DURING_WAIT,
    TX_REENTER,
    TX_FOREIGN_CALLBACKS
} tx_mode_t;
static tx_mode_t s_tx_mode;
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *p_uart);
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *p_uart);
void HAL_UART_ErrorCallback(UART_HandleTypeDef *p_uart);
static void receive_byte(uint8_t byte);

static void tx_complete(void)
{
    s_dma_active = 0U;
    huart2.gState = HAL_UART_STATE_READY;
    s_ipsr = 1U;
    HAL_UART_TxCpltCallback(&huart2);
    s_ipsr = 0U;
}

static void uart_error(uint32_t errors)
{
    huart2.ErrorCode = errors;
    if ((errors & HAL_UART_ERROR_DMA) != 0U) {
        s_dma_active = 0U;
        huart2.gState = HAL_UART_STATE_READY;
    }
    if ((errors & ~HAL_UART_ERROR_DMA) != 0U) { huart2.RxState = 0U; }
    s_ipsr = 1U;
    HAL_UART_ErrorCallback(&huart2);
    s_ipsr = 0U;
}

void test_debug_uart_enter_critical(void)
{
    if (s_critical_depth == 0U) {
        s_critical_basepri = s_basepri;
        s_basepri = 0x50U;
    }
    s_critical_depth++;
}

void test_debug_uart_exit_critical(void)
{
    assert(s_critical_depth > 0U);
    s_critical_depth--;
    if (s_critical_depth == 0U) {
        s_basepri = s_critical_basepri;
        /* Pending TC runs once interrupts are restored, before the first wait. */
        if (s_pending_early_complete != 0U) {
            s_pending_early_complete = 0U;
            tx_complete();
        }
    }
}

uint32_t __get_PRIMASK(void) { return s_primask; }
uint32_t __get_BASEPRI(void) { return s_basepri; }
uint32_t __get_IPSR(void) { return s_ipsr; }
void __disable_irq(void) { s_primask = 1U; }
void __set_PRIMASK(uint32_t state) { s_primask = state; }
BaseType_t xTaskGetSchedulerState(void) { return s_scheduler; }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *p_control)
{
    assert(p_control != NULL);
    s_semaphore_creations++;
    if (s_fail_semaphore != 0U) { s_fail_semaphore = 0U; return NULL; }
    p_control->count = 0U;
    return p_control;
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t *p_woken)
{
    assert(semaphore != NULL && p_woken != NULL && s_ipsr != 0U);
    if (semaphore->count != 0U) { return pdFALSE; }
    semaphore->count = 1U;
    *p_woken = pdTRUE;
    return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait)
{
    UART_HandleTypeDef other_uart = {0};
    assert(semaphore != NULL);
    if (wait != 0U) {
        assert(wait == pdMS_TO_TICKS(15U));
        assert(s_primask == 0U && s_basepri == 0U && s_ipsr == 0U);
        assert(s_critical_depth == 0U);
    }
    if (wait != 0U && semaphore->count == 0U) {
        switch (s_tx_mode) {
        case TX_COMPLETE: tx_complete(); break;
        case TX_COMPLETE_EARLY: assert(0); break;
        case TX_TIMEOUT: break;
        case TX_DMA_ERROR: uart_error(HAL_UART_ERROR_DMA); break;
        case TX_DMA_ERROR_LATE_COMPLETE:
            uart_error(HAL_UART_ERROR_DMA);
            tx_complete();
            break;
        case TX_RX_ERROR_COMPLETE:
            uart_error(HAL_UART_ERROR_ORE);
            tx_complete();
            break;
        case TX_MIXED_ERROR:
            uart_error(HAL_UART_ERROR_DMA | HAL_UART_ERROR_NE);
            break;
        case TX_RX_DURING_WAIT:
            receive_byte('R');
            tx_complete();
            break;
        case TX_RX_RECOVER_DURING_WAIT:
            uart_error(HAL_UART_ERROR_FE);
            assert(stm32_debug_uart_rx_recover());
            assert(s_dma_active != 0U && huart2.gState == HAL_UART_STATE_BUSY_TX);
            tx_complete();
            break;
        case TX_REENTER:
            assert(!stm32_debug_uart_send(NULL, s_expected_tx, s_expected_length));
            tx_complete();
            break;
        case TX_FOREIGN_CALLBACKS:
            other_uart.ErrorCode = HAL_UART_ERROR_DMA | HAL_UART_ERROR_ORE;
            s_ipsr = 1U;
            HAL_UART_TxCpltCallback(&other_uart);
            HAL_UART_RxCpltCallback(&other_uart);
            HAL_UART_ErrorCallback(&other_uart);
            s_ipsr = 0U;
            break;
        }
    }
    if (semaphore->count == 0U) { return pdFALSE; }
    semaphore->count = 0U;
    return pdTRUE;
}
void HAL_NVIC_SetPriority(uint32_t irq, uint32_t priority, uint32_t subpriority)
{ assert(irq == USART2_IRQn && priority == 5U && subpriority == 0U); }
void HAL_NVIC_EnableIRQ(uint32_t irq) { assert(irq == USART2_IRQn); }
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *p_uart,
                                       const uint8_t *p_data, uint16_t length)
{
    assert(p_uart == &huart2 && p_data == s_expected_tx && length == s_expected_length);
    assert(p_data != NULL && length != 0U && s_dma_active == 0U);
    assert(s_critical_depth > 0U && s_basepri == 0x50U);
    s_tx_starts++;
    if (s_start_status != HAL_OK) { return s_start_status; }
    s_dma_active = 1U;
    p_uart->gState = HAL_UART_STATE_BUSY_TX;
    p_uart->ErrorCode = HAL_UART_ERROR_NONE;
    if (s_tx_mode == TX_COMPLETE_EARLY) { s_pending_early_complete = 1U; }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef *p_uart)
{
    assert(p_uart == &huart2);
    assert(s_critical_depth > 0U && s_basepri == 0x50U);
    s_tx_aborts++;
    s_dma_active = 0U;
    p_uart->gState = HAL_UART_STATE_READY;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *p_uart, uint8_t *p_data,
                                     uint16_t length)
{
    assert(p_uart == &huart2 && length == 1U);
    if (s_fail_rearm != 0U) { s_fail_rearm = 0U; return HAL_ERROR; }
    s_receive = p_data;
    p_uart->RxState = 1U;
    p_uart->ErrorCode = HAL_UART_ERROR_NONE;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *p_uart)
{ assert(p_uart == &huart2); p_uart->RxState = 0U; s_abort_count++; return HAL_OK; }
void test_uart_clear(UART_HandleTypeDef *p_uart, uint32_t flags)
{ assert(p_uart == &huart2 && flags == 15U); }
static void notify(void) { s_notifications++; }
static void receive_byte(uint8_t byte)
{
    assert(s_receive != NULL && huart2.RxState == 1U);
    *s_receive = byte;
    huart2.RxState = 0U;
    HAL_UART_RxCpltCallback(&huart2);
}
static void receive_text(const char *p_text)
{ while (*p_text != '\0') { receive_byte((uint8_t)*p_text++); } }
static void expect_text(const char *p_text)
{
    uint8_t byte;
    uint32_t errors;
    while (*p_text != '\0') {
        assert(stm32_debug_uart_receive(&byte, &errors));
        assert(errors == 0U && byte == (uint8_t)*p_text++);
    }
    assert(!stm32_debug_uart_receive(&byte, &errors) && errors == 0U);
}

static void test_tx_dma(void)
{
    static const uint8_t frame[68] = {0x12U, 0x34U};
    uint32_t before_starts;
    uint32_t before_aborts;
    uint32_t before_rx_aborts;
    uint32_t before_notifications;
    uint32_t errors;
    uint8_t byte;
    s_expected_tx = frame;
    s_expected_length = sizeof(frame);
    assert(!stm32_debug_uart_send(NULL, NULL, sizeof(frame)));
    assert(!stm32_debug_uart_send(NULL, frame, 0U));
    s_ipsr = 1U;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    s_ipsr = 0U;
    s_primask = 1U;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_primask == 1U);
    s_primask = 0U;
    s_basepri = 0x50U;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_basepri == 0x50U);
    s_basepri = 0U;
    s_scheduler = taskSCHEDULER_NOT_STARTED;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    s_scheduler = taskSCHEDULER_SUSPENDED;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    s_scheduler = taskSCHEDULER_RUNNING;
    huart2.hdmatx = NULL;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    huart2.hdmatx = &s_tx_dma;
    assert(s_tx_starts == 0U && s_semaphore_creations == 0U);
    /* An inactive callback must not release a future transfer's wait. */
    tx_complete();
    s_fail_semaphore = 1U;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_tx_starts == 0U);
    s_tx_mode = TX_COMPLETE;
    assert(stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_dma_active == 0U && s_tx_aborts == 0U);
    assert(s_semaphore_creations == 2U);
    s_tx_mode = TX_COMPLETE_EARLY;
    assert(stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_semaphore_creations == 2U);

    /* Timeout releases the caller's buffer only after TX has stopped, preserving RX. */
    s_tx_mode = TX_TIMEOUT;
    before_rx_aborts = s_abort_count;
    before_aborts = s_tx_aborts;
    receive_text("BUFFERED\n");
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_dma_active == 0U && s_tx_aborts == before_aborts + 1U);
    assert(s_abort_count == before_rx_aborts && huart2.RxState == 1U);
    expect_text("BUFFERED\n");
    tx_complete(); /* A late completion from the failed transfer is ignored. */
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_dma_active == 0U);
    s_tx_mode = TX_COMPLETE;
    assert(stm32_debug_uart_send(NULL, frame, sizeof(frame)));

    before_starts = s_tx_starts;
    s_start_status = HAL_BUSY;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_tx_starts == before_starts + 1U && s_dma_active == 0U);
    s_start_status = HAL_ERROR;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_tx_starts == before_starts + 2U && s_dma_active == 0U);
    s_start_status = HAL_OK;
    assert(stm32_debug_uart_send(NULL, frame, sizeof(frame)));

    before_rx_aborts = s_abort_count;
    before_notifications = s_notifications;
    receive_text("PRESERVED\n");
    s_tx_mode = TX_DMA_ERROR;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_dma_active == 0U && s_abort_count == before_rx_aborts);
    expect_text("PRESERVED\n");
    /* A lingering DMA ErrorCode cannot discard a valid RX byte or stop rearming. */
    receive_byte('Q');
    assert(stm32_debug_uart_receive(&byte, &errors) && byte == 'Q' && errors == 0U);
    assert(s_notifications == before_notifications + 11U);
    s_tx_mode = TX_DMA_ERROR_LATE_COMPLETE;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_dma_active == 0U);
    s_tx_mode = TX_COMPLETE;
    assert(stm32_debug_uart_send(NULL, frame, sizeof(frame)));

    s_tx_mode = TX_RX_ERROR_COMPLETE;
    before_aborts = s_tx_aborts;
    assert(stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_tx_aborts == before_aborts);
    assert(!stm32_debug_uart_receive(&byte, &errors));
    assert(errors == STM32_DEBUG_UART_RX_ERROR);
    assert(stm32_debug_uart_rx_recover());
    receive_text("DISCARD\nKEPT\n");
    expect_text("KEPT\n");

    s_tx_mode = TX_MIXED_ERROR;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_dma_active == 0U);
    assert(!stm32_debug_uart_receive(&byte, &errors));
    assert(errors == STM32_DEBUG_UART_RX_ERROR);
    assert(stm32_debug_uart_rx_recover());
    receive_text("DISCARD\n");
    expect_text("");

    s_tx_mode = TX_RX_DURING_WAIT;
    before_notifications = s_notifications;
    assert(stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_notifications == before_notifications + 1U);
    assert(stm32_debug_uart_receive(&byte, &errors) && byte == 'R' && errors == 0U);
    s_tx_mode = TX_RX_RECOVER_DURING_WAIT;
    assert(stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(!stm32_debug_uart_receive(&byte, &errors));
    assert(errors == STM32_DEBUG_UART_RX_ERROR);
    receive_text("DISCARD\n");
    expect_text("");
    s_tx_mode = TX_REENTER;
    before_starts = s_tx_starts;
    assert(stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_tx_starts == before_starts + 1U);
    s_tx_mode = TX_FOREIGN_CALLBACKS;
    assert(!stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(!stm32_debug_uart_receive(&byte, &errors) && errors == 0U);
    s_tx_mode = TX_COMPLETE;
    assert(stm32_debug_uart_send(NULL, frame, sizeof(frame)));
    assert(s_dma_active == 0U && s_semaphore_creations == 2U);
}
int main(void)
{
    uint32_t i;
    uint8_t byte;
    uint32_t errors;
    huart2.hdmatx = &s_tx_dma;
    assert(stm32_debug_uart_rx_start(notify));
    assert(s_primask == 0U && s_abort_count == 1U);
    /* Cross the uint16_t producer/consumer wrap while preserving byte order. */
    for (i = 0U; i < 70000U; i++) {
        receive_byte((uint8_t)i);
        assert(stm32_debug_uart_receive(&byte, &errors));
        assert(byte == (uint8_t)i && errors == 0U);
    }
    receive_text("CONSOLE ON\r\n");
    expect_text("CONSOLE ON\r\n");
    for (i = 0U; i < 1024U; i++) { receive_byte('A'); }
    for (i = 0U; i < 1024U; i++) {
        assert(stm32_debug_uart_receive(&byte, &errors) && errors == 0U && byte == 'A');
    }
    for (i = 0U; i < 1025U; i++) { receive_byte('B'); }
    assert(!stm32_debug_uart_receive(&byte, &errors));
    assert(errors == STM32_DEBUG_UART_RX_OVERFLOW);
    stm32_debug_uart_set_binary(0U); /* Polling unchanged mode preserves line discard. */
    receive_text("CONSOLE ON\nFORMAT 12\n");
    expect_text("FORMAT 12\n");
    receive_text("CON");
    huart2.ErrorCode = 2U;
    *s_receive = 'X';
    huart2.RxState = 0U;
    HAL_UART_RxCpltCallback(&huart2);
    assert(huart2.ErrorCode == 2U);
    HAL_UART_ErrorCallback(&huart2);
    assert(!stm32_debug_uart_receive(&byte, &errors));
    assert(errors == STM32_DEBUG_UART_RX_ERROR);
    huart2.gState = 123U;
    assert(stm32_debug_uart_rx_recover());
    assert(huart2.gState == 123U && s_abort_count == 2U);
    receive_text("SOLE ON\nERRORS\r\n");
    expect_text("ERRORS\r\n");
    s_fail_rearm = 1U;
    receive_byte('C');
    assert(!stm32_debug_uart_receive(&byte, &errors));
    assert(errors == STM32_DEBUG_UART_RX_ERROR);
    assert(stm32_debug_uart_rx_recover());
    receive_text("ONSOLE ON\r\nFORMAT 24\n");
    expect_text("\nFORMAT 24\n");
    s_primask = 1U;
    assert(!stm32_debug_uart_receive(&byte, &errors));
    assert(s_primask == 1U);
    s_primask = 0U;
    test_tx_dma();
    stm32_debug_uart_set_binary(1U);
    for (i = 0U; i < 1025U; i++) { receive_byte(0U); }
    assert(!stm32_debug_uart_receive(&byte, &errors));
    assert(errors == STM32_DEBUG_UART_RX_OVERFLOW);
    receive_byte('D');
    assert(stm32_debug_uart_receive(&byte, &errors) && byte == 'D' && errors == 0U);
    huart2.ErrorCode = 2U;
    HAL_UART_ErrorCallback(&huart2);
    assert(stm32_debug_uart_rx_recover());
    receive_byte('C');
    assert(stm32_debug_uart_receive(&byte, &errors) && byte == 'C');
    assert(errors == STM32_DEBUG_UART_RX_ERROR);
    assert(s_notifications > 500U);
    puts("PASS: TX DMA completion/timeout/error/context/reentry isolation; 1024-byte RX ring and recovery");
    return 0;
}
