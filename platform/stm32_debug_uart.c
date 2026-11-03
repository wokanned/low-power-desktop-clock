#include "stm32_debug_uart.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "power.h"

#define DEBUG_UART_TIMEOUT_MS 15U
#define DEBUG_UART_RING_SIZE 1024U
#define DEBUG_UART_RING_MASK (DEBUG_UART_RING_SIZE - 1U)
#define DEBUG_UART_IRQ_PRIORITY 5U
#define DEBUG_UART_RX_ERROR_MASK (HAL_UART_ERROR_PE | HAL_UART_ERROR_NE | \
                                  HAL_UART_ERROR_FE | HAL_UART_ERROR_ORE | \
                                  HAL_UART_ERROR_RTO)

static StaticSemaphore_t s_tx_semaphore_control;
static SemaphoreHandle_t s_tx_semaphore;
static volatile uint8_t s_tx_active;
static volatile uint8_t s_tx_complete;
static volatile uint8_t s_tx_failed;

static uint8_t s_rx_ring[DEBUG_UART_RING_SIZE];
static uint8_t s_rx_byte;
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;
static volatile uint32_t s_rx_errors;
static volatile uint8_t s_rx_discard;
static volatile uint8_t s_rx_restart;
static volatile uint8_t s_binary;
static stm32_debug_uart_notify_t s_notify;
static volatile uint8_t s_rx_started;
static volatile uint8_t s_rx_suspended;

bool stm32_debug_uart_send(void *p_handle, const uint8_t *p_data, uint16_t len)
{
    HAL_StatusTypeDef result;
    BaseType_t completed;
    bool success;
    (void)p_handle;
    if (p_data == NULL || len == 0U || huart2.hdmatx == NULL ||
        __get_IPSR() != 0U || __get_PRIMASK() != 0U || __get_BASEPRI() != 0U ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        s_rx_suspended != 0U) { return false; }

    /* Single owner: debugTask. Retain the caller's buffer until UART TC or abort.
       A separate semaphore leaves RX/queue task notifications untouched. */
    taskENTER_CRITICAL();
    if (s_tx_active != 0U) {
        taskEXIT_CRITICAL();
        return false;
    }
    if (s_tx_semaphore == NULL) {
        s_tx_semaphore = xSemaphoreCreateBinaryStatic(&s_tx_semaphore_control);
    }
    if (s_tx_semaphore == NULL) {
        taskEXIT_CRITICAL();
        return false;
    }
    (void)xSemaphoreTake(s_tx_semaphore, 0U);
    s_tx_complete = 0U;
    s_tx_failed = 0U;
    s_tx_active = 1U;
    power_set_blocker(POWER_BLOCK_UART, true);
    result = HAL_UART_Transmit_DMA(&huart2, p_data, len);
    taskEXIT_CRITICAL();

    completed = pdFALSE;
    if (result == HAL_OK) {
        completed = xSemaphoreTake(s_tx_semaphore,
                                   pdMS_TO_TICKS(DEBUG_UART_TIMEOUT_MS));
    }
    taskENTER_CRITICAL();
    success = completed == pdTRUE && s_tx_complete != 0U && s_tx_failed == 0U;
    if (!success) {
        /* TX-only abort stops DMA before the caller can reuse its buffer.
           This L4 HAL abort has no polling wait. Mask both UART/DMA IRQs so
           DMA completion cannot re-enable TCIE midway through the abort. */
        (void)HAL_UART_AbortTransmit(&huart2);
    }
    s_tx_active = 0U;
    power_set_blocker(POWER_BLOCK_UART, false);
    taskEXIT_CRITICAL();
    return success;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *p_uart)
{
    BaseType_t higher_priority = pdFALSE;
    if (p_uart != &huart2 || s_tx_active == 0U) { return; }
    /* Normal DMA completion only enables TC IRQ; this callback is wire-complete. */
    s_tx_complete = 1U;
    (void)xSemaphoreGiveFromISR(s_tx_semaphore, &higher_priority);
    portYIELD_FROM_ISR(higher_priority);
}

bool stm32_debug_uart_rx_start(stm32_debug_uart_notify_t notify)
{
    s_notify = notify;
    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_rx_errors = 0U;
    s_rx_discard = 0U;
    s_rx_restart = 1U;
    s_binary = 0U;
    s_rx_started = 1U;
    s_rx_suspended = 0U;
    HAL_NVIC_SetPriority(USART2_IRQn, DEBUG_UART_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
    return stm32_debug_uart_rx_recover();
}

void stm32_debug_uart_set_binary(uint8_t enabled)
{
    uint32_t irq_state = __get_PRIMASK();
    __disable_irq();
    if (s_binary != (enabled != 0U ? 1U : 0U)) {
        s_binary = enabled != 0U ? 1U : 0U;
        s_rx_discard = 0U;
    }
    __set_PRIMASK(irq_state);
}

bool stm32_debug_uart_rx_recover(void)
{
    uint32_t irq_state;
    HAL_StatusTypeDef result;
    if (s_rx_suspended != 0U) { return false; }
    if (s_rx_restart == 0U) { return true; }
    irq_state = __get_PRIMASK();
    __disable_irq();
    /* Receive-only abort preserves an unrelated DMA transmit. */
    (void)HAL_UART_AbortReceive(&huart2);
    __HAL_UART_CLEAR_FLAG(&huart2, UART_CLEAR_PEF | UART_CLEAR_FEF |
                         UART_CLEAR_NEF | UART_CLEAR_OREF);
    result = HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1U);
    if (result == HAL_OK) { s_rx_restart = 0U; }
    __set_PRIMASK(irq_state);
    return result == HAL_OK;
}

