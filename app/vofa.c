#include "vofa.h"

/*==================== JustFloat 帧尾定义 ====================*/
/*
 * JustFloat 协议的帧结构：
 *  [ N 个 float 的小端二进制字节 ][ 0x00 0x00 0x80 0x7F ]
 *
 * 这里定义的 VOFA_JUSTFLOAT_TAIL 就是最后这 4 个字节。
 * 它在小端下的 32 位整型值是 0x7F800000，对应 IEEE754 单精度浮点数的 +∞。
 * VOFA+ 软件靠这 4 个字节来识别“一帧结束”。
 */

#define VOFA_JUSTFLOAT_TAIL_LEN 4
static const uint8_t VOFA_JUSTFLOAT_TAIL[VOFA_JUSTFLOAT_TAIL_LEN] = {
    0x00, 0x00, 0x80, 0x7F
};

/*===============================================================
 *  以下是一组“环形缓冲区”相关的静态函数，
 *  用来缓存从串口中断回调 VOFA_RxCallback 收到的原始字节。
 *===============================================================*/

/*==================== 判断环形缓冲区是否为空 ====================*/
static bool VOFA_IsRingBufferEmpty(VOFA_HandleTypeDef *hvofa) {
    return hvofa->rx_read_idx == hvofa->rx_write_idx;
}

/*==================== 判断环形缓冲区是否已满 ====================*/
/*
 * 环形缓冲区满的判定方法：
 *  - 我们约定“留一个空槽”来区分“空”和“满”。
 *  - 当 (write_idx + 1) % SIZE == read_idx 时，认为缓冲区已满。
 */
static bool VOFA_IsRingBufferFull(VOFA_HandleTypeDef *hvofa) {
    return ((hvofa->rx_write_idx + 1) % VOFA_RING_BUF_SIZE) == hvofa->rx_read_idx;
}

/*==================== 获取缓冲区中可用数据长度 ====================*/
/*
 * 返回当前环形缓冲区中“未读字节数”。
 */
static uint16_t VOFA_RingBufferAvailable(VOFA_HandleTypeDef *hvofa) {
    if (hvofa->rx_write_idx >= hvofa->rx_read_idx) {
        return (uint16_t)(hvofa->rx_write_idx - hvofa->rx_read_idx);
    } else {
        // 写指针已经绕回到头部，需要分两段计算
        return (uint16_t)(VOFA_RING_BUF_SIZE - hvofa->rx_read_idx + hvofa->rx_write_idx);
    }
}

/*==================== 从环形缓冲区读取一个字节 ====================*/
/*
 * 若缓冲区非空，返回 rx_ring_buf[rx_read_idx]，并把 rx_read_idx 递增。
 * 若为空，返回 0（但通常你要先检查是否为空）。
 */
static uint8_t VOFA_RingBufferRead(VOFA_HandleTypeDef *hvofa) {
    uint8_t data = 0;
    if (!VOFA_IsRingBufferEmpty(hvofa)) {
        data = hvofa->rx_ring_buf[hvofa->rx_read_idx];
        hvofa->rx_read_idx = (uint16_t)((hvofa->rx_read_idx + 1) % VOFA_RING_BUF_SIZE);
    }
    return data;
}

/*==================== 向环形缓冲区写入一个字节 ====================*/
/*
 * 若缓冲区已满，则丢弃最旧的一个字节（读指针前进一位），并统计溢出次数。
 * 然后把新字节写到写指针位置，写指针前进一位。
 */
static void VOFA_RingBufferWrite(VOFA_HandleTypeDef *hvofa, uint8_t data) {
    if (VOFA_IsRingBufferFull(hvofa)) {
        // 丢弃一个旧数据，避免覆盖读指针未知的数据
        hvofa->rx_read_idx = (uint16_t)((hvofa->rx_read_idx + 1) % VOFA_RING_BUF_SIZE);
        hvofa->rx_overflow_cnt++;
    }
    hvofa->rx_ring_buf[hvofa->rx_write_idx] = data;
    hvofa->rx_write_idx = (uint16_t)((hvofa->rx_write_idx + 1) % VOFA_RING_BUF_SIZE);
    hvofa->rx_total_bytes++;
}

/*==================== 偷看（不移动读指针）指定偏移位置的字节 ====================*/
/*
 * 只读环形缓冲区中 rx_read_idx + offset 位置的字节，不改变读写指针。
 * 用于“寻找帧尾”时预览数据。
 */
static uint8_t VOFA_RingBufferPeek(VOFA_HandleTypeDef *hvofa, uint16_t offset) {
    uint16_t idx = (uint16_t)((hvofa->rx_read_idx + offset) % VOFA_RING_BUF_SIZE);
    return hvofa->rx_ring_buf[idx];
}

