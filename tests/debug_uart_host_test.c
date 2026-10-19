#include "stm32_debug_uart.h"
#include "usart.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

UART_HandleTypeDef huart2;
static uint8_t *s_receive;
static uint32_t s_primask;
static uint32_t s_notifications;
static uint32_t s_abort_count;
static uint32_t s_fail_rearm;
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *p_uart);
void HAL_UART_ErrorCallback(UART_HandleTypeDef *p_uart);

uint32_t __get_PRIMASK(void) { return s_primask; }
void __disable_irq(void) { s_primask = 1U; }
void __set_PRIMASK(uint32_t state) { s_primask = state; }
void HAL_NVIC_SetPriority(uint32_t irq, uint32_t priority, uint32_t subpriority)
{ assert(irq == USART2_IRQn && priority == 5U && subpriority == 0U); }
void HAL_NVIC_EnableIRQ(uint32_t irq) { assert(irq == USART2_IRQn); }
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *p_uart, uint8_t *p_data,
                                   uint16_t length, uint32_t timeout)
{
    assert(p_uart == &huart2 && p_data != NULL && length != 0U && timeout == 15U);
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
int main(void)
{
    uint32_t i;
    uint8_t byte;
    uint32_t errors;
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
    for (i = 0U; i < 256U; i++) { receive_byte('A'); }
    for (i = 0U; i < 256U; i++) {
        assert(stm32_debug_uart_receive(&byte, &errors) && errors == 0U && byte == 'A');
    }
    for (i = 0U; i < 257U; i++) { receive_byte('B'); }
    assert(!stm32_debug_uart_receive(&byte, &errors));
    assert(errors == STM32_DEBUG_UART_RX_OVERFLOW);
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
    assert(!stm32_debug_uart_send(NULL, NULL, 1U));
    assert(stm32_debug_uart_send(NULL, (const uint8_t *)"OK", 2U));
    assert(s_notifications > 500U);
    puts("PASS: 256-byte RX ring, overflow isolation, HAL error capture and receive-only recovery");
    return 0;
}
