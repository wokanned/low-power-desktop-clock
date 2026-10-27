#ifndef TEST_CLOCK_TASK_H
#define TEST_CLOCK_TASK_H
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout);
void clock_test_enter(void);
void clock_test_exit(void);
#define taskENTER_CRITICAL() clock_test_enter()
#define taskEXIT_CRITICAL() clock_test_exit()
#endif