/*===============================================================
 *  以下是一组“帧尾检测 & 帧提取”函数，
 *  主要用于 FireWater 文本格式（以 '\r' / '\n' 为帧结尾）。
 *===============================================================*/

/*==================== 帧尾检测函数 ====================*/
/*
 * VOFA_CheckPacketTail:
 *   - 用来检测从某个 offset 开始的字节是不是“帧尾”：
 *       '\r' / '\n' / "\r\n" / "\n\r"
 *   - 如果是，返回 true，并通过 tail_len 告知帧尾占用的字节数（1 或 2）。
 *
 * 参数：
 *   hvofa     : VOFA 句柄
 *   start_idx : 从当前“可用数据起点”算起的偏移位置
 *   tail_len  : 输出参数，若检测到帧尾则写入帧尾长度
 */
static bool VOFA_CheckPacketTail(VOFA_HandleTypeDef *hvofa,
                                 uint16_t start_idx,
                                 uint16_t *tail_len) {
    uint8_t first_char = VOFA_RingBufferPeek(hvofa, start_idx);
    uint8_t second_char;

    if (first_char == '\r') {
        // 可能是 "\r\n" 或者单独的 '\r'
        if (start_idx + 1 < VOFA_RingBufferAvailable(hvofa)) {
            second_char = VOFA_RingBufferPeek(hvofa, (uint16_t)(start_idx + 1));
            if (second_char == '\n') {
                *tail_len = 2;
                return true;
            }
        }
        *tail_len = 1;
        return true;
    } else if (first_char == '\n') {
        // 可能是 "\n\r" 或者单独的 '\n'
        if (start_idx + 1 < VOFA_RingBufferAvailable(hvofa)) {
            second_char = VOFA_RingBufferPeek(hvofa, (uint16_t)(start_idx + 1));
            if (second_char == '\r') {
                *tail_len = 2;
                return true;
            }
        }
        *tail_len = 1;
        return true;
    }
    return false;
}

/*==================== 从环形缓冲区提取完整一帧 ====================*/
/*
 * VOFA_ExtractFrameFromBuffer:
 *   - 在环形缓冲区当前可用数据中，查找第一个“帧尾”（\r/\n/...）。
 *   - 若找到：
 *       1) 计算这一帧的数据长度 frame_len（不含帧尾）
 *       2) 若太长则丢弃整帧；若长度为 0 则丢弃帧尾
 *       3) 否则逐字节读出到 frame_buf 中，设置 frame_len、new_frame_flag 等。
 *   - 若未找到任何帧尾，则返回 false 表示暂无完整帧。
 */
static bool VOFA_ExtractFrameFromBuffer(VOFA_HandleTypeDef *hvofa) {
    uint16_t available = VOFA_RingBufferAvailable(hvofa);
    uint16_t frame_start;
    uint16_t tail_len = 0;

    // 从当前已有的数据中，依次尝试每一个 offset 是否为帧尾
    for (frame_start = 0; frame_start < available; frame_start++) {
        if (VOFA_CheckPacketTail(hvofa, frame_start, &tail_len)) {
            uint16_t frame_len = frame_start;  // 帧数据长度（不含尾）

            // 处理“空帧”情况：只有帧尾，没有数据
            if (frame_len == 0) {
                for (uint16_t i = 0; i < tail_len; i++) {
                    VOFA_RingBufferRead(hvofa);  // 丢弃帧尾
                }
                return false;
            }

            // 若这一帧数据长度超过上限，则整帧丢弃
            if (frame_len > VOFA_FRAME_BUF_SIZE) {
                for (uint16_t i = 0; i < frame_len + tail_len; i++) {
                    VOFA_RingBufferRead(hvofa);  // 丢弃整帧（含尾）
                }
                return false;
            }

            // 读取帧数据到 frame_buf
            hvofa->frame_len = 0;
            for (uint16_t i = 0; i < frame_len; i++) {
                hvofa->frame_buf[hvofa->frame_len++] = VOFA_RingBufferRead(hvofa);
            }

            // 再把帧尾字节也从环形缓冲区丢掉
            for (uint16_t i = 0; i < tail_len; i++) {
                VOFA_RingBufferRead(hvofa);
            }

            // 更新状态，表示有一帧新数据可以处理
            hvofa->frame_count++;
            hvofa->new_frame_flag = true;
            return true;
        }
    }
    return false;  // 未找到完整帧
}

/*===============================================================
 *  VOFA_Init：初始化 VOFA 句柄
 *===============================================================*/

