#ifndef __VOFA_H
#define __VOFA_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * 说明：
 *  这个头文件是 VOFA 通用发送/接收库的对外接口声明，不依赖 HAL。
 *  - 只关心“有一个能发串口字节的函数（回调）”，其余都由上层提供。
 *  - 支持三种数据格式：FireWater / JustFloat / RawData。
 *  - 内部还带一个简单的指令接收和解析逻辑（VOFA_ProcessCmd）。
 *
 * 使用流程简要：
 *  1. 定义一个 VOFA_HandleTypeDef 变量（比如全局 hvofa）。
 *  2. 在初始化时调用 VOFA_Init，传入：
 *      - 串口句柄指针（void*）
 *      - 串口发送回调函数（VOFA_SerialSendFunc）
 *      - 初始格式（VOFA_FMT_xxx）
 *      - 波特率（仅记录用）
 *  3. 想发数据给 VOFA+ 时，调用 VOFA_SendData。
 *  4. 若需要接收来自 PC/VOFA 的命令：
 *      - 在中断中每收到 1 字节就调用 VOFA_RxCallback
 *      - 在主循环里周期性调用 VOFA_Poll，它会自动提取一帧并调用 VOFA_ProcessCmd。
 */

/*==================== 1. 数据类型枚举 ====================*/
/*
 * VOFA_Data_TypeDef_Enum 表示每个“通道”的数据类型。
 * VOFA_SendData 可以根据这些类型把你的 void* data 正确解释成 int/float 等。
 *
 * 注意：
 *  - 目前不再支持 double 类型
 *  - 实际使用时，强烈建议“每一帧的所有通道都用同一种类型”，例如全部 VOFA_DATA_FLOAT。
 */

typedef enum {
    VOFA_DATA_INT,        // int32_t
    VOFA_DATA_UINT,       // uint32_t
    VOFA_DATA_FLOAT,      // float
    VOFA_DATA_CHAR,       // int8_t
    VOFA_DATA_UCHAR,      // uint8_t
    VOFA_DATA_SHORT,      // int16_t
    VOFA_DATA_USHORT,     // uint16_t
	//double数据类型不再支持
}VOFA_Data_TypeDef_Enum;

/*==================== 2. 数据格式枚举 ====================*/
/*
 * VOFA_FormatEnum 表示当前选择的数据打包格式（和 VOFA+ 软件里的“数据引擎”对应）。
 *
 *  - VOFA_FMT_FIREFIREWATER：FireWater 文本格式，一帧是一行 ASCII 字符，以 '\n' 结束。
 *  - VOFA_FMT_JUSTFLOAT    ：JustFloat 二进制格式，一帧是 N 个 float + 0x00 0x00 0x80 0x7F 帧尾。
 *  - VOFA_FMT_RAWDATA      ：原始字节格式，不做解析，直接按原始类型发。
 */

typedef enum {
    VOFA_FMT_FIREFIREWATER = 0,
    VOFA_FMT_JUSTFLOAT,
    VOFA_FMT_RAWDATA,
    VOFA_FMT_MAX
}VOFA_FormatEnum;

/*==================== 3. 波特率枚举（记录用） ====================*/
/*
 * VOFA_BaudRateEnum 只是记录当前串口波特率，便于你自己查阅或显示。
 * 真正配置硬件波特率，仍然由上层（HAL/裸机等）来做，VOFA 库不会改硬件寄存器。
 */

typedef enum {
    VOFA_BAUD_1200    = 1200,
    VOFA_BAUD_2400    = 2400,
    VOFA_BAUD_4800    = 4800,
    VOFA_BAUD_9600    = 9600,
    VOFA_BAUD_19200   = 19200,
    VOFA_BAUD_38400   = 38400,
    VOFA_BAUD_57600   = 57600,
    VOFA_BAUD_74800   = 74800,
    VOFA_BAUD_115200  = 115200,
    VOFA_BAUD_230400  = 230400,
    VOFA_BAUD_460800  = 460800,
    VOFA_BAUD_500000  = 500000,
    VOFA_BAUD_576000  = 576000,
    VOFA_BAUD_921600  = 921600,
    VOFA_BAUD_1000000 = 1000000,
    VOFA_BAUD_1152000 = 1152000,
    VOFA_BAUD_1500000 = 1500000,
    VOFA_BAUD_2000000 = 2000000,
    VOFA_BAUD_2500000 = 2500000,
    VOFA_BAUD_3000000 = 3000000,
    VOFA_BAUD_3500000 = 3500000,
    VOFA_BAUD_4000000 = 4000000,
    VOFA_BAUD_MAX
}VOFA_BaudRateEnum;


/*==================== 4. 串口发送回调函数类型 ====================*/
/*
 * VOFA_SerialSendFunc 是一个函数指针类型，表示“串口发送函数”。
 * VOFA 库自身不会关心底层是 HAL 还是裸寄存器，只要你提供这样一个函数即可：
 *
 *   bool send_cb(void *serial_handle, const uint8_t *data, uint16_t len);
 *
 * 参数：
 *   serial_handle：上层传进来的串口句柄指针（比如 UART_HandleTypeDef*），VOFA 不解释它，只原样传回给回调。
 *   data         ：要发送的字节数组指针
 *   len          ：要发送的字节数
 *
 * 返回：
 *   true  表示发送成功；
 *   false 表示发送失败（比如硬件故障、队列满等）。
 */

typedef bool (*VOFA_SerialSendFunc)(void *serial_handle,
                                    const uint8_t *data,
                                    uint16_t len);

/*==================== 5. 缓冲区大小定义 ====================*/
/*
 * VOFA_RING_BUF_SIZE ：环形缓冲区大小，用于缓存接收到的原始字节（从串口中断进来）。
 * VOFA_FRAME_BUF_SIZE：单帧最大长度，一旦检测到帧尾，就把一帧提取到 frame_buf。
 *
 * 根据你的场景，可以适当调大/调小。
 */

