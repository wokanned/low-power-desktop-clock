#ifndef STM32_ADXL345_H
#define STM32_ADXL345_H
#include "imu_adxl345.h"

/* PCLK1=80 MHz, analog filter on, digital filter off. Fast-mode target. */
#define STM32_I2C2_FAST_TIMING 0x00702991U
int32_t stm32_adxl345_bind(adxl345_ctx_t *p_ctx);
typedef void (*stm32_adxl345_motion_notify_t)(void);
/* Callback runs in EXTI9_5 IRQ, after EXTI8 has been masked against storms. */
void stm32_adxl345_motion_init(stm32_adxl345_motion_notify_t notify);
void stm32_adxl345_motion_disarm(void);
/* Mask line and consume MCU pending before the owner reads sensor INT_SOURCE. */
void stm32_adxl345_motion_take_pending(void);
uint8_t stm32_adxl345_motion_high(void);
/* Enable only when sensor line and all MCU pending state are clear. */
uint8_t stm32_adxl345_motion_arm(void);
#endif