bool stm32_debug_uart_receive(uint8_t *p_byte, uint32_t *p_errors)
{
    uint32_t irq_state;
    bool available;
    if (p_byte == NULL || p_errors == NULL) { return false; }
    irq_state = __get_PRIMASK();
    __disable_irq();
    *p_errors = s_rx_errors;
    s_rx_errors = 0U;
    available = s_rx_tail != s_rx_head;
    if (available) {
        *p_byte = s_rx_ring[s_rx_tail & DEBUG_UART_RING_MASK];
        s_rx_tail++;
    }
    __set_PRIMASK(irq_state);
    return available;
}

bool stm32_debug_uart_sleep_ready(void)
{
    return s_rx_started != 0U && s_rx_suspended == 0U && s_tx_active == 0U &&
        s_rx_restart == 0U && s_rx_head == s_rx_tail && s_rx_errors == 0U &&
        s_binary == 0U && huart2.gState == HAL_UART_STATE_READY &&
        huart2.RxState == HAL_UART_STATE_BUSY_RX &&
        __HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC) != 0U &&
        __HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE) == 0U &&
        __HAL_UART_GET_FLAG(&huart2, UART_FLAG_BUSY) == 0U &&
        __HAL_UART_GET_FLAG(&huart2, UART_FLAG_PE) == 0U &&
        __HAL_UART_GET_FLAG(&huart2, UART_FLAG_FE) == 0U &&
        __HAL_UART_GET_FLAG(&huart2, UART_FLAG_NE) == 0U &&
        __HAL_UART_GET_FLAG(&huart2, UART_FLAG_ORE) == 0U;
}

bool stm32_debug_uart_suspend_rx(void)
{
    if (__get_PRIMASK() == 0U || !stm32_debug_uart_sleep_ready()) { return false; }
    /* RX uses no DMA. AbortReceive is bounded and leaves TX untouched. */
    if (HAL_UART_AbortReceive(&huart2) != HAL_OK) {
        s_rx_restart = 1U;
        return false;
    }
    __HAL_UART_DISABLE(&huart2);
    s_rx_suspended = 1U;
    return true;
}

void stm32_debug_uart_resume_rx(void)
{
    if (s_rx_suspended == 0U) { return; }
    __HAL_UART_CLEAR_FLAG(&huart2, UART_CLEAR_PEF | UART_CLEAR_FEF |
                         UART_CLEAR_NEF | UART_CLEAR_OREF);
    __HAL_UART_SEND_REQ(&huart2, UART_RXDATA_FLUSH_REQUEST);
    s_rx_discard = 0U;
    s_rx_suspended = 0U;
    s_rx_restart = 1U;
    __HAL_UART_ENABLE(&huart2);
    if (HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1U) == HAL_OK) {
        s_rx_restart = 0U;
    }
    /* A failed rearm keeps sleep_ready false until the debug owner recovers. */
}

/* The USART2 IRQ entry is generated in Core/Src/stm32l4xx_it.c. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *p_uart)
{
    if (p_uart != &huart2 || s_rx_suspended != 0U) { return; }
    /* HAL may invoke RxCplt before ErrorCallback for the same damaged byte. */
    if ((p_uart->ErrorCode & DEBUG_UART_RX_ERROR_MASK) == 0U) {
        if (s_rx_discard != 0U) {
            if (s_rx_byte == '\n' || s_rx_byte == '\r') { s_rx_discard = 0U; }
        } else if ((uint16_t)(s_rx_head - s_rx_tail) >= DEBUG_UART_RING_SIZE) {
            s_rx_errors |= STM32_DEBUG_UART_RX_OVERFLOW;
            s_rx_tail = s_rx_head;
            s_rx_discard = (uint8_t)(s_binary == 0U && s_rx_byte != '\n' && s_rx_byte != '\r');
        } else {
            s_rx_ring[s_rx_head & DEBUG_UART_RING_MASK] = s_rx_byte;
            s_rx_head++;
        }
        if (HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1U) != HAL_OK) {
            s_rx_errors |= STM32_DEBUG_UART_RX_ERROR;
            s_rx_tail = s_rx_head;
            s_rx_discard = (uint8_t)(s_binary == 0U && s_rx_byte != '\n' && s_rx_byte != '\r');
            s_rx_restart = 1U;
        }
    }
    if (s_notify != NULL) { s_notify(); }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *p_uart)
{
    BaseType_t higher_priority = pdFALSE;
    uint32_t errors;
    if (p_uart != &huart2 || s_rx_suspended != 0U) { return; }
    errors = p_uart->ErrorCode;
    /* USART2 RX is interrupt-driven, so a DMA error belongs to TX only. */
    if ((errors & HAL_UART_ERROR_DMA) != 0U && s_tx_active != 0U) {
        s_tx_failed = 1U;
        (void)xSemaphoreGiveFromISR(s_tx_semaphore, &higher_priority);
        portYIELD_FROM_ISR(higher_priority);
    }
    if ((errors & DEBUG_UART_RX_ERROR_MASK) == 0U) { return; }
    /* Save the error before HAL clears ErrorCode or any receive rearm. */
    s_rx_errors |= STM32_DEBUG_UART_RX_ERROR;
    s_rx_tail = s_rx_head;
    s_rx_discard = (uint8_t)(s_binary == 0U);
    s_rx_restart = 1U;
    if (s_notify != NULL) { s_notify(); }
}
