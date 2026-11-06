/* Real input task, debounce, GPIO adapter and shared EXTI dispatch. Fake only
 * the RTOS, GPIO registers and display/power consumers. No physical IO. */
#include "buttons.h"
#include "display.h"
#include "power.h"
#include "stm32_buttons.h"
#include "main.h"
#include "task.h"
#include "cmsis_os.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

GPIO_TypeDef g_test_gpioa, g_test_gpioc;
test_exti_t g_test_exti;
static uint32_t s_tick, s_primask, s_blockers, s_notifications, s_motion;
static uint32_t s_action_count, s_waits, s_race_injected;
static uint32_t s_irq_enabled;
static osThreadFunc_t s_run;
static jmp_buf s_finished;
static const display_action_t s_expected[] = {DISPLAY_ACTION_NEXT,
    DISPLAY_ACTION_PREVIOUS, DISPLAY_ACTION_NEXT, DISPLAY_ACTION_HOME,
    DISPLAY_ACTION_TOGGLE_CYCLE, DISPLAY_ACTION_NEXT};

uint32_t __get_PRIMASK(void) { return s_primask; }
void __disable_irq(void) { s_primask = 1U; }
void __set_PRIMASK(uint32_t value) { s_primask = value; }
uint32_t HAL_GPIO_ReadPin(GPIO_TypeDef *p_port, uint16_t pin)
{ return (p_port->IDR & pin) != 0U; }
void HAL_NVIC_DisableIRQ(uint32_t irq) { assert(irq == EXTI15_10_IRQn); s_irq_enabled = 0U; }
void HAL_NVIC_SetPriority(uint32_t irq, uint32_t priority, uint32_t subpriority)
{ assert(irq == EXTI15_10_IRQn && priority == 5U && subpriority == 0U); }
void HAL_NVIC_EnableIRQ(uint32_t irq) { assert(irq == EXTI15_10_IRQn); s_irq_enabled = 1U; }
void HAL_GPIO_EXTI_IRQHandler(uint16_t pin)
{
    if ((EXTI->PR1 & pin) != 0U) {
        EXTI->PR1 &= ~(uint32_t)pin;
        HAL_GPIO_EXTI_Callback(pin);
    }
}
void stm32_adxl345_motion_exti_callback(uint16_t pin)
{ if (pin == GPIO_PIN_8) { s_motion++; } }

static void s_raw(uint32_t mask)
{
    uint32_t old = stm32_buttons_read_pressed_mask();
    GPIOC->IDR = ((mask & STM32_BUTTON_1) != 0U ? GPIO_PIN_10 : 0U) |
        ((mask & STM32_BUTTON_3) != 0U ? GPIO_PIN_12 : 0U) |
        ((mask & STM32_BUTTON_4) != 0U ? GPIO_PIN_11 : 0U);
    GPIOA->IDR = (mask & STM32_BUTTON_2) != 0U ? GPIO_PIN_15 : 0U;
    mask &= ~old;
    EXTI->PR1 |= ((mask & STM32_BUTTON_1) != 0U ? GPIO_PIN_10 : 0U) |
        ((mask & STM32_BUTTON_2) != 0U ? GPIO_PIN_15 : 0U) |
        ((mask & STM32_BUTTON_3) != 0U ? GPIO_PIN_12 : 0U) |
        ((mask & STM32_BUTTON_4) != 0U ? GPIO_PIN_11 : 0U);
    if (s_irq_enabled != 0U) { stm32_buttons_irq(); }
}

TickType_t xTaskGetTickCount(void) { return s_tick; }
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *p_woken)
{ assert(task != NULL); s_notifications++; *p_woken = pdTRUE; }
bool display_handle_action(display_action_t action)
{
    assert((s_blockers & POWER_BLOCK_INPUT) != 0U && s_primask == 0U);
    assert(s_action_count < sizeof(s_expected) / sizeof(s_expected[0]));
    assert(action == s_expected[s_action_count++]);
    return true;
}
void power_set_blocker(uint32_t mask, bool active)
{
    assert(mask == POWER_BLOCK_INPUT && s_primask == 0U);
    if (active) { s_blockers |= mask; }
    else {
        assert(stm32_buttons_sleep_ready());
        s_blockers &= ~mask;
        /* Edge AFTER readiness check but BEFORE task blocks. The notification
         * must survive and the platform must veto sleep until it is consumed. */
        if (s_action_count == 5U && s_race_injected == 0U) {
            s_race_injected = 1U;
            s_raw(STM32_BUTTON_2);
            assert(!stm32_buttons_sleep_ready());
        }
    }
}
osThreadId_t osThreadNew(osThreadFunc_t function, void *p_argument,
                        const osThreadAttr_t *p_attributes)
{
    assert(p_argument == NULL && p_attributes->stack_size == 1024U);
    assert(p_attributes->cb_mem != NULL && p_attributes->stack_mem != NULL);
    s_run = function;
    return (osThreadId_t)1;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    assert(clear == pdTRUE && wait == portMAX_DELAY);
    assert((s_blockers & POWER_BLOCK_INPUT) == 0U);
    s_waits++;
    if (s_notifications != 0U) { s_notifications = 0U; return 1U; }
    if (s_action_count == 0U) { s_tick = 100U; s_raw(STM32_BUTTON_2); }
    else if (s_action_count == 1U) { s_tick = 300U; s_raw(STM32_BUTTON_1 | STM32_BUTTON_2); }
    else if (s_action_count == 3U) { s_tick = 400U; s_raw(STM32_BUTTON_3); }
    else if (s_action_count == 4U) { s_tick = 500U; s_raw(STM32_BUTTON_4); }
    else {
        assert(s_action_count == 6U && s_race_injected != 0U);
        assert(stm32_buttons_sleep_ready());
        longjmp(s_finished, 1);
    }
    s_notifications = 0U;
    return 1U;
}
int32_t osDelay(uint32_t ticks)
{
    assert(ticks == 10U && (s_blockers & POWER_BLOCK_INPUT) != 0U);
    s_tick += ticks;
    if (s_tick >= 200U && s_tick < 300U) { s_raw(0U); }
    if (s_tick >= 350U && s_tick < 400U) { s_raw(0U); }
    if (s_tick >= 450U && s_tick < 500U) { s_raw(0U); }
    if (s_tick >= 550U && s_tick < 580U) { s_raw(0U); }
    if (s_tick >= 630U) { s_raw(0U); }
    assert(s_tick < 1000U);
    return 0;
}

int main(void)
{
    assert(!stm32_buttons_init(NULL));
    assert(!stm32_buttons_sleep_ready());
    assert(buttons_start()); assert(buttons_start());
    HAL_GPIO_EXTI_Callback(GPIO_PIN_8);
    assert(s_motion == 1U && stm32_buttons_take_pending() == 0U);
    /* Read-only sleep query preserves hardware pending and IRQ masking. */
    EXTI->PR1 = GPIO_PIN_10;
    __disable_irq(); assert(!stm32_buttons_sleep_ready());
    assert(EXTI->PR1 == GPIO_PIN_10 && s_primask == 1U); __set_PRIMASK(0U);
    stm32_buttons_irq();
    assert(stm32_buttons_take_pending() == STM32_BUTTON_1);
    assert(stm32_buttons_sleep_ready()); s_notifications = 0U;
    if (setjmp(s_finished) == 0) { s_run(NULL); }
    assert(s_waits >= 6U && s_action_count == 6U && s_motion == 1U);
    puts("PASS: real GPIO/filter/input task, four mappings, held key, multi-key order, ISR dispatch, wake-first operation, sleep-release race and idle blocking");
    return 0;
}
