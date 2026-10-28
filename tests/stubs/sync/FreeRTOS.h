#ifndef TEST_SYNC_FREERTOS_H
#define TEST_SYNC_FREERTOS_H
#include "../debug/FreeRTOS.h"
typedef uint32_t StackType_t;
typedef struct { uint32_t unused; } StaticTask_t;
size_t xPortGetFreeHeapSize(void);
void sync_test_enter(void);
void sync_test_exit(void);
#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL
#define taskENTER_CRITICAL() sync_test_enter()
#define taskEXIT_CRITICAL() sync_test_exit()
#endif
