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
/* Owner-task only. tick_ms anchors the function entry in a 1ms monotonic
 * domain. Timestamp output uses that same domain. Outputs unchanged on error.
 * Returns -4 when the read/operation timing budget cannot be established. */
int32_t stm32_clock_read_timestamp(uint32_t tick_ms, clock_timestamp_t *p_sample);
/* Input calendar/millisecond denotes function entry, not the later INIT exit.
 * Compensates elapsed IO time, applies the hardware fractional shift, then
 * verifies within 20ms of this local target. This is not an NTP accuracy test. */
int32_t stm32_clock_write_timestamp(const clock_timestamp_t *p_requested,
                                   uint32_t tick_ms,
                                   clock_timestamp_t *p_verified);
/* Linked/reserved SRAM and Flash load-image lengths, not runtime heap sizes. */
int32_t stm32_clock_image_sizes(uint32_t *p_sram_bytes, uint32_t *p_flash_bytes);

#endif
