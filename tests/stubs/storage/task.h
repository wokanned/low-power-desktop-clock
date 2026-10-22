#ifndef TEST_STORAGE_TASK_H
#define TEST_STORAGE_TASK_H
#include "FreeRTOS.h"
void storage_test_enter(void);
void storage_test_exit(void);
TickType_t xTaskGetTickCount(void);
#define taskENTER_CRITICAL() storage_test_enter()
#define taskEXIT_CRITICAL() storage_test_exit()
#endif
