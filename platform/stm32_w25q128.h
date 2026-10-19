#ifndef STM32_W25Q128_H
#define STM32_W25Q128_H

#include "flash_w25q128.h"

/* Bind during system initialization, before tasks can access SPI1.
 * All bound contexts share one static mutex. Driver calls require task context. */
int32_t stm32_w25q128_bind(w25q128_ctx_t *p_ctx);

#endif
