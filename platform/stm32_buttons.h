#ifndef STM32_BUTTONS_H
#define STM32_BUTTONS_H
#include <stdbool.h>
#include <stdint.h>

/* Logical order only: GPIO/SW mapping belongs exclusively to this adapter. */
#define STM32_BUTTON_1 (1UL << 0)
#define STM32_BUTTON_2     (1UL << 1)
#define STM32_BUTTON_3     (1UL << 2)
#define STM32_BUTTON_4    (1UL << 3)
typedef void (*stm32_buttons_notify_t)(void);

/* After GPIO init. Callback is ISR-only and must be installed before enabling. */
bool stm32_buttons_init(stm32_buttons_notify_t notify_from_isr);
uint32_t stm32_buttons_read_pressed_mask(void);
/* Task context; consume software records without clearing new hardware edges. */
uint32_t stm32_buttons_take_pending(void);
/* Read-only, bounded; safe with IRQs masked in the final STOP2 checks. */
bool stm32_buttons_sleep_ready(void);
/* Core vector forwards here; HAL callback is owned by stm32_gpio_exti. */
void stm32_buttons_irq(void);
void stm32_buttons_exti_callback(uint16_t pin);
#endif
