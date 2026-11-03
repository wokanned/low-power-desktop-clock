#include "low_power_time.h"

uint32_t low_power_time_cycles(uint32_t lse_counts, uint32_t *p_remainder)
{
    uint64_t scaled = (uint64_t)lse_counts * LOW_POWER_CORE_HZ + *p_remainder;
    *p_remainder = (uint32_t)(scaled % LOW_POWER_LSE_HZ);
    return (uint32_t)(scaled / LOW_POWER_LSE_HZ);
}

low_power_time_result_t low_power_time_advance(uint32_t phase_cycles,
                                             uint32_t elapsed_cycles)
{
    low_power_time_result_t result;
    uint32_t total = phase_cycles + elapsed_cycles;
    result.ticks = total / LOW_POWER_CYCLES_PER_TICK;
    result.phase_cycles = total % LOW_POWER_CYCLES_PER_TICK;
    return result;
}

uint32_t low_power_time_deadline(uint32_t expected_ticks,
                                uint32_t remaining_cycles)
{
    uint64_t cycles;
    if (expected_ticks == 0U || remaining_cycles == 0U ||
        remaining_cycles > LOW_POWER_CYCLES_PER_TICK) { return 0U; }
    if (expected_ticks > LOW_POWER_MAX_SLEEP_TICKS) {
        expected_ticks = LOW_POWER_MAX_SLEEP_TICKS;
    }
    cycles = (uint64_t)(expected_ticks - 1U) * LOW_POWER_CYCLES_PER_TICK +
             remaining_cycles;
    /* Wake on or before the requested boundary; never round past it. */
    return (uint32_t)(cycles * LOW_POWER_LSE_HZ / LOW_POWER_CORE_HZ);
}

uint32_t low_power_time_step(uint32_t elapsed_ticks, uint32_t expected_ticks)
{
    if (expected_ticks == 0U) { return 0U; }
    return elapsed_ticks < expected_ticks ? elapsed_ticks : expected_ticks - 1U;
}
