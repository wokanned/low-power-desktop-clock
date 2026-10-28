#include "stm32_debug_uart.h"
#include "usart.h"

#define DEBUG_UART_TIMEOUT_MS 15U
#define DEBUG_UART_RING_SIZE 1024U
#define DEBUG_UART_RING_MASK (DEBUG_UART_RING_SIZE - 1U)
#define DEBUG_UART_IRQ_PRIORITY 5U

static uint8_t s_rx_ring[DEBUG_UART_RING_SIZE];
static uint8_t s_rx_byte;
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;
static volatile uint32_t s_rx_errors;
static volatile uint8_t s_rx_discard;
static volatile uint8_t s_rx_restart;
static volatile uint8_t s_binary;
static stm32_debug_uart_notify_t s_notify;

bool stm32_debug_uart_send(void *p_handle, const uint8_t *p_data, uint16_t len)
{
    (void)p_handle;
    if (p_data == NULL || len == 0U) { return false; }
    /* Single UART owner: the debug task. No replay after a partial transfer. */
    return HAL_UART_Transmit(&huart2, (uint8_t *)p_data, len,
                             DEBUG_UART_TIMEOUT_MS) == HAL_OK;
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
    if (s_rx_restart == 0U) { return true; }
    irq_state = __get_PRIMASK();
    __disable_irq();
    /* Receive-only abort preserves an unrelated blocking transmit. */
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

/* The USART2 IRQ entry is generated in Core/Src/stm32l4xx_it.c. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *p_uart)
{
    if (p_uart != &huart2) { return; }
    /* HAL may invoke RxCplt before ErrorCallback for the same damaged byte. */
    if (p_uart->ErrorCode == HAL_UART_ERROR_NONE) {
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
    if (p_uart != &huart2) { return; }
    /* Save the error before HAL clears ErrorCode or any receive rearm. */
    s_rx_errors |= STM32_DEBUG_UART_RX_ERROR;
    s_rx_tail = s_rx_head;
    s_rx_discard = (uint8_t)(s_binary == 0U);
    s_rx_restart = 1U;
    if (s_notify != NULL) { s_notify(); }
}
