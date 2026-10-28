#ifndef TEST_DEBUG_FREERTOS_H
#define TEST_DEBUG_FREERTOS_H
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef int32_t BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configTICK_RATE_HZ 1000U
#ifdef TEST_DEBUG_UART_CRITICAL
void test_debug_uart_enter_critical(void);
void test_debug_uart_exit_critical(void);
#define taskENTER_CRITICAL() test_debug_uart_enter_critical()
#define taskEXIT_CRITICAL() test_debug_uart_exit_critical()
#else
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#endif
#define portYIELD_FROM_ISR(value) ((void)(value))
#endif
