#ifndef EPD_GDEY029T94_H
#define EPD_GDEY029T94_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_GDEY029T94_WIDTH_PIXELS        128U
#define EPD_GDEY029T94_HEIGHT_PIXELS       296U
#define EPD_GDEY029T94_BYTES_PER_ROW       (EPD_GDEY029T94_WIDTH_PIXELS / 8U)
#define EPD_GDEY029T94_FRAME_BUFFER_SIZE   \
    (EPD_GDEY029T94_BYTES_PER_ROW * EPD_GDEY029T94_HEIGHT_PIXELS)
#define EPD_GDEY029T94_X_ALIGNMENT_PIXELS  8U
#define EPD_GDEY029T94_GRAY4_SIZE (32U * EPD_GDEY029T94_HEIGHT_PIXELS)
#define EPD_GDEY029T94_MAX_PARTIAL_WINDOWS 5U

typedef enum {
    EPD_PROFILE_BW_296 = 0,
    EPD_PROFILE_BW_180,
    EPD_PROFILE_GUI_342,
    EPD_PROFILE_GRAY4
} epd_gdey029t94_profile_t;

/*
 * 原厂演示在 5 次局刷后清一次残影。时钟每秒局刷时会造成明显整屏闪烁，
 * 因此产品默认放宽到 120 次（通常约两分钟）；可由构建参数按实测残影覆盖。
 * 这是成功快刷/局刷的次数，不是毫秒时间；首帧建立 0x24/0x26 背景的全刷不计入。
 */
#ifndef EPD_GDEY029T94_CLEANUP_INTERVAL
#define EPD_GDEY029T94_CLEANUP_INTERVAL    120U
#endif

typedef enum {
    EPD_GDEY029T94_OK = 0,
    EPD_GDEY029T94_ERROR_ARGUMENT = -1,
    EPD_GDEY029T94_ERROR_IO = -2,
    EPD_GDEY029T94_ERROR_BUSY_TIMEOUT = -3,
    EPD_GDEY029T94_ERROR_STATE = -4,
    EPD_GDEY029T94_ERROR_BUFFER_SIZE = -5,
    EPD_GDEY029T94_ERROR_GEOMETRY = -6,
    EPD_GDEY029T94_ERROR_ALIGNMENT = -7
} epd_gdey029t94_status_t;

typedef struct {
    /* 使用帧缓冲区左上角坐标；x 和 width 必须按 8 像素对齐。 */
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} epd_gdey029t94_rect_t;

typedef struct {
    epd_gdey029t94_rect_t window;
    const uint8_t *image;
    uint32_t image_length;
} epd_gdey029t94_partial_item_t;

typedef struct {
    uint32_t full_refreshes;
    uint32_t fast_refreshes;
    uint32_t partial_refreshes;
    uint32_t refreshes_since_full;
} epd_gdey029t94_refresh_stats_t;

typedef struct {
    /* 硬件操作由平台适配层注入，使控制器协议不依赖 STM32 HAL 和具体引脚名。 */
    int32_t (*spi_write)(void *handle,
                         const uint8_t *data,
                         uint32_t length,
                         uint32_t timeout_ms);
    int32_t (*read_busy)(void *handle, uint8_t *busy_level);
    void (*set_cs)(void *handle, uint8_t level);
    void (*set_dc)(void *handle, uint8_t level);
    void (*set_reset)(void *handle, uint8_t level);
    void (*delay_ms)(void *handle, uint32_t delay_ms);
    uint32_t (*get_tick_ms)(void *handle);
    void *handle;
} epd_gdey029t94_io_t;

typedef struct {
    /* 控制器状态集中保存在设备对象中，不放到应用层。 */
    epd_gdey029t94_io_t io;
    epd_gdey029t94_profile_t profile;
    uint8_t initialized;
    uint8_t sleeping;
    uint8_t fast_initialized;
    uint8_t background_valid;
    uint32_t refreshes_since_full;
    uint32_t full_refreshes;
    uint32_t fast_refreshes;
    uint32_t partial_refreshes;
    /* 保存控制器背景/当前图像，用于多次局刷后执行计划内的全刷。 */
    uint8_t background_image[EPD_GDEY029T94_FRAME_BUFFER_SIZE];
} epd_gdey029t94_t;

epd_gdey029t94_status_t epd_gdey029t94_init(
    epd_gdey029t94_t *device,
    const epd_gdey029t94_io_t *io);

/* Zero-initialize the object before its first profile init. Cumulative stats
   survive profile changes; session state and cleanup count do not. */
epd_gdey029t94_status_t epd_gdey029t94_init_profile(
    epd_gdey029t94_t *device, const epd_gdey029t94_io_t *io,
    epd_gdey029t94_profile_t profile);
epd_gdey029t94_status_t epd_gdey029t94_profile_refresh(
    epd_gdey029t94_t *device, const uint8_t *image, uint32_t image_length);
epd_gdey029t94_status_t epd_gdey029t94_gray4_refresh(
    epd_gdey029t94_t *device, const uint8_t *image, uint32_t image_length);
/* Low/high plane, including vendor output inversion. */
uint8_t epd_gdey029t94_gray4_encode_pair(uint8_t first, uint8_t second,
                                       uint8_t plane);
/* Validate the whole batch before IO. Windows may not overlap; source bytes
   may not alias the device object. One reset and one activation per batch. */
epd_gdey029t94_status_t epd_gdey029t94_partial_refresh_batch(
    epd_gdey029t94_t *device,
    const epd_gdey029t94_partial_item_t *items, uint8_t count);

/* 全刷同时同步旧图 RAM，并清零快刷/局刷共用的残影计数器。 */
epd_gdey029t94_status_t epd_gdey029t94_full_refresh(
    epd_gdey029t94_t *device,
    const uint8_t *image,
    uint32_t image_length);

/* 装载原厂快刷温度参数；当前状态未准备好时，fast_refresh 会自动调用。 */
epd_gdey029t94_status_t epd_gdey029t94_fast_init(
    epd_gdey029t94_t *device);

/* 使用快刷波形刷新完整的 4736 字节图像，然后同步旧图 RAM，保证后续可局刷。 */
epd_gdey029t94_status_t epd_gdey029t94_fast_refresh(
    epd_gdey029t94_t *device,
    const uint8_t *image,
    uint32_t image_length);

/* 将完整图像同时写入控制器 RAM 0x24 和旧图 RAM 0x26；局刷前必须先建立背景。 */
epd_gdey029t94_status_t epd_gdey029t94_set_background(
    epd_gdey029t94_t *device,
    const uint8_t *image,
    uint32_t image_length);

/* 图像参数是窗口行紧密排列的数据，不包含整屏行跨度，不得与设备对象别名；达到
   EPD_GDEY029T94_CLEANUP_INTERVAL 次成功的快刷/局刷后，使用驱动保存的完整图像
   执行一次全刷，以减少残影累积。 */
epd_gdey029t94_status_t epd_gdey029t94_partial_refresh(
    epd_gdey029t94_t *device,
    const epd_gdey029t94_rect_t *window,
    const uint8_t *image,
    uint32_t image_length);

/* 返回累计刷新次数以及当前共用的残影计数。 */
epd_gdey029t94_status_t epd_gdey029t94_get_refresh_stats(
    const epd_gdey029t94_t *device,
    epd_gdey029t94_refresh_stats_t *stats);

epd_gdey029t94_status_t epd_gdey029t94_sleep(
    epd_gdey029t94_t *device);

#ifdef __cplusplus
}
#endif

#endif
