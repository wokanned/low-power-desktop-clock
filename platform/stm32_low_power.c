#include "stm32_low_power.h"
#include "low_power_time.h"
#include "power.h"
#include "stm32_debug_uart.h"
#include "stm32_buttons.h"
#include "main.h"
#include "lptim.h"
#include "FreeRTOS.h"
#include "task.h"

#define LP_MIN_IDLE_TICKS 10U
#define LP_COUNTER_MASK 0xFFFFUL
#define LP_COUNTER_READ_ATTEMPTS 32U
#define LP_WAIT_LOOP_LIMIT 1600000UL
#define LP_COMPARE_WAIT_COUNTS 66U
#define LP_RESTORE_WAIT_COUNTS 656U
#define LP_WAKE_MARGIN_COUNTS 66U
#define LP_COMPARE_GUARD_COUNTS 4U
#define LP_TIMER_IRQ_FLAGS (LPTIM_ISR_CMPM | LPTIM_ISR_ARRM)
#define LP_SYSTICK_FLAGS (SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk | \
                         SysTick_CTRL_CLKSOURCE_Msk)
#define LP_PENDING_FLAGS (SCB_ICSR_ISRPENDING_Msk | SCB_ICSR_PENDSTSET_Msk | \
                         SCB_ICSR_PENDSVSET_Msk)
#define LP_CLOCK_DIVIDERS (RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2)

typedef char low_power_requires_1ms_tick[(configTICK_RATE_HZ == 1000U) ? 1 : -1];

volatile stm32_low_power_diagnostics_t g_stm32_low_power_diagnostics;
static uint32_t s_pll_configuration;
static uint32_t s_lse_fraction;
static uint32_t s_hal_submicro_cycles;

static void s_error(stm32_low_power_error_t error, uint32_t fault)
{
    g_stm32_low_power_diagnostics.last_error = (uint32_t)error;
    g_stm32_low_power_diagnostics.rcc_cr = RCC->CR;
    g_stm32_low_power_diagnostics.rcc_cfgr = RCC->CFGR;
    g_stm32_low_power_diagnostics.ready = 0U;
    power_set_fault(fault, true);
}

/* A failed PLL restore must not release tasks/IRQs at an incorrect bus rate.
 * Keep the fault in SRAM for SWD; recovery requires a deliberate reset. */
static void s_fatal(stm32_low_power_error_t error, uint32_t fault)
{
    __disable_irq();
    s_error(error, fault);
    g_stm32_low_power_diagnostics.fatal = 1U;
    for (;;) { __NOP(); }
}

static uint8_t s_counter(uint32_t *p_value)
{
    uint32_t previous = LPTIM1->CNT;
    uint32_t current;
    uint32_t attempt;
    /* CNT crosses clock domains; a single APB read is not a stable sample. */
    for (attempt = 0U; attempt < LP_COUNTER_READ_ATTEMPTS; attempt++) {
        current = LPTIM1->CNT;
        if (previous == current) { *p_value = current & LP_COUNTER_MASK; return 1U; }
        previous = current;
    }
    return 0U;
}

static uint8_t s_wait(volatile uint32_t *p_register, uint32_t mask,
                      uint32_t expected, uint32_t max_counts)
{
    uint32_t start;
    uint32_t now;
    uint32_t loops;
    if (s_counter(&start) == 0U) { return 0U; }
    /* The loop cap remains finite even when the LSE itself has failed. */
    for (loops = 0U; loops < LP_WAIT_LOOP_LIMIT; loops++) {
        if ((*p_register & mask) == expected) { return 1U; }
        if (s_counter(&now) == 0U || ((now - start) & LP_COUNTER_MASK) >= max_counts) {
            return 0U;
        }
    }
    return 0U;
}

