#ifndef TEST_STORAGE_CMSIS_OS_H
#define TEST_STORAGE_CMSIS_OS_H
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
#define osPriorityBelowNormal 16U
osThreadId_t osThreadNew(osThreadFunc_t function, void *p_argument,
                         const osThreadAttr_t *p_attributes);
#endif
