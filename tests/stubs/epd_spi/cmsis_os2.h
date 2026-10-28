#ifndef TEST_EPD_CMSIS_OS2_H
#define TEST_EPD_CMSIS_OS2_H
#include <stdint.h>
typedef enum { osKernelInactive, osKernelReady, osKernelRunning } osKernelState_t;
osKernelState_t osKernelGetState(void);
uint32_t osKernelGetTickFreq(void);
int32_t osDelay(uint32_t ticks);
#endif
