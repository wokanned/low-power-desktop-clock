#ifndef TEST_CLOCK_FREERTOS_H
#define TEST_CLOCK_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int32_t BaseType_t;
typedef void *TaskHandle_t;
#define pdTRUE ((BaseType_t)1)
#define configTICK_RATE_HZ 1000U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
size_t xPortGetFreeHeapSize(void);
#endif