void VOFA_Init(VOFA_HandleTypeDef *hvofa,
               void *serial_handle,
               VOFA_SerialSendFunc send_cb,
               VOFA_FormatEnum init_fmt,
               VOFA_BaudRateEnum init_baud) {
    if (hvofa == NULL) return;

    /* 绑定串口句柄和发送回调 */
    hvofa->serial_handle = serial_handle;
    hvofa->send_callback = send_cb;

    /* 设置初始数据格式和波特率（非法值则回退到默认） */
    hvofa->fmt  = (init_fmt  < VOFA_FMT_MAX)  ? init_fmt  : VOFA_FMT_RAWDATA;
    hvofa->baud = (init_baud < VOFA_BAUD_MAX) ? init_baud : VOFA_BAUD_115200;

    /* 初始化环形缓冲区相关变量 */
    hvofa->rx_read_idx     = 0;
    hvofa->rx_write_idx    = 0;
    hvofa->rx_overflow_cnt = 0;
    hvofa->rx_total_bytes  = 0;
    memset(hvofa->rx_ring_buf, 0, VOFA_RING_BUF_SIZE);

    /* 初始化帧缓冲区相关变量 */
    hvofa->frame_len      = 0;
    hvofa->frame_count    = 0;
    hvofa->new_frame_flag = false;
    memset(hvofa->frame_buf, 0, VOFA_FRAME_BUF_SIZE);

    /* 处理状态 */
    hvofa->is_processing = false;
}

/*===============================================================
 *  VOFA_SendData：按三种格式之一打包并发送一帧数据
 *===============================================================*/

