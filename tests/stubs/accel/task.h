#ifndef TEST_ACCEL_TASK_H
#define TEST_ACCEL_TASK_H
#include "FreeRTOS.h"
typedef void *TaskHandle_t;

TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
void xTaskNotifyGive(TaskHandle_t task);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *higher_priority);

#endif
