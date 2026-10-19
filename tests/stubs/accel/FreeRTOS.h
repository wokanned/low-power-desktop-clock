#ifndef TEST_ACCEL_FREERTOS_H
#define TEST_ACCEL_FREERTOS_H
#include <stdint.h>

typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef struct { uint32_t unused; } StaticTask_t;
#define configTICK_RATE_HZ 1000U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
void test_accel_critical_enter(void);
void test_accel_critical_exit(void);
#define taskENTER_CRITICAL() test_accel_critical_enter()
#define taskEXIT_CRITICAL() test_accel_critical_exit()

#endif
