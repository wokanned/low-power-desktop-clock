#include "debug.h"
#include "stm32_debug_uart.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#define DEBUG_QUEUE_DEPTH 8U
#define DEBUG_COUNTER_MAX 0x00FFFFFFU

static VOFA_HandleTypeDef s_hvofa;
static QueueHandle_t s_debug_queue;
static StaticQueue_t s_queue_control;
static uint8_t s_queue_storage[DEBUG_QUEUE_DEPTH * VOFA_MAX_CHANNELS * sizeof(float)];
static volatile uint32_t s_queue_drop_count;
static volatile uint32_t s_uart_error_count;

void Debug_Init(void)
{
    VOFA_Init(&s_hvofa, NULL, stm32_debug_uart_send,
              VOFA_FMT_JUSTFLOAT, VOFA_BAUD_115200);
    s_queue_drop_count = 0U;
    s_uart_error_count = 0U;
    s_debug_queue = xQueueCreateStatic(DEBUG_QUEUE_DEPTH,
        VOFA_MAX_CHANNELS * sizeof(float), s_queue_storage, &s_queue_control);
}

bool Debug_IsReady(void)
{
    return s_debug_queue != NULL;
}

void Debug_Log(const char *p_msg)
{
    (void)p_msg;
}

bool Debug_SendFloats(const float *p_data, uint8_t ch_cnt)
{
    if (p_data == NULL || ch_cnt != VOFA_MAX_CHANNELS || s_debug_queue == NULL) {
        return false;
    }
    if (xQueueSend(s_debug_queue, p_data, 0U) != pdPASS) {
        taskENTER_CRITICAL();
        if (s_queue_drop_count < DEBUG_COUNTER_MAX) {
            s_queue_drop_count++;
        }
        taskEXIT_CRITICAL();
        return false;
    }
    return true;
}

bool Debug_SendFrame(const void *p_data, uint8_t ch_cnt,
                     const VOFA_Data_TypeDef_Enum *p_types)
{
    uint8_t i;
    if (p_types == NULL || ch_cnt != VOFA_MAX_CHANNELS) {
        return false;
    }
    for (i = 0U; i < ch_cnt; i++) {
        if (p_types[i] != VOFA_DATA_FLOAT) {
            return false;
        }
    }
    return Debug_SendFloats((const float *)p_data, ch_cnt);
}

void StartDebugTask(void *p_argument)
{
    float frame[VOFA_MAX_CHANNELS];
    VOFA_Data_TypeDef_Enum types[VOFA_MAX_CHANNELS];
    uint8_t i;
    (void)p_argument;
    for (i = 0U; i < VOFA_MAX_CHANNELS; i++) {
        types[i] = VOFA_DATA_FLOAT;
    }
    if (!Debug_IsReady()) {
        vTaskSuspend(NULL);
    }
    for (;;) {
        if (xQueueReceive(s_debug_queue, frame, portMAX_DELAY) == pdPASS) {
            /* Capture current transmit counters, not their old queued values. */
            frame[13] = (float)s_queue_drop_count;
            frame[14] = (float)s_uart_error_count;
            if (!VOFA_SendData(&s_hvofa, frame, VOFA_MAX_CHANNELS, types)) {
                if (s_uart_error_count < DEBUG_COUNTER_MAX) {
                    s_uart_error_count++;
                }
            }
        }
    }
}

uint32_t Debug_GetQueueDropCount(void) { return s_queue_drop_count; }
uint32_t Debug_GetUartErrorCount(void) { return s_uart_error_count; }
