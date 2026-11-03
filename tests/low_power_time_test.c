#include "low_power_time.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void)
{
    uint32_t rem = 0U;
    uint32_t i;
    uint32_t sum = 0U;
    low_power_time_result_t r;
    for (i = 0U; i < 32768U; i++) { sum += low_power_time_cycles(1U, &rem); }
    assert(sum == LOW_POWER_CORE_HZ && rem == 0U);
    assert(low_power_time_deadline(10000U, 80000U) == 32768U);
    assert(low_power_time_deadline(1U, 40000U) == 16U);
    r = low_power_time_advance(79999U, 1U);
    assert(r.ticks == 1U && r.phase_cycles == 0U);
    r = low_power_time_advance(40000U, 240000U);
    assert(r.ticks == 3U && r.phase_cycles == 40000U);
    assert(low_power_time_step(5U, 10U) == 5U);
    assert(low_power_time_step(10U, 10U) == 9U);
    assert(low_power_time_step(12U, 10U) == 9U);
    assert(low_power_time_step(1U, 0U) == 0U);
    puts("PASS: fractional accumulation, capped deadline, phase, early and due tick boundaries");
    return 0;
}
