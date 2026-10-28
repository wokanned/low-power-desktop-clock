#ifndef TEST_EPD_MAIN_H
#define TEST_EPD_MAIN_H
#include <stddef.h>
#include <stdint.h>
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t state; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;
typedef struct { uint32_t CCR; } DMA_Channel_TypeDef;
typedef struct { uint32_t CR1, CR2, SR; } SPI_TypeDef;
typedef struct {
    DMA_Channel_TypeDef *Instance;
    uint32_t ErrorCode;
    uint32_t State;
} DMA_HandleTypeDef;
typedef struct {
    SPI_TypeDef *Instance;
    DMA_HandleTypeDef *hdmatx;
    uint32_t State;
    uint32_t ErrorCode;
    uint32_t TxXferCount;
    uint32_t RxXferCount;
} SPI_HandleTypeDef;
typedef struct {
    uint32_t ErrorCode;
    uint32_t gState;
    uint32_t RxState;
    DMA_HandleTypeDef *hdmatx;
} UART_HandleTypeDef;
typedef uint32_t IRQn_Type;
extern GPIO_TypeDef g_test_gpio;
#define CS2_GPIO_Port (&g_test_gpio)
#define DC_GPIO_Port (&g_test_gpio)
#define RES_GPIO_Port (&g_test_gpio)
#define BUSY_GPIO_Port (&g_test_gpio)
#define CS2_Pin 1U
#define DC_Pin 2U
#define RES_Pin 4U
#define BUSY_Pin 8U
#define HAL_MAX_DELAY UINT32_MAX
#define DMA1_Channel5_IRQn 15U
#define SPI2_IRQn 36U
#define USART2_IRQn 38U
#define SPI_CR1_SPE 1U
#define SPI_CR2_TXDMAEN 1U
#define SPI_CR2_RXDMAEN 2U
#define SPI_CR2_ERRIE 4U
#define SPI_CR2_TXEIE 8U
#define SPI_CR2_RXNEIE 16U
#define SPI_IT_ERR SPI_CR2_ERRIE
#define SPI_IT_TXE SPI_CR2_TXEIE
#define SPI_IT_RXNE SPI_CR2_RXNEIE
#define DMA_CCR_EN 1U
#define DMA_IT_TC 2U
#define DMA_IT_HT 4U
#define DMA_IT_TE 8U
#define DMA_FLAG_GL5 0x10000U
#define HAL_SPI_STATE_READY 0U
#define HAL_SPI_STATE_BUSY_TX 1U
#define HAL_SPI_ERROR_NONE 0U
#define HAL_SPI_ERROR_DMA 1U
#define HAL_SPI_ERROR_MODF 2U
#define HAL_SPI_ERROR_OVR 4U
#define HAL_DMA_STATE_READY 0U
#define HAL_DMA_STATE_BUSY 1U
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
#define CLEAR_BIT(reg, mask) ((reg) &= ~(uint32_t)(mask))
#define SET_BIT(reg, mask) ((reg) |= (uint32_t)(mask))
#define READ_BIT(reg, mask) ((reg) & (uint32_t)(mask))
#define __HAL_SPI_DISABLE(p_spi) CLEAR_BIT((p_spi)->Instance->CR1, SPI_CR1_SPE)
#define __HAL_SPI_DISABLE_IT(p_spi, mask) CLEAR_BIT((p_spi)->Instance->CR2, (mask))
#define __HAL_DMA_DISABLE(p_dma) CLEAR_BIT((p_dma)->Instance->CCR, DMA_CCR_EN)
#define __HAL_DMA_DISABLE_IT(p_dma, mask) CLEAR_BIT((p_dma)->Instance->CCR, (mask))
#define __HAL_DMA_GET_GI_FLAG_INDEX(p_dma) ((void)(p_dma), DMA_FLAG_GL5)
void test_clear_dma_flags(DMA_HandleTypeDef *p_dma, uint32_t flags);
#define __HAL_DMA_CLEAR_FLAG(p_dma, flags) test_clear_dma_flags((p_dma), (flags))
void test_force_reset(void);
void test_release_reset(void);
#define __HAL_RCC_SPI2_FORCE_RESET() test_force_reset()
#define __HAL_RCC_SPI2_RELEASE_RESET() test_release_reset()
void test_uart_clear(UART_HandleTypeDef *p_uart, uint32_t flags);
#define __HAL_UART_CLEAR_FLAG(p_uart, flags) test_uart_clear((p_uart), (flags))
uint32_t __get_PRIMASK(void);
uint32_t __get_BASEPRI(void);
uint32_t __get_IPSR(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t state);
void HAL_NVIC_SetPriority(IRQn_Type irq, uint32_t priority, uint32_t subpriority);
void HAL_NVIC_EnableIRQ(IRQn_Type irq);
void HAL_NVIC_DisableIRQ(IRQn_Type irq);
void HAL_NVIC_ClearPendingIRQ(IRQn_Type irq);
void HAL_GPIO_WritePin(GPIO_TypeDef *p_port, uint16_t pin, GPIO_PinState state);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *p_port, uint16_t pin);
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t delay);
HAL_StatusTypeDef HAL_SPI_Transmit_DMA(SPI_HandleTypeDef *p_spi, uint8_t *p_data, uint16_t size);
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *p_spi);
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *p_spi);
HAL_StatusTypeDef HAL_DMA_Abort(DMA_HandleTypeDef *p_dma);
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *p_uart, const uint8_t *p_data, uint16_t size);
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef *p_uart);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *p_uart, uint8_t *p_data, uint16_t size);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *p_uart);
#endif
