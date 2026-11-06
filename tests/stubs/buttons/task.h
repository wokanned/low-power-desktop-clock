#ifndef TEST_BUTTON_TASK_H
#define TEST_BUTTON_TASK_H
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *p_woken);
#endif
