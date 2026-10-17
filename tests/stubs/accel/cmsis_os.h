#ifndef TEST_ACCEL_CMSIS_OS_H
#define TEST_ACCEL_CMSIS_OS_H
#include <stddef.h>
#include <stdint.h>

typedef void *osThreadId_t;
typedef void (*osThreadFunc_t)(void *argument);
typedef struct {
    const char *name;
    void *cb_mem;
    uint32_t cb_size;
    void *stack_mem;
    uint32_t stack_size;
    uint32_t priority;
} osThreadAttr_t;
#define osPriorityAboveNormal 32U
osThreadId_t osThreadNew(osThreadFunc_t function, void *argument,
                         const osThreadAttr_t *attributes);

#endif
