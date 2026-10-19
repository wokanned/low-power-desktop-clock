#ifndef TEST_CLOCK_FREERTOS_H
#define TEST_CLOCK_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 1000U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
size_t xPortGetFreeHeapSize(void);
#endif