#define VOFA_RING_BUF_SIZE   512
#define VOFA_FRAME_BUF_SIZE  256

/*==================== 6. VOFA 句柄结构体 ====================*/
/*
 * VOFA_HandleTypeDef 是 VOFA 库的“上下文对象”，相当于一个类的实例：
 *  - 里面存放串口发送回调、接收缓冲区、当前格式等所有状态。
 *  - 你一般定义一个全局 VOFA_HandleTypeDef 变量（比如 hvofa），在初始化时传给 VOFA_Init。
 */

typedef struct {
    /* 串口适配层（与具体硬件解耦） */
    void *serial_handle;              // 任意串口对象指针（例如 UART_HandleTypeDef*）
    VOFA_SerialSendFunc send_callback;// 串口发送回调函数指针（由上层实现，VOFA 调用）

    VOFA_FormatEnum fmt;              // 当前 VOFA 数据格式（FireWater/JustFloat/RawData）
    VOFA_BaudRateEnum baud;           // 当前波特率（只记录，不改硬件）

    /* 环形缓冲区（用来缓存串口接收到的原始数据） */
    uint8_t  rx_ring_buf[VOFA_RING_BUF_SIZE]; // 实际数据缓冲区
    uint16_t rx_read_idx;                     // 读索引（从这里取数据）
    uint16_t rx_write_idx;                    // 写索引（写到这里）
    uint16_t rx_overflow_cnt;                 // 溢出计数（写满时被覆盖的次数）
    uint32_t rx_total_bytes;                  // 累计收到的字节数

    /* 提取出的一帧数据 */
    uint8_t  frame_buf[VOFA_FRAME_BUF_SIZE];  // 存放已经提取好的单帧数据（不含帧尾）
    uint16_t frame_len;                       // 当前帧有效数据长度
    uint16_t frame_count;                     // 已经提取但尚未处理的帧数量（简易计数）

    bool     new_frame_flag;                  // 是否有新帧等待处理（true 表示 frame_buf 有新数据）

    /* 处理状态 */
    bool     is_processing;                   // 当前是否处于处理状态（防止重入）
} VOFA_HandleTypeDef;

/*==================== 7. 对外函数声明 ====================*/

/**
 * @brief  VOFA 初始化函数
 * @param  hvofa         : VOFA 句柄指针（必须是已分配的变量）
 * @param  serial_handle : 串口句柄指针（由上层定义，比如 &huart2）
 * @param  send_cb       : 串口发送回调函数指针
 * @param  init_fmt      : 初始数据格式（FireWater / JustFloat / RawData）
 * @param  init_baud     : 初始波特率（仅记录用）
 * @note
 *  - 该函数会清空环形缓冲区和帧缓冲区，并设置初始状态。
 *  - 不会实际修改硬件串口配置，只是记录 fmt 和 baud。
 */
void VOFA_Init(VOFA_HandleTypeDef *hvofa,
               void *serial_handle,
               VOFA_SerialSendFunc send_cb,
               VOFA_FormatEnum init_fmt,
               VOFA_BaudRateEnum init_baud);

/**
 * @brief  发送一帧数据到 VOFA+
 * @param  hvofa     : VOFA 句柄指针
 * @param  data      : 通道数据数组的起始地址（void*，根据 data_types 解释）
 * @param  ch_cnt    : 通道数
 * @param  data_types: 每个通道对应的数据类型数组（长度为 ch_cnt）
 * @return true  : 发送成功
 *         false : 参数错误或发送回调返回失败
 *
 * 说明：
 *  - 根据 hvofa->fmt 不同，采用不同协议格式打包：
 *      VOFA_FMT_FIREFIREWATER : 文本 "v0,v1,...,vN\n"
 *      VOFA_FMT_JUSTFLOAT     : N个float的二进制 + 帧尾 00 00 80 7F
 *      VOFA_FMT_RAWDATA       : 原始字节，不做解析
 */
bool VOFA_SendData(VOFA_HandleTypeDef *hvofa,
                   void *data,
                   uint8_t ch_cnt,
                   VOFA_Data_TypeDef_Enum *data_types);

/**
 * @brief  单字节接收回调（一般在串口中断里调用）
 * @param  hvofa : VOFA 句柄指针
 * @param  data  : 从串口收到的 1 个字节
 * @note
 *  - 会把该字节写入环形缓冲区 rx_ring_buf。
 *  - 不会立刻解析帧，只是缓存，真正提取帧由 VOFA_Poll 完成。
 */
void VOFA_RxCallback(VOFA_HandleTypeDef *hvofa, uint8_t data);

/**
 * @brief  轮询处理函数（建议在主循环中周期性调用）
 * @param  hvofa : VOFA 句柄指针
 * @note
 *  - 内部会检查环形缓冲区是否有数据，并尝试提取一帧（通过回车/换行判断帧尾）。
 *  - 提取到完整帧后，会把数据拷贝到 frame_buf，并调用 VOFA_ProcessCmd 进行处理。
 */
void VOFA_Poll(VOFA_HandleTypeDef *hvofa);

/**
 * @brief  单帧命令处理函数
 * @param  hvofa : VOFA 句柄指针
 * @note
 *  - 默认实现里，简单解析了几种自定义命令（0x01 / 0x04 开头），并通过 send_callback 回 ACK。
 *  - 你可以根据需要在此处扩展自己的协议。
 *  - 一般不用在主循环直接调用 VOFA_ProcessCmd，而是让 VOFA_Poll 内部调用。
 */
void VOFA_ProcessCmd(VOFA_HandleTypeDef *hvofa);

#endif

