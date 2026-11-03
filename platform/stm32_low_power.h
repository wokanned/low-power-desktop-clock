#ifndef STM32_LOW_POWER_H
#define STM32_LOW_POWER_H

#include <stdint.h>

typedef enum {
    STM32_LOW_POWER_OK = 0,
    STM32_LOW_POWER_BAD_CLOCK,
    STM32_LOW_POWER_BAD_TIMER,
    STM32_LOW_POWER_COUNTER_UNSTABLE,
    STM32_LOW_POWER_COMPARE_TIMEOUT,
    STM32_LOW_POWER_HSE_TIMEOUT,
    STM32_LOW_POWER_PLL_TIMEOUT,
    STM32_LOW_POWER_SWITCH_TIMEOUT,
    STM32_LOW_POWER_SYSTICK_TIMEOUT
} stm32_low_power_error_t;

typedef struct {
    uint32_t ready;
    uint32_t aborted;
    uint32_t fatal;
    uint32_t last_error;
    uint32_t last_lse_counts;
    uint32_t last_step_ticks;
    uint32_t last_pended_ticks;
    uint32_t rcc_cr;
    uint32_t rcc_cfgr;
} stm32_low_power_diagnostics_t;

/* Readable through SWD even if restoration fails with interrupts masked. */
extern volatile stm32_low_power_diagnostics_t g_stm32_low_power_diagnostics;

/* After Cube peripheral init, before tasks. Does not alter RTC or display. */
void stm32_low_power_init(void);
/* USER hook in LPTIM1_IRQHandler; no FreeRTOS calls and no peripheral IO. */
void stm32_low_power_irq(void);

#endif
