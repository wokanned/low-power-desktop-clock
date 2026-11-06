#ifndef TEST_BUTTON_FREERTOS_H
#define TEST_BUTTON_FREERTOS_H
#include <stdint.h>
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef struct { uint32_t unused; } StaticTask_t;
typedef int32_t BaseType_t;
typedef void *TaskHandle_t;
#define configTICK_RATE_HZ 1000U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define portYIELD_FROM_ISR(value) ((void)(value))
#endif
