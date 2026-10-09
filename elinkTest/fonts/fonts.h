#ifndef EPD_ASCII_FONTS_H
#define EPD_ASCII_FONTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vendor ASCII bitmaps: 95 printable characters, row-major and MSB-first. */
#define FONT_ASCII_FIRST 0x20U
#define FONT_ASCII_LAST  0x7EU
#define FONT_ASCII_COUNT 95U

typedef struct {
    const uint8_t *table;
    uint16_t Width;
    uint16_t Height;
} sFONT;

extern const sFONT Font8;
extern const sFONT Font12;
extern const sFONT Font16;
extern const sFONT Font20;
extern const sFONT Font24;

#ifdef __cplusplus
}
#endif

#endif
