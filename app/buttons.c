#include "buttons.h"
#include "display.h"
#include "power.h"
#include "button_filter.h"
#include "stm32_buttons.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

#define BUTTON_TASK_STACK_BYTES 1024U
#define BUTTON_SAMPLE_MS 10U
#define BUTTON_MS_PER_SECOND 1000U
static StackType_t s_stack[BUTTON_TASK_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_task_control;
static osThreadId_t s_task;
static button_filter_t s_filter;

static void s_notify_from_isr(void)
{
    BaseType_t higher_priority_woken = pdFALSE;
    if (s_task != NULL) {
        vTaskNotifyGiveFromISR((TaskHandle_t)s_task, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

static void s_dispatch(uint32_t pressed)
{
    if ((pressed & STM32_BUTTON_1) != 0U) {
        (void)display_handle_action(DISPLAY_ACTION_PREVIOUS);
    }
    if ((pressed & STM32_BUTTON_2) != 0U) {
        (void)display_handle_action(DISPLAY_ACTION_NEXT);
    }
    if ((pressed & STM32_BUTTON_3) != 0U) {
        (void)display_handle_action(DISPLAY_ACTION_HOME);
    }
    if ((pressed & STM32_BUTTON_4) != 0U) {
        (void)display_handle_action(DISPLAY_ACTION_TOGGLE_CYCLE);
    }
}

static void s_run(void *p_argument)
{
    button_filter_result_t result;
    uint32_t now_ms;
    (void)p_argument;
    button_filter_init(&s_filter);
    for (;;) {
        power_set_blocker(POWER_BLOCK_INPUT, true);
        (void)stm32_buttons_take_pending();
        now_ms = (uint32_t)((uint64_t)xTaskGetTickCount() *
                            BUTTON_MS_PER_SECOND / configTICK_RATE_HZ);
        result = button_filter_update(&s_filter,
                    stm32_buttons_read_pressed_mask(), now_ms);
        s_dispatch(result.pressed_mask);
        if (result.needs_sampling) {
            (void)osDelay(pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
        } else if (stm32_buttons_sleep_ready()) {
            /* An edge racing this release stays latched in the adapter and
             * task notification. STOP2 checks the same latch with IRQs masked. */
            power_set_blocker(POWER_BLOCK_INPUT, false);
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            (void)osDelay(pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
        }
    }
}

bool buttons_start(void)
{
    const osThreadAttr_t attributes = {
        .name = "buttonsTask",
        .cb_mem = &s_task_control,
        .cb_size = sizeof(s_task_control),
        .stack_mem = s_stack,
        .stack_size = sizeof(s_stack),
        .priority = osPriorityAboveNormal
    };
    if (s_task != NULL) { return true; }
    power_set_blocker(POWER_BLOCK_INPUT, true);
    s_task = osThreadNew(s_run, NULL, &attributes);
    if (s_task == NULL) { return false; }
    return stm32_buttons_init(s_notify_from_isr);
}
