#include "stm32_debug_uart.h"
#include "usart.h"

#define DEBUG_UART_TIMEOUT_MS 15U

bool stm32_debug_uart_send(void *p_handle, const uint8_t *p_data, uint16_t len)
{
    (void)p_handle;
    /* Single UART owner: the debug task. No replay after a partial transfer. */
    return HAL_UART_Transmit(&huart2, (uint8_t *)p_data, len,
                             DEBUG_UART_TIMEOUT_MS) == HAL_OK;
}
