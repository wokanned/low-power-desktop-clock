#ifndef EPD_VENDOR_ASSETS_H
#define EPD_VENDOR_ASSETS_H
#include <stdint.h>
/* Source: S-GDEY029T94-FP4G-20250210/HARDWARE/EPD/Ap_29demo.h.
   Unmodified vendor byte order. BW: 128x296, 1bpp MSB-first, 16 bytes/row.
   GRAY: 128x296, 2bpp MSB-pair-first, 32 bytes/row.
   Digit patches: native 64x32, 1bpp, 8 bytes/row. */
#define EPD_VENDOR_BW_SIZE 4736U
#define EPD_VENDOR_GRAY_SIZE 9472U
#define EPD_VENDOR_PATCH_SIZE 256U
extern const unsigned char Num[10][256];
extern const unsigned char gImage_numdot[256];
extern const unsigned char gImage_white[256];
extern const unsigned char gImage_basemap[4736];
extern const unsigned char gImage_1[4736];
extern const unsigned char gImage_2[4736];
extern const unsigned char gImage_p1[4736];
extern const unsigned char gImage_p2[4736];
extern const unsigned char gImage_p3[4736];
extern const unsigned char gImage_p4[4736];
extern const unsigned char gImage_p5[4736];
extern const unsigned char gImage_4G1[9472];
#endif
