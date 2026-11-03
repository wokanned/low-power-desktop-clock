#ifndef STM32_DEBUG_UART_H
#define STM32_DEBUG_UART_H
#include <stdbool.h>
#include <stdint.h>
#define STM32_DEBUG_UART_RX_ERROR 1UL
#define STM32_DEBUG_UART_RX_OVERFLOW 2UL
typedef void (*stm32_debug_uart_notify_t)(void);
/* debugTask only, with scheduler/interrupts enabled. Waits for DMA + UART TC
   (15 ms timeout); failure aborts TX before return, without replay or RX abort.
   The caller retains the buffer and may reuse it only after this call returns. */
bool stm32_debug_uart_send(void *p_handle, const uint8_t *p_data, uint16_t len);
/* Call from the debug task; the callback is invoked only in USART2 IRQ. */
bool stm32_debug_uart_rx_start(stm32_debug_uart_notify_t notify);
bool stm32_debug_uart_rx_recover(void);
/* Task context: binary framing resynchronizes on magic/CRC, not newline. */
void stm32_debug_uart_set_binary(uint8_t enabled);
/* Task-context, atomic event and byte retrieval. Errors invalidate partial lines. */
bool stm32_debug_uart_receive(uint8_t *p_byte, uint32_t *p_errors);
/* Idle port only: caller keeps PRIMASK set until clocks and RX are restored.
   Suspend rechecks RX/TX; a false result leaves the active receiver unchanged.
   Bytes sent while suspended are intentionally discarded, never joined to a
   previous command. The debug owner must first discard/timed-out partial lines. */
bool stm32_debug_uart_sleep_ready(void);
bool stm32_debug_uart_suspend_rx(void);
void stm32_debug_uart_resume_rx(void);
#endif
