#ifndef STM32_DEBUG_UART_H
#define STM32_DEBUG_UART_H
#include <stdbool.h>
#include <stdint.h>
bool stm32_debug_uart_send(void *p_handle, const uint8_t *p_data, uint16_t len);
#endif
