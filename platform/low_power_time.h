#ifndef LOW_POWER_TIME_H
#define LOW_POWER_TIME_H

#include <stdint.h>

#define LOW_POWER_LSE_HZ 32768UL
#define LOW_POWER_CORE_HZ 80000000UL
#define LOW_POWER_TICK_HZ 1000UL
#define LOW_POWER_CYCLES_PER_TICK (LOW_POWER_CORE_HZ / LOW_POWER_TICK_HZ)
#define LOW_POWER_CYCLES_PER_US (LOW_POWER_CORE_HZ / 1000000UL)
#define LOW_POWER_MAX_SLEEP_TICKS 1000UL

typedef struct {
    uint32_t ticks;
    uint32_t phase_cycles;
} low_power_time_result_t;

/* Convert only stopped-clock time. Carry the division residue across sleeps. */
uint32_t low_power_time_cycles(uint32_t lse_counts, uint32_t *p_remainder);
low_power_time_result_t low_power_time_advance(uint32_t phase_cycles,
                                             uint32_t elapsed_cycles);
/* The current, partially elapsed tick is included in expected_ticks. */
uint32_t low_power_time_deadline(uint32_t expected_ticks,
                                uint32_t remaining_cycles);
/* StepTick cannot release the final deadline. Return the safe direct jump. */
uint32_t low_power_time_step(uint32_t elapsed_ticks, uint32_t expected_ticks);

#endif
