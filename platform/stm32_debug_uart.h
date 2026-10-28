#ifndef STM32_DEBUG_UART_H
#define STM32_DEBUG_UART_H
#include <stdbool.h>
#include <stdint.h>
#define STM32_DEBUG_UART_RX_ERROR 1UL
#define STM32_DEBUG_UART_RX_OVERFLOW 2UL
typedef void (*stm32_debug_uart_notify_t)(void);
bool stm32_debug_uart_send(void *p_handle, const uint8_t *p_data, uint16_t len);
/* Call from the debug task; the callback is invoked only in USART2 IRQ. */
bool stm32_debug_uart_rx_start(stm32_debug_uart_notify_t notify);
bool stm32_debug_uart_rx_recover(void);
/* Task context: binary framing resynchronizes on magic/CRC, not newline. */
void stm32_debug_uart_set_binary(uint8_t enabled);
/* Task-context, atomic event and byte retrieval. Errors invalidate partial lines. */
bool stm32_debug_uart_receive(uint8_t *p_byte, uint32_t *p_errors);
#endif