bool VOFA_SendData(VOFA_HandleTypeDef *hvofa,
                   void *data,
                   uint8_t ch_cnt,
                   VOFA_Data_TypeDef_Enum *data_types) {
    if (hvofa == NULL || data == NULL || ch_cnt == 0 || data_types == NULL) {
        return false;
    }
    if (hvofa->send_callback == NULL) return false;

    bool sendSucess = true;
    uint8_t  send_buf[256] = {0};        // FireWater/字符串格式使用的缓冲区
    uint16_t send_len = 0;
    float    float_buf[16] = {0.0f};     // JustFloat 格式中临时存放 float 的缓冲区

    switch (hvofa->fmt) {

    /*---------------- FireWater 文本格式 ----------------*/
    /*
     * 帧格式："v0,v1,v2,...,vN\n"
     * 每个通道用 snprintf 转成字符串，中间逗号分隔，最后加 '\n'。
     */
    case VOFA_FMT_FIREFIREWATER:
        for (uint8_t i = 0; i < ch_cnt; i++) {
            switch (data_types[i]) {
            case VOFA_DATA_INT:
                send_len += (uint16_t)snprintf((char*)send_buf + send_len,
                                               sizeof(send_buf) - send_len,
                                               "%d", ((int32_t*)data)[i]);
                break;
            case VOFA_DATA_UINT:
                send_len += (uint16_t)snprintf((char*)send_buf + send_len,
                                               sizeof(send_buf) - send_len,
                                               "%u", ((uint32_t*)data)[i]);
                break;
            case VOFA_DATA_FLOAT:
                send_len += (uint16_t)snprintf((char*)send_buf + send_len,
                                               sizeof(send_buf) - send_len,
                                               "%.3f", ((float*)data)[i]);
                break;
            case VOFA_DATA_CHAR:
                send_len += (uint16_t)snprintf((char*)send_buf + send_len,
                                               sizeof(send_buf) - send_len,
                                               "%c", ((int8_t*)data)[i]);
                break;
            case VOFA_DATA_UCHAR:
                send_len += (uint16_t)snprintf((char*)send_buf + send_len,
                                               sizeof(send_buf) - send_len,
                                               "%u", ((uint8_t*)data)[i]);
                break;
            case VOFA_DATA_SHORT:
                send_len += (uint16_t)snprintf((char*)send_buf + send_len,
                                               sizeof(send_buf) - send_len,
                                               "%d", ((int16_t*)data)[i]);
                break;
            case VOFA_DATA_USHORT:
                send_len += (uint16_t)snprintf((char*)send_buf + send_len,
                                               sizeof(send_buf) - send_len,
                                               "%u", ((uint16_t*)data)[i]);
                break;
            default:
                break;
            }

            // 非最后一个通道后面加逗号
            if (i < ch_cnt - 1) {
                send_len += (uint16_t)snprintf((char*)send_buf + send_len,
                                               sizeof(send_buf) - send_len,
                                               ",");
            }
        }

        // 最后加换行，表示一帧结束
        send_len += (uint16_t)snprintf((char*)send_buf + send_len,
                                       sizeof(send_buf) - send_len,
                                       "\n");

        // 调用上层提供的 send_callback 实际发送
        sendSucess = hvofa->send_callback(hvofa->serial_handle,
                                          send_buf, send_len);
        break;

    /*---------------- JustFloat 二进制格式 ----------------*/
    /*
     * 帧格式：
     *   [ ch0(float,4B) ][ ch1(float,4B) ] ... [ chN-1(float,4B) ][ 00 00 80 7F ]
     * 注意：
     *   - float 在 STM32(ARM) 上是小端存储，内存布局即为发送字节序。
     *   - 所有通道最终都会被转换成 float 存在 float_buf[] 中。
     */
    case VOFA_FMT_JUSTFLOAT:
        if (ch_cnt > 16) {
            sendSucess = false;
            break;
        }

        // 把各种类型统一转换成 float 存进 float_buf
        for (uint8_t i = 0; i < ch_cnt; i++) {
            switch (data_types[i]) {
            case VOFA_DATA_INT:    float_buf[i] = (float)((int32_t*)data)[i];   break;
            case VOFA_DATA_UINT:   float_buf[i] = (float)((uint32_t*)data)[i];  break;
            case VOFA_DATA_FLOAT:  float_buf[i] = ((float*)data)[i];            break;
            case VOFA_DATA_CHAR:   float_buf[i] = (float)((int8_t*)data)[i];    break;
            case VOFA_DATA_UCHAR:  float_buf[i] = (float)((uint8_t*)data)[i];   break;
            case VOFA_DATA_SHORT:  float_buf[i] = (float)((int16_t*)data)[i];   break;
            case VOFA_DATA_USHORT: float_buf[i] = (float)((uint16_t*)data)[i];  break;
            default: break;
            }
        }

        // 先发送 float 数组的原始二进制（N*4 字节）
        sendSucess = hvofa->send_callback(hvofa->serial_handle,
                                          (uint8_t*)float_buf,
                                          (uint16_t)(ch_cnt * sizeof(float)));
        // 再追加发送 4 字节帧尾
        if (sendSucess) {
            sendSucess = hvofa->send_callback(hvofa->serial_handle,
                                              VOFA_JUSTFLOAT_TAIL,
                                              VOFA_JUSTFLOAT_TAIL_LEN);
        }
        break;

    /*---------------- RawData 原始字节格式 ----------------*/
    /*
     * 不做任何协议打包，直接按原始类型的字节序发出去。
     * 注意：
     *   - 这里的实现假设 data 中的各通道数据是“按类型大小顺序紧挨着”排放的；
     *   - 如果你混用了不同长度的类型，要特别注意偏移计算是否符合实际内存布局。
     */
    case VOFA_FMT_RAWDATA: {
        uint16_t offset = 0;  // 累加偏移，正确处理混合类型的通道
        for (uint8_t i = 0; i < ch_cnt; i++) {
            uint8_t type_size = 0;
            switch (data_types[i]) {
            case VOFA_DATA_INT:    type_size = sizeof(int32_t); break;
            case VOFA_DATA_UINT:   type_size = sizeof(uint32_t); break;
            case VOFA_DATA_FLOAT:  type_size = sizeof(float);    break;
            case VOFA_DATA_CHAR:   type_size = sizeof(int8_t);   break;
            case VOFA_DATA_UCHAR:  type_size = sizeof(uint8_t);  break;
            case VOFA_DATA_SHORT:  type_size = sizeof(int16_t);  break;
            case VOFA_DATA_USHORT: type_size = sizeof(uint16_t); break;
            default: type_size = 0; break;
            }
            if (type_size == 0) continue;

            // 按累加偏移计算第 i 个通道在 data 中的起始地址
            uint8_t *data_addr = (uint8_t*)data + offset;
            sendSucess = hvofa->send_callback(hvofa->serial_handle,
                                              data_addr, type_size);
            if (!sendSucess) break;

            offset += type_size;  // 累加，为下一个通道准备正确的偏移
        }
        break;
    }

    default:
        sendSucess = false;
        break;
    }

    return sendSucess;
}

/*===============================================================
 *  VOFA_RxCallback：中断里把单字节写入环形缓冲区
 *===============================================================*/

void VOFA_RxCallback(VOFA_HandleTypeDef *hvofa, uint8_t data) {
    if (hvofa == NULL) return;
    VOFA_RingBufferWrite(hvofa, data);
}

/*===============================================================
 *  VOFA_Poll：主循环轮询，从环形缓冲区提取帧并处理
 *===============================================================*/

