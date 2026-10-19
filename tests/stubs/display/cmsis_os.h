#ifndef TEST_DISPLAY_CMSIS_OS_H
#define TEST_DISPLAY_CMSIS_OS_H
#include <stddef.h>
#include <stdint.h>
typedef void *osThreadId_t;
typedef void (*osThreadFunc_t)(void *p_argument);
typedef struct {
    const char *name;
    void *cb_mem;
    uint32_t cb_size;
    void *stack_mem;
    uint32_t stack_size;
    uint32_t priority;
} osThreadAttr_t;
#define osPriorityNormal 24U
osThreadId_t osThreadNew(osThreadFunc_t function, void *p_argument,
                         const osThreadAttr_t *p_attributes);
osThreadId_t osThreadGetId(void);
int32_t osThreadSuspend(osThreadId_t thread);
int32_t osDelay(uint32_t ticks);
#endif
