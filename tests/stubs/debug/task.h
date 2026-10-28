#ifndef TEST_DEBUG_TASK_H
#define TEST_DEBUG_TASK_H
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_SUSPENDED 1
#define taskSCHEDULER_RUNNING 2
BaseType_t xTaskGetSchedulerState(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TickType_t xTaskGetTickCount(void);
void xTaskNotifyGive(TaskHandle_t task);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *p_woken);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskSuspend(TaskHandle_t task);
#endif
