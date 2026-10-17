#include "vofa.h"
#include <float.h>
#include <string.h>

#if FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128
#error JustFloat requires IEEE 754 binary32
#endif
typedef char vofa_float_size_check[(sizeof(float) == 4U) ? 1 : -1];

void VOFA_Init(VOFA_HandleTypeDef *p_vofa, void *p_serial_handle,
               VOFA_SerialSendFunc send_cb, VOFA_FormatEnum init_fmt,
               VOFA_BaudRateEnum init_baud)
{
    (void)init_fmt;
    if (p_vofa == NULL) {
        return;
    }
    p_vofa->serial_handle = p_serial_handle;
    p_vofa->send_callback = send_cb;
    p_vofa->fmt = VOFA_FMT_JUSTFLOAT;
    p_vofa->baud = init_baud;
}

bool VOFA_SendData(VOFA_HandleTypeDef *p_vofa, const void *p_data,
                   uint8_t ch_cnt, const VOFA_Data_TypeDef_Enum *p_types)
{
    uint8_t frame[VOFA_MAX_CHANNELS * 4U + VOFA_JUSTFLOAT_TAIL_LEN];
    const uint8_t *p_bytes = (const uint8_t *)p_data;
    uint16_t offset = 0U;
    uint16_t frame_len;
    uint8_t i;
    uint32_t bits;
    float value;
    int32_t signed32;
    uint32_t unsigned32;
    int16_t signed16;
    uint16_t unsigned16;
    int8_t signed8;

    if (p_vofa == NULL || p_vofa->send_callback == NULL || p_data == NULL ||
        p_types == NULL || ch_cnt == 0U || ch_cnt > VOFA_MAX_CHANNELS) {
        return false;
    }
    for (i = 0U; i < ch_cnt; i++) {
        switch (p_types[i]) {
        case VOFA_DATA_INT:
            memcpy(&signed32, p_bytes + offset, sizeof(signed32));
            value = (float)signed32; offset += sizeof(signed32); break;
        case VOFA_DATA_UINT:
            memcpy(&unsigned32, p_bytes + offset, sizeof(unsigned32));
            value = (float)unsigned32; offset += sizeof(unsigned32); break;
        case VOFA_DATA_FLOAT:
            memcpy(&value, p_bytes + offset, sizeof(value));
            offset += sizeof(value); break;
        case VOFA_DATA_SHORT:
            memcpy(&signed16, p_bytes + offset, sizeof(signed16));
            value = (float)signed16; offset += sizeof(signed16); break;
        case VOFA_DATA_USHORT:
            memcpy(&unsigned16, p_bytes + offset, sizeof(unsigned16));
            value = (float)unsigned16; offset += sizeof(unsigned16); break;
        case VOFA_DATA_CHAR:
            memcpy(&signed8, p_bytes + offset, sizeof(signed8));
            value = (float)signed8; offset += sizeof(signed8); break;
        case VOFA_DATA_UCHAR:
            value = (float)p_bytes[offset++]; break;
        default:
            return false;
        }
        memcpy(&bits, &value, sizeof(bits));
        /* Infinity has the same representation as the frame delimiter. */
        if ((bits & 0x7FFFFFFFU) == 0x7F800000U) {
            return false;
        }
        frame[i * 4U] = (uint8_t)bits;
        frame[i * 4U + 1U] = (uint8_t)(bits >> 8U);
        frame[i * 4U + 2U] = (uint8_t)(bits >> 16U);
        frame[i * 4U + 3U] = (uint8_t)(bits >> 24U);
    }
    frame_len = (uint16_t)(ch_cnt * 4U);
    frame[frame_len++] = 0x00U;
    frame[frame_len++] = 0x00U;
    frame[frame_len++] = 0x80U;
    frame[frame_len++] = 0x7FU;
    return p_vofa->send_callback(p_vofa->serial_handle, frame, frame_len);
}

