#include "stm32_buttons.h"
#include "main.h"

#define BUTTON_IRQ_PRIORITY 5U
/* Historical board mapping: active high, external pull-down. Verify on target. */
#define BUTTON_1_PIN GPIO_PIN_10
#define BUTTON_2_PIN     GPIO_PIN_15
#define BUTTON_3_PIN     GPIO_PIN_12
#define BUTTON_4_PIN    GPIO_PIN_11
#define BUTTON_EXTI_MASK (BUTTON_1_PIN | BUTTON_2_PIN | \
                          BUTTON_3_PIN | BUTTON_4_PIN)
typedef struct {
    GPIO_TypeDef *p_port;
    uint16_t pin;
    uint32_t key;
} button_pin_t;
static const button_pin_t s_pins[] = {
    {GPIOC, BUTTON_1_PIN, STM32_BUTTON_1},
    {GPIOA, BUTTON_2_PIN, STM32_BUTTON_2},
    {GPIOC, BUTTON_3_PIN, STM32_BUTTON_3},
    {GPIOC, BUTTON_4_PIN, STM32_BUTTON_4}
};
static stm32_buttons_notify_t s_notify;
static volatile uint32_t s_pending;
static volatile uint8_t s_initialized;

bool stm32_buttons_init(stm32_buttons_notify_t notify_from_isr)
{
    if (notify_from_isr == NULL) { return false; }
    HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
    s_notify = notify_from_isr;
    s_pending = stm32_buttons_read_pressed_mask();
    s_initialized = 1U;
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, BUTTON_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
    return true;
}

uint32_t stm32_buttons_read_pressed_mask(void)
{
    uint32_t mask = 0U;
    uint32_t i;
    for (i = 0U; i < sizeof(s_pins) / sizeof(s_pins[0]); i++) {
        if (HAL_GPIO_ReadPin(s_pins[i].p_port, s_pins[i].pin) == GPIO_PIN_SET) {
            mask |= s_pins[i].key;
        }
    }
    return mask;
}

uint32_t stm32_buttons_take_pending(void)
{
    uint32_t primask = __get_PRIMASK();
    uint32_t pending;
    __disable_irq();
    pending = s_pending;
    s_pending = 0U;
    __set_PRIMASK(primask);
    return pending;
}

bool stm32_buttons_sleep_ready(void)
{
    return s_initialized != 0U && s_pending == 0U &&
        (EXTI->PR1 & BUTTON_EXTI_MASK) == 0U &&
        stm32_buttons_read_pressed_mask() == 0U;
}

void stm32_buttons_irq(void)
{
    uint32_t i;
    for (i = 0U; i < sizeof(s_pins) / sizeof(s_pins[0]); i++) {
        HAL_GPIO_EXTI_IRQHandler(s_pins[i].pin);
    }
}

void stm32_buttons_exti_callback(uint16_t pin)
{
    uint32_t i;
    for (i = 0U; i < sizeof(s_pins) / sizeof(s_pins[0]); i++) {
        if (s_pins[i].pin == pin) {
            s_pending |= s_pins[i].key;
            if (s_notify != NULL) { s_notify(); }
            return;
        }
    }
}
