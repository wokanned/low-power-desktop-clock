#include "debug.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

/*==================== 模块内部常量 ====================*/

#define DEBUG_QUEUE_DEPTH       8
#define DEBUG_MAX_CHANNELS      16
#define DEBUG_MAX_DATA_BYTES    64   /* 16 通道 × 最大 4 字节/通道 */
#define DEBUG_HEARTBEAT_MS      1000

/*==================== 队列消息结构（模块私有） ====================*/

typedef enum {
    DEBUG_MSG_FRAME,   /* VOFA 格式数据帧 */
    DEBUG_MSG_LOG,     /* 纯文本日志 */
} Debug_MsgType;

typedef struct {
    Debug_MsgType             type;
    VOFA_FormatEnum           fmt;          /* 本帧使用的 VOFA 格式 */
    uint8_t                   ch_cnt;
    VOFA_Data_TypeDef_Enum    data_types[DEBUG_MAX_CHANNELS];
    uint8_t                   data[DEBUG_MAX_DATA_BYTES];
    uint16_t                  log_len;      /* DEBUG_MSG_LOG 的有效长度 */
} Debug_Msg;

/*==================== 模块级静态变量 ====================*/

static VOFA_HandleTypeDef s_hvofa;
static QueueHandle_t      s_debug_queue;

/*==================== 串口发送回调（平台绑定） ====================*/

/*
 * VOFA 要求的发送回调，内部调用 HAL_UART_Transmit。
 * 这是本模块中唯一直接接触 HAL 的函数。后续可提取到 platform/ 层。
 */
static bool s_uart_send_cb(void *p_serial_handle,
                           const uint8_t *p_data,
                           uint16_t len) {
    UART_HandleTypeDef *p_huart = (UART_HandleTypeDef *)p_serial_handle;
    HAL_StatusTypeDef status = HAL_UART_Transmit(p_huart,
                                                  (uint8_t *)p_data,
                                                  len,
                                                  100);
    return (status == HAL_OK);
}

/*==================== 辅助函数 ====================*/

/*
 * 根据类型数组计算数据缓冲区的总字节数。
 * 用于 Debug_SendFrame 确定要拷贝多少原始数据字节。
 */
static uint16_t s_calc_data_size(uint8_t ch_cnt,
                                 VOFA_Data_TypeDef_Enum *p_types) {
    uint16_t total = 0;
    uint8_t i;
    for (i = 0; i < ch_cnt; i++) {
        switch (p_types[i]) {
        case VOFA_DATA_INT:    total += sizeof(int32_t);  break;
        case VOFA_DATA_UINT:   total += sizeof(uint32_t); break;
        case VOFA_DATA_FLOAT:  total += sizeof(float);    break;
        case VOFA_DATA_CHAR:   total += sizeof(int8_t);   break;
        case VOFA_DATA_UCHAR:  total += sizeof(uint8_t);  break;
        case VOFA_DATA_SHORT:  total += sizeof(int16_t);  break;
        case VOFA_DATA_USHORT: total += sizeof(uint16_t); break;
        default: break;
        }
    }
    return total;
}

/*===============================================================
 *  Debug_Init：初始化 VOFA 句柄并创建消息队列
 *===============================================================*/

void Debug_Init(void) {
    /* 初始化 VOFA，绑定 USART2，默认 FireWater 格式 */
    VOFA_Init(&s_hvofa,
              (void *)&huart2,
              s_uart_send_cb,
              VOFA_FMT_FIREFIREWATER,
              VOFA_BAUD_115200);

    /* 创建调试消息队列 */
    s_debug_queue = xQueueCreate(DEBUG_QUEUE_DEPTH, sizeof(Debug_Msg));
}

/*===============================================================
 *  Debug_Log：推送纯文本日志（非阻塞）
 *===============================================================*/

void Debug_Log(const char *p_msg) {
    Debug_Msg msg;
    uint16_t len;
    uint16_t i;

    if (p_msg == NULL || s_debug_queue == NULL) {
        return;
    }

    /* 计算字符串长度，截断到缓冲区上限 */
    len = 0;
    while (p_msg[len] != '\0' && len < DEBUG_MAX_DATA_BYTES) {
        len++;
    }

    msg.type    = DEBUG_MSG_LOG;
    msg.log_len = len;
    for (i = 0; i < len; i++) {
        msg.data[i] = (uint8_t)p_msg[i];
    }

    /* 非阻塞发送：队列满则丢弃（超时时间 = 0） */
    xQueueSend(s_debug_queue, &msg, 0);
}

/*===============================================================
 *  Debug_SendFrame：推送 VOFA 数据帧（非阻塞）
 *===============================================================*/

