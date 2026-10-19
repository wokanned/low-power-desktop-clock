#ifndef TEST_DEBUG_QUEUE_H
#define TEST_DEBUG_QUEUE_H
#include "FreeRTOS.h"
typedef struct { uint8_t data[8][64]; uint32_t count; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(uint32_t depth, uint32_t size, uint8_t *p_storage,
                                 StaticQueue_t *p_control);
BaseType_t xQueueSend(QueueHandle_t queue, const void *p_item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *p_item, TickType_t wait);
BaseType_t xQueueReset(QueueHandle_t queue);
#endif
