#ifndef TEST_DEBUG_USART_H
#define TEST_DEBUG_USART_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t ErrorCode; } DMA_HandleTypeDef;
typedef struct {
    uint32_t ErrorCode;
    uint32_t gState;
    uint32_t RxState;
    DMA_HandleTypeDef *hdmatx;
} UART_HandleTypeDef;
typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;
#define USART2_IRQn 38U
#define HAL_UART_ERROR_NONE 0U
#define HAL_UART_ERROR_PE 1U
#define HAL_UART_ERROR_NE 2U
#define HAL_UART_ERROR_FE 4U
#define HAL_UART_ERROR_ORE 8U
#define HAL_UART_ERROR_DMA 16U
#define HAL_UART_ERROR_RTO 32U
#define HAL_UART_STATE_READY 0U
#define HAL_UART_STATE_BUSY_TX 1U
#define UART_CLEAR_PEF 1U
#define UART_CLEAR_FEF 2U
#define UART_CLEAR_NEF 4U
#define UART_CLEAR_OREF 8U
extern UART_HandleTypeDef huart2;
uint32_t __get_PRIMASK(void);
uint32_t __get_BASEPRI(void);
uint32_t __get_IPSR(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t state);
void HAL_NVIC_SetPriority(uint32_t irq, uint32_t priority, uint32_t subpriority);
void HAL_NVIC_EnableIRQ(uint32_t irq);
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *p_uart,
                                       const uint8_t *p_data, uint16_t length);
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef *p_uart);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *p_uart, uint8_t *p_data,
                                     uint16_t length);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *p_uart);
void HAL_UART_IRQHandler(UART_HandleTypeDef *p_uart);
void test_uart_clear(UART_HandleTypeDef *p_uart, uint32_t flags);
#define __HAL_UART_CLEAR_FLAG(uart, flags) test_uart_clear((uart), (flags))
#endif
