#ifndef TEST_STORAGE_QUEUE_H
#define TEST_STORAGE_QUEUE_H
#include <stdint.h>
#include "FreeRTOS.h"
typedef struct { uint32_t unused; } StaticQueue_t;
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(uint32_t length, uint32_t item_size,
                                uint8_t *p_bytes, StaticQueue_t *p_control);
int xQueueSend(QueueHandle_t queue, const void *p_item, uint32_t wait);
int xQueueReceive(QueueHandle_t queue, void *p_item, uint32_t wait);
#endif