bool Debug_SendFrame(void *p_data, uint8_t ch_cnt,
                     VOFA_Data_TypeDef_Enum *p_types) {
    Debug_Msg msg;
    uint16_t data_size;
    uint16_t i;
    uint8_t j;

    if (p_data == NULL || ch_cnt == 0 || p_types == NULL) {
        return false;
    }
    if (ch_cnt > DEBUG_MAX_CHANNELS || s_debug_queue == NULL) {
        return false;
    }

    /* 计算原始数据大小并拷贝 */
    data_size = s_calc_data_size(ch_cnt, p_types);
    if (data_size > DEBUG_MAX_DATA_BYTES) {
        return false;
    }

    msg.type   = DEBUG_MSG_FRAME;
    msg.fmt    = s_hvofa.fmt;   /* 使用当前全局格式 */
    msg.ch_cnt = ch_cnt;

    for (j = 0; j < ch_cnt; j++) {
        msg.data_types[j] = p_types[j];
    }
    for (i = 0; i < data_size; i++) {
        msg.data[i] = ((uint8_t *)p_data)[i];
    }
    msg.log_len = 0;

    if (xQueueSend(s_debug_queue, &msg, 0) != pdPASS) {
        return false;
    }
    return true;
}

/*===============================================================
 *  Debug_SendFloats：便捷发送浮点数组（JustFloat 格式）
 *===============================================================*/

void Debug_SendFloats(float *p_data, uint8_t ch_cnt) {
    Debug_Msg msg;
    uint16_t data_size;
    uint8_t i;

    if (p_data == NULL || ch_cnt == 0 || s_debug_queue == NULL) {
        return;
    }
    if (ch_cnt > DEBUG_MAX_CHANNELS) {
        return;
    }

    data_size = (uint16_t)(ch_cnt * sizeof(float));

    msg.type   = DEBUG_MSG_FRAME;
    msg.fmt    = VOFA_FMT_JUSTFLOAT;   /* 强制 JustFloat 二进制格式 */
    msg.ch_cnt = ch_cnt;

    for (i = 0; i < ch_cnt; i++) {
        msg.data_types[i] = VOFA_DATA_FLOAT;
    }
    for (i = 0; i < data_size; i++) {
        msg.data[i] = ((uint8_t *)p_data)[i];
    }
    msg.log_len = 0;

    xQueueSend(s_debug_queue, &msg, 0);
}

/*===============================================================
 *  StartDebugTask：调试任务入口
 *===============================================================*/

void StartDebugTask(void *p_argument) {
    Debug_Msg msg;
    TickType_t last_heartbeat;
    VOFA_FormatEnum saved_fmt;
    char info_buf[64];
    int info_len;
    TickType_t now;
    uint32_t tick_val;
    VOFA_Data_TypeDef_Enum heartbeat_type;

    /* 防止未使用参数警告 */
    (void)p_argument;

    /* 队列未创建则直接挂起（Debug_Init 失败） */
    if (s_debug_queue == NULL) {
        vTaskSuspend(NULL);
        /* 不会执行到这里 */
    }

    /*---------- 上电启动信息 ----------*/

    /* 启动横幅 */
    s_uart_send_cb((void *)&huart2,
                   (const uint8_t *)"DeskClock Boot\r\n", 16);

    /* 系统时钟频率 */
    info_len = snprintf(info_buf, sizeof(info_buf),
                        "SystemCoreClock=%lu\r\n", SystemCoreClock);
    if (info_len > 0) {
        s_uart_send_cb((void *)&huart2,
                       (uint8_t *)info_buf, (uint16_t)info_len);
    }

    /* 初始 tick */
    info_len = snprintf(info_buf, sizeof(info_buf),
                        "TickStart=%lu\r\n",
                        (uint32_t)xTaskGetTickCount());
    if (info_len > 0) {
        s_uart_send_cb((void *)&huart2,
                       (uint8_t *)info_buf, (uint16_t)info_len);
    }

    /*---------- 主循环 ----------*/

    last_heartbeat = xTaskGetTickCount();

    for (;;) {
        /*
         * 阻塞等待队列消息，超时时间 = 心跳间隔。
         * 超时后会自然跳出，执行下面的心跳逻辑。
         */
        if (xQueueReceive(s_debug_queue, &msg,
                          pdMS_TO_TICKS(DEBUG_HEARTBEAT_MS)) == pdPASS) {

            if (msg.type == DEBUG_MSG_FRAME) {
                /*
                 * 临时切换 VOFA 格式（安全：只有本任务使用 s_hvofa）
                 */
                saved_fmt   = s_hvofa.fmt;
                s_hvofa.fmt = msg.fmt;
                VOFA_SendData(&s_hvofa, msg.data,
                              msg.ch_cnt, msg.data_types);
                s_hvofa.fmt = saved_fmt;

            } else if (msg.type == DEBUG_MSG_LOG) {
                /* 纯文本日志：直接通过串口回调发送 */
                s_uart_send_cb((void *)&huart2,
                               msg.data, msg.log_len);
            }
        }

        /*---------- 心跳：每 1 秒发送一次 tick ----------*/
        now = xTaskGetTickCount();
        if ((now - last_heartbeat) >= pdMS_TO_TICKS(DEBUG_HEARTBEAT_MS)) {
            last_heartbeat = now;

            /* 以 FireWater 文本格式发送 tick 值 */
            tick_val      = (uint32_t)now;
            heartbeat_type = VOFA_DATA_UINT;
            VOFA_SendData(&s_hvofa, &tick_val, 1, &heartbeat_type);
        }
    }
}