static uint8_t s_clock_valid(void)
{
    return (uint8_t)(SystemCoreClock == LOW_POWER_CORE_HZ &&
        HAL_RCC_GetSysClockFreq() == LOW_POWER_CORE_HZ &&
        (RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_PLL &&
        (RCC->CFGR & LP_CLOCK_DIVIDERS) == 0U &&
        (RCC->PLLCFGR & RCC_PLLCFGR_PLLSRC) == RCC_PLLCFGR_PLLSRC_HSE &&
        (RCC->PLLCFGR & RCC_PLLCFGR_PLLREN) != 0U &&
        (PWR->CR1 & PWR_CR1_VOS) == PWR_REGULATOR_VOLTAGE_SCALE1 &&
        (FLASH->ACR & FLASH_ACR_LATENCY) == FLASH_LATENCY_4);
}

void stm32_low_power_init(void)
{
    uint32_t first;
    uint32_t now;
    uint32_t loops;
    NVIC_DisableIRQ(LPTIM1_IRQn);
    if (s_clock_valid() == 0U) {
        s_error(STM32_LOW_POWER_BAD_CLOCK, POWER_FAULT_CLOCK); return;
    }
    if (hlptim1.Instance != LPTIM1 ||
        __HAL_RCC_GET_LPTIM1_SOURCE() != RCC_LPTIM1CLKSOURCE_LSE ||
        (RCC->BDCR & RCC_BDCR_LSERDY) == 0U ||
        (LPTIM1->CFGR & (LPTIM_CFGR_PRESC | LPTIM_CFGR_CKSEL |
                         LPTIM_CFGR_COUNTMODE)) != 0U ||
        (LPTIM1->CR & LPTIM_CR_ENABLE) != 0U) {
        s_error(STM32_LOW_POWER_BAD_TIMER, POWER_FAULT_TIMER); return;
    }
    s_pll_configuration = RCC->PLLCFGR;
    /* IER is writable only while disabled. Keep both sources armed and gate
     * the NVIC while awake, rather than disabling/restarting the LPTIM. */
    LPTIM1->IER = LPTIM_IER_CMPMIE | LPTIM_IER_ARRMIE;
    __HAL_LPTIM_LPTIM1_EXTI_ENABLE_IT();
    SET_BIT(RCC->APB1SMENR1, RCC_APB1SMENR1_LPTIM1SMEN);
    if (HAL_LPTIM_Counter_Start(&hlptim1, LP_COUNTER_MASK) != HAL_OK ||
        s_counter(&first) == 0U) {
        s_error(STM32_LOW_POWER_BAD_TIMER, POWER_FAULT_TIMER); return;
    }
    for (loops = 0U; loops < LP_WAIT_LOOP_LIMIT; loops++) {
        if (s_counter(&now) == 0U) { break; }
        if (now != first) {
            SET_BIT(RCC->CFGR, RCC_CFGR_STOPWUCK); /* HSI16 on STOP exit. */
            SET_BIT(CoreDebug->DEMCR, CoreDebug_DEMCR_TRCENA_Msk);
            SET_BIT(DWT->CTRL, DWT_CTRL_CYCCNTENA_Msk);
            g_stm32_low_power_diagnostics.ready = 1U;
            return;
        }
    }
    s_error(STM32_LOW_POWER_BAD_TIMER, POWER_FAULT_TIMER);
}

void stm32_low_power_irq(void)
{
    NVIC_DisableIRQ(LPTIM1_IRQn);
    LPTIM1->ICR = LP_TIMER_IRQ_FLAGS;
}

static uint8_t s_motion_clear(void)
{
    return (uint8_t)((ACCINT2A8_GPIO_Port->IDR & ACCINT2A8_Pin) == 0U &&
        (EXTI->PR1 & ACCINT2A8_Pin) == 0U &&
        (EXTI->IMR1 & ACCINT2A8_Pin) != 0U);
}

static void s_restore_clock(void)
{
    if (RCC->PLLCFGR != s_pll_configuration ||
        (RCC->CFGR & LP_CLOCK_DIVIDERS) != 0U ||
        (PWR->CR1 & PWR_CR1_VOS) != PWR_REGULATOR_VOLTAGE_SCALE1) {
        s_fatal(STM32_LOW_POWER_BAD_CLOCK, POWER_FAULT_CLOCK);
    }
    MODIFY_REG(FLASH->ACR, FLASH_ACR_LATENCY, FLASH_LATENCY_4);
    if (!s_wait(&FLASH->ACR, FLASH_ACR_LATENCY, FLASH_LATENCY_4, LP_COMPARE_WAIT_COUNTS) ||
        !s_wait(&PWR->SR2, PWR_SR2_VOSF, 0U, LP_RESTORE_WAIT_COUNTS)) {
        s_fatal(STM32_LOW_POWER_BAD_CLOCK, POWER_FAULT_CLOCK);
    }
    SET_BIT(RCC->CR, RCC_CR_HSEON);
    if (!s_wait(&RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY, LP_RESTORE_WAIT_COUNTS)) {
        s_fatal(STM32_LOW_POWER_HSE_TIMEOUT, POWER_FAULT_CLOCK);
    }
    SET_BIT(RCC->CR, RCC_CR_PLLON);
    if (!s_wait(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY, LP_RESTORE_WAIT_COUNTS)) {
        s_fatal(STM32_LOW_POWER_PLL_TIMEOUT, POWER_FAULT_CLOCK);
    }
    MODIFY_REG(RCC->CFGR, RCC_CFGR_SW, RCC_CFGR_SW_PLL);
    if (!s_wait(&RCC->CFGR, RCC_CFGR_SWS, RCC_CFGR_SWS_PLL, LP_RESTORE_WAIT_COUNTS)) {
        s_fatal(STM32_LOW_POWER_SWITCH_TIMEOUT, POWER_FAULT_CLOCK);
    }
    SystemCoreClockUpdate();
    if (SystemCoreClock != LOW_POWER_CORE_HZ) {
        s_fatal(STM32_LOW_POWER_BAD_CLOCK, POWER_FAULT_CLOCK);
    }
}

static uint8_t s_program_compare(uint32_t start, uint32_t counts)
{
    uint32_t compare;
    uint32_t now;
    if (counts <= LP_WAKE_MARGIN_COUNTS + LP_COMPARE_GUARD_COUNTS) { return 0U; }
    counts -= LP_WAKE_MARGIN_COUNTS;
    /* A compare value below CNT can match immediately on this LPTIM. Wake
     * just before wrap and let the next idle cycle cover the remaining time. */
    compare = start + counts;
    if (compare >= LP_COUNTER_MASK) { compare = LP_COUNTER_MASK - 1U; }
    if (compare <= start + LP_COMPARE_GUARD_COUNTS) { return 0U; }
    LPTIM1->ICR = LPTIM_ICR_CMPOKCF | LP_TIMER_IRQ_FLAGS;
    NVIC_ClearPendingIRQ(LPTIM1_IRQn);
    LPTIM1->CMP = compare;
    if (!s_wait(&LPTIM1->ISR, LPTIM_ISR_CMPOK, LPTIM_ISR_CMPOK,
                LP_COMPARE_WAIT_COUNTS)) {
        s_error(STM32_LOW_POWER_COMPARE_TIMEOUT, POWER_FAULT_TIMER); return 0U;
    }
    if (s_counter(&now) == 0U) {
        s_error(STM32_LOW_POWER_COUNTER_UNSTABLE, POWER_FAULT_TIMER); return 0U;
    }
    if (now < start || now + LP_COMPARE_GUARD_COUNTS >= compare ||
        (LPTIM1->ISR & LP_TIMER_IRQ_FLAGS) != 0U) { return 0U; }
    NVIC_EnableIRQ(LPTIM1_IRQn);
    return 1U;
}

void vPortSuppressTicksAndSleep(TickType_t expected_idle)
{
    uint32_t primask;
    uint32_t sys_control;
    uint32_t tim_control;
    uint32_t tim_interrupts;
    uint32_t remaining;
    uint32_t hal_phase;
    uint32_t start;
    uint32_t end;
    uint32_t elapsed;
    uint32_t cycles;
    uint32_t end_cpu;
    uint32_t pending_sys;
    uint32_t pending_hal;
    uint32_t step;
    uint32_t ticks;
    uint32_t wake = 0U;
    uint32_t attempt;
    uint8_t slept = 0U;
    low_power_time_result_t sys_time;
    low_power_time_result_t hal_time;
    low_power_time_result_t correction;

    if (expected_idle < LP_MIN_IDLE_TICKS || g_stm32_low_power_diagnostics.ready == 0U ||
        !power_can_sleep((uint32_t)xTaskGetTickCount())) { return; }
    primask = __get_PRIMASK();
    __disable_irq();
    __DSB();
    __ISB();
    if (eTaskConfirmSleepModeStatus() == eAbortSleep ||
        !power_can_sleep((uint32_t)xTaskGetTickCount()) || !s_motion_clear() ||
        (SCB->ICSR & LP_PENDING_FLAGS) != 0U ||
        !stm32_debug_uart_sleep_ready() ||
        !stm32_buttons_sleep_ready()) { goto abort_unmodified; }
    sys_control = SysTick->CTRL;
    tim_control = TIM17->CR1;
    tim_interrupts = TIM17->DIER;
    if ((sys_control & LP_SYSTICK_FLAGS) != LP_SYSTICK_FLAGS ||
        SysTick->LOAD != LOW_POWER_CYCLES_PER_TICK - 1U ||
        TIM17->PSC != LOW_POWER_CYCLES_PER_US - 1U || TIM17->ARR != 999U ||
        (tim_control & TIM_CR1_CEN) == 0U || (tim_interrupts & TIM_DIER_UIE) == 0U ||
        RCC->PLLCFGR != s_pll_configuration || s_clock_valid() == 0U) {
        s_error(STM32_LOW_POWER_BAD_CLOCK, POWER_FAULT_CLOCK); goto abort_unmodified;
    }
    if (!stm32_debug_uart_suspend_rx()) { goto abort_unmodified; }
    if (s_counter(&start) == 0U) {
        s_error(STM32_LOW_POWER_COUNTER_UNSTABLE, POWER_FAULT_TIMER);
        stm32_debug_uart_resume_rx(); goto abort_unmodified;
    }
    SysTick->CTRL = sys_control & ~SysTick_CTRL_ENABLE_Msk;
    TIM17->CR1 = tim_control & ~TIM_CR1_CEN;
    TIM17->DIER = tim_interrupts & ~TIM_DIER_UIE;
    __DSB();
    remaining = SysTick->VAL;
    remaining = remaining == 0U ? LOW_POWER_CYCLES_PER_TICK : remaining + 1U;
    hal_phase = TIM17->CNT * LOW_POWER_CYCLES_PER_US + s_hal_submicro_cycles;
    pending_sys = (SCB->ICSR & SCB_ICSR_PENDSTSET_Msk) != 0U ? 1U : 0U;
    pending_hal = (TIM17->SR & TIM_SR_UIF) != 0U ? 1U : 0U;
    /* Own pending ticks are consumed into the compensation below, never lost.
     * Do not clear shared NVIC pending bits or any external wake source. */
    if (pending_sys != 0U) { SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk; }
    if (pending_hal != 0U) { TIM17->SR = ~TIM_SR_UIF; }
    if (pending_sys == 0U && pending_hal == 0U &&
        s_program_compare(start, low_power_time_deadline(expected_idle, remaining)) != 0U &&
        eTaskConfirmSleepModeStatus() != eAbortSleep && s_motion_clear() &&
        power_can_sleep((uint32_t)xTaskGetTickCount()) &&
        stm32_buttons_sleep_ready() &&
        (SCB->ICSR & LP_PENDING_FLAGS) == 0U) {
        __DSB();
        HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);
        __ISB();
        slept = 1U;
        if ((LPTIM1->ISR & LP_TIMER_IRQ_FLAGS) != 0U) { wake |= POWER_WAKE_TIMER; }
        if ((EXTI->PR1 & ACCINT2A8_Pin) != 0U ||
            (ACCINT2A8_GPIO_Port->IDR & ACCINT2A8_Pin) != 0U) { wake |= POWER_WAKE_MOTION; }
        if (wake == 0U) { wake = POWER_WAKE_OTHER; }
        s_restore_clock();
    }
    NVIC_DisableIRQ(LPTIM1_IRQn);
    LPTIM1->ICR = LP_TIMER_IRQ_FLAGS;
    NVIC_ClearPendingIRQ(LPTIM1_IRQn);
    if (s_counter(&end) == 0U) {
        s_fatal(STM32_LOW_POWER_COUNTER_UNSTABLE, POWER_FAULT_TIMER);
    }
    end_cpu = DWT->CYCCNT;
    elapsed = (end - start) & LP_COUNTER_MASK;
    cycles = low_power_time_cycles(elapsed, &s_lse_fraction);
    sys_time = low_power_time_advance(LOW_POWER_CYCLES_PER_TICK - remaining, cycles);
    hal_time = low_power_time_advance(hal_phase, cycles);
    /* Include the 64-bit conversion work after the final LSE sample. Once
     * timers run again, their pending interrupts cover subsequent bookkeeping. */
    cycles = DWT->CYCCNT - end_cpu;
    correction = low_power_time_advance(sys_time.phase_cycles, cycles);
    sys_time.ticks += correction.ticks + pending_sys;
    sys_time.phase_cycles = correction.phase_cycles;
    correction = low_power_time_advance(hal_time.phase_cycles, cycles);
    hal_time.ticks += correction.ticks + pending_hal;
    hal_time.phase_cycles = correction.phase_cycles;
    uwTick += hal_time.ticks;
    s_hal_submicro_cycles = hal_time.phase_cycles % LOW_POWER_CYCLES_PER_US;
    TIM17->CNT = hal_time.phase_cycles / LOW_POWER_CYCLES_PER_US;
    TIM17->DIER = tim_interrupts;
    TIM17->CR1 = tim_control;
    remaining = LOW_POWER_CYCLES_PER_TICK - sys_time.phase_cycles;
    SysTick->LOAD = remaining - 1U;
    SysTick->VAL = 0U;
    SysTick->CTRL = sys_control;
    /* Let the short first reload latch before restoring the normal reload. */
    for (attempt = 0U; attempt < LP_COUNTER_READ_ATTEMPTS; attempt++) {
        if (SysTick->VAL != 0U || (SCB->ICSR & SCB_ICSR_PENDSTSET_Msk) != 0U) { break; }
    }
    if (attempt == LP_COUNTER_READ_ATTEMPTS) {
        s_fatal(STM32_LOW_POWER_SYSTICK_TIMEOUT, POWER_FAULT_TIMER);
    }
    SysTick->LOAD = LOW_POWER_CYCLES_PER_TICK - 1U;
    step = low_power_time_step(sys_time.ticks, expected_idle);
    if (step != 0U) { vTaskStepTick((TickType_t)step); }
    ticks = sys_time.ticks - step;
    for (attempt = 0U; attempt < ticks; attempt++) {
        (void)xTaskIncrementTick(); /* Scheduler remains suspended: pend only. */
    }
    g_stm32_low_power_diagnostics.last_lse_counts = elapsed;
    g_stm32_low_power_diagnostics.last_step_ticks = step;
    g_stm32_low_power_diagnostics.last_pended_ticks = ticks;
    stm32_debug_uart_resume_rx();
    if (slept != 0U) { power_record_sleep(elapsed, wake); }
    else { g_stm32_low_power_diagnostics.aborted++; }
    __set_PRIMASK(primask);
    return;

abort_unmodified:
    g_stm32_low_power_diagnostics.aborted++;
    __set_PRIMASK(primask);
}
