#ifndef TEST_CLOCK_TASK_H
#define TEST_CLOCK_TASK_H
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void clock_test_enter(void);
void clock_test_exit(void);
#define taskENTER_CRITICAL() clock_test_enter()
#define taskEXIT_CRITICAL() clock_test_exit()
#endif
