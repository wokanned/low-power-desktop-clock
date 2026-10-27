#ifndef STM32_CLOCK_H
#define STM32_CLOCK_H

#include "clock_model.h"

/* Returns 0, -1 (IO), or -2 (invalid calendar). */
int32_t stm32_clock_read_calendar(clock_calendar_t *p_calendar);
/* Task-context 24-hour hardware calendar update. Weekday is derived from date.
 * Returns 0 only after a single INIT transaction and verified readback (exact
 * value or its next second), -1 (IO/busy), -2 (invalid), or -3 (verification).
 * Output is unchanged on failure. Does not change alarms or backup registers. */
int32_t stm32_clock_write_calendar(const clock_calendar_t *p_calendar,
                                  clock_calendar_t *p_verified);
/* Linked/reserved SRAM and Flash load-image lengths, not runtime heap sizes. */
int32_t stm32_clock_image_sizes(uint32_t *p_sram_bytes, uint32_t *p_flash_bytes);

#endif
