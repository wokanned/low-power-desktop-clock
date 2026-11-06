#ifndef TEST_BUTTON_MAIN_H
#define TEST_BUTTON_MAIN_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t IDR; } GPIO_TypeDef;
typedef struct { uint32_t PR1; } test_exti_t;
extern GPIO_TypeDef g_test_gpioa, g_test_gpioc;
extern test_exti_t g_test_exti;
#define GPIOA (&g_test_gpioa)
#define GPIOC (&g_test_gpioc)
#define EXTI (&g_test_exti)
#define GPIO_PIN_8 (1U << 8)
#define GPIO_PIN_10 (1U << 10)
#define GPIO_PIN_11 (1U << 11)
#define GPIO_PIN_12 (1U << 12)
#define GPIO_PIN_15 (1U << 15)
#define GPIO_PIN_SET 1U
#define EXTI15_10_IRQn 40U
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t value);
uint32_t HAL_GPIO_ReadPin(GPIO_TypeDef *p_port, uint16_t pin);
void HAL_NVIC_DisableIRQ(uint32_t irq);
void HAL_NVIC_SetPriority(uint32_t irq, uint32_t priority, uint32_t subpriority);
void HAL_NVIC_EnableIRQ(uint32_t irq);
void HAL_GPIO_EXTI_IRQHandler(uint16_t pin);
void HAL_GPIO_EXTI_Callback(uint16_t pin);
#endif
