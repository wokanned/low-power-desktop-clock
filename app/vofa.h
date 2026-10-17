#ifndef VOFA_H
#define VOFA_H

#include <stdbool.h>
#include <stdint.h>

#define VOFA_MAX_CHANNELS 16U
#define VOFA_JUSTFLOAT_TAIL_LEN 4U

typedef enum {
    VOFA_DATA_INT, VOFA_DATA_UINT, VOFA_DATA_FLOAT, VOFA_DATA_CHAR,
    VOFA_DATA_UCHAR, VOFA_DATA_SHORT, VOFA_DATA_USHORT
} VOFA_Data_TypeDef_Enum;

/* Legacy names remain source-compatible; this library only sends JustFloat. */
typedef enum {
    VOFA_FMT_FIREFIREWATER = 0, VOFA_FMT_JUSTFLOAT, VOFA_FMT_RAWDATA, VOFA_FMT_MAX
} VOFA_FormatEnum;

typedef enum { VOFA_BAUD_115200 = 115200 } VOFA_BaudRateEnum;

/* Callback consumes/copies the complete frame before returning. Not DMA-safe. */
typedef bool (*VOFA_SerialSendFunc)(void *p_serial_handle,
                                  const uint8_t *p_data, uint16_t len);
typedef struct {
    void *serial_handle;
    VOFA_SerialSendFunc send_callback;
    VOFA_FormatEnum fmt;
    VOFA_BaudRateEnum baud;
} VOFA_HandleTypeDef;

void VOFA_Init(VOFA_HandleTypeDef *p_vofa, void *p_serial_handle,
               VOFA_SerialSendFunc send_cb, VOFA_FormatEnum init_fmt,
               VOFA_BaudRateEnum init_baud);
/* Packed input channels follow types[]; memcpy permits unaligned input. */
bool VOFA_SendData(VOFA_HandleTypeDef *p_vofa, const void *p_data,
                   uint8_t ch_cnt, const VOFA_Data_TypeDef_Enum *p_types);

#endif