void VOFA_Poll(VOFA_HandleTypeDef *hvofa) {
    if (hvofa == NULL) return;

    // 如果环形缓冲区中有数据，则尝试提取一帧
    if (VOFA_RingBufferAvailable(hvofa) != 0) {
        if (VOFA_ExtractFrameFromBuffer(hvofa)) {
            // 提取到完整帧后，进入命令处理
            VOFA_ProcessCmd(hvofa);
        }
    }
}

/*===============================================================
 *  VOFA_ProcessCmd：简单命令解析（你可以按需扩展）
 *===============================================================*/
/*
 * 当前默认的协议（示例）：
 *   frame_buf[0] 用于标识命令类别，例如：
 *     0x01: PID 相关参数命令
 *     0x04: LED 亮度等其他命令
 *
 *   对于 0x01:
 *     frame_buf[1] = 'A'     --> 设置 KP：后面跟一个浮点数
 *     frame_buf[1] = 0x03    --> 设置转速：后面跟一个 int32 整数
 *
 *   对于 0x04:
 *     frame_buf[1] = 0x05    --> 设置 LED 亮度：后面跟一个 uint32 整数
 *
 * 这些具体内容只是示例，实际项目可根据自己需求修改。
 */

void VOFA_ProcessCmd(VOFA_HandleTypeDef *hvofa) {
    if (hvofa == NULL) return;

    // 若当前没有新帧，或已经在处理中，则直接返回
    if (!hvofa->new_frame_flag || hvofa->is_processing) {
        return;
    }

    hvofa->is_processing = true;

    // 给 frame_buf 的末尾补上 '\0'，方便用 sscanf / printf 作为 C 字符串处理
    if (hvofa->frame_len < VOFA_FRAME_BUF_SIZE) {
        hvofa->frame_buf[hvofa->frame_len] = '\0';
    }

    switch (hvofa->frame_buf[0]) {
    case 0x01:  /* 一类命令：比如 PID 等控制参数 */
        switch (hvofa->frame_buf[1]) {
        case 'A': {  // 0x01 'A'：设置 KP
            float kp_val = 0.0f;
            // 从 frame_buf[2] 开始解析一个浮点数
            if (sscanf((char*)&hvofa->frame_buf[2], "%f", &kp_val) == 1) {
                char ack_buf[32] = {0};
                int len = snprintf(ack_buf, sizeof(ack_buf),
                                   "KP set to %.2f\r\n", kp_val);
                if (len > 0 && hvofa->send_callback) {
                    hvofa->send_callback(hvofa->serial_handle,
                                         (uint8_t*)ack_buf, (uint16_t)len);
                }
                // 这里你可以把 kp_val 存到全局 PID 参数里
            }
            break;
        }
        case 0x03: {  // 0x01 0x03：设置速度（示例）
            int32_t speed_val = 0;
            if (sscanf((char*)&hvofa->frame_buf[2], "%d", &speed_val) == 1) {
                char ack_buf[32] = {0};
                int len = snprintf(ack_buf, sizeof(ack_buf),
                                   "Speed set to %d\r\n", speed_val);
                if (len > 0 && hvofa->send_callback) {
                    hvofa->send_callback(hvofa->serial_handle,
                                         (uint8_t*)ack_buf, (uint16_t)len);
                }
                // 把 speed_val 存到全局变量，作为电机转速目标等
            }
            break;
        }
        }
        break;

    case 0x04:  /* 另一类命令：比如 LED 亮度 等 */
        switch (hvofa->frame_buf[1]) {
        case 0x05: {  // 0x04 0x05：设置 LED 亮度
            uint32_t led_bright = 0;
            if (sscanf((char*)&hvofa->frame_buf[2], "%u", &led_bright) == 1) {
                char ack_buf[32] = {0};
                int len = snprintf(ack_buf, sizeof(ack_buf),
                                   "LED bright set to %u\r\n", led_bright);
                if (len > 0 && hvofa->send_callback) {
                    hvofa->send_callback(hvofa->serial_handle,
                                         (uint8_t*)ack_buf, (uint16_t)len);
                }
                // 根据 led_bright 调整实际硬件亮度
            }
            break;
        }
        }
        break;
    }

    /* 清理帧状态，以便下一次使用 */
    hvofa->new_frame_flag = false;
    hvofa->frame_len      = 0;
    if (hvofa->frame_count > 0) {
        hvofa->frame_count--;
    }
    memset(hvofa->frame_buf, 0, VOFA_FRAME_BUF_SIZE);

    hvofa->is_processing = false;
}

