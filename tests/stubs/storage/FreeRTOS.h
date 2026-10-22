#ifndef TEST_STORAGE_FREERTOS_H
#define TEST_STORAGE_FREERTOS_H
#include <stdint.h>
typedef uint32_t StackType_t;
typedef uint32_t TickType_t;
typedef struct { uint32_t unused; } StaticTask_t;
#define pdPASS 1
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
#define portMAX_DELAY 0xFFFFFFFFUL
#endif
