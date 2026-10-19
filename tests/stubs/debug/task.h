#ifndef TEST_DEBUG_TASK_H
#define TEST_DEBUG_TASK_H
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void xTaskNotifyGive(TaskHandle_t task);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *p_woken);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskSuspend(TaskHandle_t task);
#endif
