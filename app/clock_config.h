#ifndef CLOCK_CONFIG_H
#define CLOCK_CONFIG_H

#include "clock_model.h"
#include "flash_w25q128.h"

#define CLOCK_CONFIG_SECTOR_A 0x00FFE000UL
#define CLOCK_CONFIG_SECTOR_B 0x00FFF000UL
#define CLOCK_CONFIG_RECORD_SIZE 24U
#define CLOCK_CONFIG_ERROR_CORRUPT (-20)
#define CLOCK_CONFIG_NO_SLOT 0xFFU

typedef struct {
    clock_format_t format;
    uint32_t sequence;
    uint8_t active_slot;
    uint8_t valid;
    uint8_t degraded;
} clock_config_state_t;

/* Exclusive owner only. Each driver call takes its own device lock. */
int32_t clock_config_load(const w25q128_ctx_t *p_context,
                          clock_config_state_t *p_state);
/* Inactive-sector erase/body verify/commit verify; state changes on success only.
 * After any I/O failure reload before attempting another save. */
int32_t clock_config_save(const w25q128_ctx_t *p_context,
                          clock_config_state_t *p_state, clock_format_t format);

#endif
