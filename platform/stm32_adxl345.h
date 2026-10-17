#ifndef STM32_ADXL345_H
#define STM32_ADXL345_H
#include "imu_adxl345.h"

/* PCLK1=80 MHz, analog filter on, digital filter off. Fast-mode target. */
#define STM32_I2C2_FAST_TIMING 0x00702991U
int32_t stm32_adxl345_bind(adxl345_ctx_t *p_ctx);
#endif
