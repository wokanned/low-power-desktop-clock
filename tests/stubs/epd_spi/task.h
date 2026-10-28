#ifndef TEST_EPD_TASK_H
#define TEST_EPD_TASK_H
#include "FreeRTOS.h"
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_SUSPENDED 1
#define taskSCHEDULER_RUNNING 2
BaseType_t xTaskGetSchedulerState(void);
TickType_t xTaskGetTickCount(void);
#endif
