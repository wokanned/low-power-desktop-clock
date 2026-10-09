#ifndef EPD_FONTS_CN_H
#define EPD_FONTS_CN_H
#include <stdint.h>

/* Vendor fonts keep 164-byte padded entries, including the smaller font. */
typedef struct {
    uint8_t index[2];
    uint8_t matrix[164];
} CH_CN;
typedef struct {
    const CH_CN *table;
    uint16_t size;
    uint16_t ASCII_Width;
    uint16_t Width;
    uint16_t Height;
} cFONT;
extern const CH_CN Font12CN_Table[6];
extern const CH_CN Font24CN_Table[6];
extern const cFONT Font12CN;
extern const cFONT Font24CN;
#endif
