#ifndef __DEBUG_H
#define __DEBUG_H

/*
 * 说明：
 *   VOFA 调试服务模块 — 单例 Debug Task + 消息队列架构。
 *
 * 使用流程：
 *   1. main.c 中，外设初始化完成后调用 Debug_Init()。
 *   2. freertos.c 中，用 osThreadNew 创建 StartDebugTask。
 *   3. 任意任务中调用 Debug_SendFrame / Debug_SendFloats / Debug_Log 推送调试数据。
 *
 * 注意：
 *   - 发送接口均为非阻塞。队列满时消息会被静默丢弃。
 *   - 该模块硬绑定 USART2（调试串口），不打算支持多串口实例。
 */

#include <stdint.h>
#include <stdbool.h>
#include "vofa.h"

/*==================== 公开接口 ====================*/

/**
 * @brief  初始化调试服务（VOFA + 消息队列）
 * @note   必须在 FreeRTOS 调度器启动之前调用。
 *         绑定 USART2（huart2），默认使用 FireWater 文本格式。
 */
void Debug_Init(void);

/**
 * @brief  FreeRTOS 调试任务入口
 * @note   上电后发送一次启动信息，之后周期发送 tick 心跳并处理队列中的调试消息。
 */
void StartDebugTask(void *argument);

/**
 * @brief  推送一帧 VOFA 数据到发送队列（非阻塞）
 * @param  data    : 数据缓冲区指针
 * @param  ch_cnt  : 通道数量
 * @param  types   : 每个通道对应的数据类型数组
 * @return true  : 消息已入队
 *         false : 队列满，消息被丢弃
 */
bool Debug_SendFrame(void *data, uint8_t ch_cnt, VOFA_Data_TypeDef_Enum *types);

/**
 * @brief  便捷接口：以 JustFloat 格式发送浮点数组（非阻塞）
 * @param  data   : 浮点数组指针
 * @param  ch_cnt : 通道数量（最大 16）
 */
void Debug_SendFloats(float *data, uint8_t ch_cnt);

/**
 * @brief  便捷接口：发送纯文本日志（非阻塞）
 * @param  msg : 以 '\0' 结尾的字符串
 */
void Debug_Log(const char *msg);

#endif /* __DEBUG_H */
