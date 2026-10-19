#ifndef STM32_CLOCK_H
#define STM32_CLOCK_H

#include "clock_model.h"

/* Returns 0, -1 (IO), or -2 (invalid calendar). */
int32_t stm32_clock_read_calendar(clock_calendar_t *p_calendar);
/* Linked/reserved SRAM and Flash load-image lengths, not runtime heap sizes. */
int32_t stm32_clock_image_sizes(uint32_t *p_sram_bytes, uint32_t *p_flash_bytes);

#endif
