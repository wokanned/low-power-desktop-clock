#ifndef TEST_EPD_SEMPHR_H
#define TEST_EPD_SEMPHR_H
#include "FreeRTOS.h"
typedef struct { uint32_t count; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *p_control);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t *p_woken);
#endif
