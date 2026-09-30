#include "epd_bringup.h"

#include "../Core/Inc/rtc.h"
#include "../platform/stm32_gdey029t94.h"

#include <string.h>

/*
 * 应用层：
 * - 负责时钟业务和帧缓冲区绘制策略。
 * - 通过设备层公开接口操作墨水屏。
 * - 不直接访问 SPI 或 GPIO；硬件回调由 platform/stm32_gdey029t94.c 提供。
 */
#define EPD_CLOCK_POLL_MS       200U
#define EPD_CLOCK_LOGICAL_WIDTH  EPD_GDEY029T94_HEIGHT_PIXELS
#define EPD_CLOCK_LOGICAL_HEIGHT EPD_GDEY029T94_WIDTH_PIXELS
#define EPD_CLOCK_SCALE         7U
#define EPD_CLOCK_GLYPH_W       5U
#define EPD_CLOCK_GLYPH_H       7U
#define EPD_CLOCK_DIGIT_CELL    6U
#define EPD_CLOCK_COLON_CELL    2U
#define EPD_CLOCK_TOTAL_COLS    ((EPD_CLOCK_DIGIT_CELL * 6U) + (EPD_CLOCK_COLON_CELL * 2U))
#define EPD_CLOCK_X0            ((EPD_CLOCK_LOGICAL_WIDTH - (EPD_CLOCK_TOTAL_COLS * EPD_CLOCK_SCALE)) / 2U)
#define EPD_CLOCK_Y0            ((EPD_CLOCK_LOGICAL_HEIGHT - (EPD_CLOCK_GLYPH_H * EPD_CLOCK_SCALE)) / 2U)
#define EPD_TEST_WHITE_BYTE     0xFFU

/* 以下窗口使用原生帧缓冲区坐标；横屏文字经过 90 度旋转后对应一条窄窗口。 */
#define EPD_CLOCK_WINDOW_X        32U
#define EPD_CLOCK_WINDOW_WIDTH    56U
#define EPD_SECONDS_WINDOW_Y      8U
#define EPD_SECONDS_WINDOW_HEIGHT 84U
#define EPD_CLOCK_WINDOW_BUFFER_SIZE \
    ((EPD_CLOCK_WINDOW_WIDTH / 8U) * EPD_SECONDS_WINDOW_HEIGHT)

static epd_gdey029t94_t epd_device;
static stm32_gdey029t94_context_t epd_platform_context;
static uint8_t epd_frame_buffer[EPD_GDEY029T94_FRAME_BUFFER_SIZE];
static uint8_t epd_window_buffer[EPD_CLOCK_WINDOW_BUFFER_SIZE];
/* 使用 volatile，便于在调试器中直接观察刷新计数。 */
static volatile epd_gdey029t94_refresh_stats_t epd_refresh_stats;
static volatile epd_gdey029t94_status_t epd_last_status =
    EPD_GDEY029T94_ERROR_STATE;

/* 按控制器原生的 128 x 296 内存布局写入一个像素。 */
static void epd_set_physical_pixel(uint16_t x, uint16_t y, uint8_t black)
{
    uint32_t byte_index;
    uint8_t bit_mask;

    if ((x >= EPD_GDEY029T94_WIDTH_PIXELS) ||
        (y >= EPD_GDEY029T94_HEIGHT_PIXELS)) {
        return;
    }

    byte_index = ((uint32_t)y * EPD_GDEY029T94_BYTES_PER_ROW) +
                 ((uint32_t)x / 8U);
    bit_mask = (uint8_t)(0x80U >> (x % 8U));

    if (black != 0U) {
        epd_frame_buffer[byte_index] &= (uint8_t)(~bit_mask);
    } else {
        epd_frame_buffer[byte_index] |= bit_mask;
    }
}

static void epd_set_logical_pixel(uint16_t x, uint16_t y, uint8_t black)
{
    if ((x >= EPD_CLOCK_LOGICAL_WIDTH) ||
        (y >= EPD_CLOCK_LOGICAL_HEIGHT)) {
        return;
    }

    /* 将横屏逻辑画布旋转到控制器的纵向 RAM 布局。 */
    epd_set_physical_pixel(y,
                           (uint16_t)(EPD_GDEY029T94_HEIGHT_PIXELS - 1U - x),
                           black);
}

static const uint8_t epd_digit_glyphs[10][EPD_CLOCK_GLYPH_H] = {
    {0x0EU, 0x11U, 0x13U, 0x15U, 0x19U, 0x11U, 0x0EU},
    {0x04U, 0x0CU, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU},
    {0x0EU, 0x11U, 0x01U, 0x02U, 0x04U, 0x08U, 0x1FU},
    {0x1EU, 0x01U, 0x01U, 0x0EU, 0x01U, 0x01U, 0x1EU},
    {0x02U, 0x06U, 0x0AU, 0x12U, 0x1FU, 0x02U, 0x02U},
    {0x1FU, 0x10U, 0x10U, 0x1EU, 0x01U, 0x01U, 0x1EU},
    {0x06U, 0x08U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x0EU},
    {0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x08U, 0x08U},
    {0x0EU, 0x11U, 0x11U, 0x0EU, 0x11U, 0x11U, 0x0EU},
    {0x0EU, 0x11U, 0x11U, 0x0FU, 0x01U, 0x02U, 0x1CU}
};

static void epd_draw_clock_glyph(uint16_t x, uint16_t y, uint8_t digit)
{
    uint16_t sx;
    uint16_t sy;
    uint8_t row;
    uint8_t column;

    if (digit > 9U) {
        return;
    }

    for (row = 0U; row < EPD_CLOCK_GLYPH_H; row++) {
        for (column = 0U; column < EPD_CLOCK_GLYPH_W; column++) {
            if ((epd_digit_glyphs[digit][row] &
                 (uint8_t)(0x10U >> column)) != 0U) {
                for (sy = 0U; sy < EPD_CLOCK_SCALE; sy++) {
                    for (sx = 0U; sx < EPD_CLOCK_SCALE; sx++) {
                        epd_set_logical_pixel((uint16_t)(x +
                                                         (column * EPD_CLOCK_SCALE) + sx),
                                              (uint16_t)(y +
                                                         (row * EPD_CLOCK_SCALE) + sy),
                                              1U);
                    }
                }
            }
        }
    }
}

static void epd_draw_clock_colon(uint16_t x, uint16_t y)
{
    uint16_t dot;
    uint16_t dot_y;

    for (dot_y = 0U; dot_y < EPD_CLOCK_SCALE; dot_y++) {
        for (dot = 0U; dot < EPD_CLOCK_SCALE; dot++) {
            epd_set_logical_pixel((uint16_t)(x + dot),
                                  (uint16_t)(y + (2U * EPD_CLOCK_SCALE) + dot_y),
                                  1U);
            epd_set_logical_pixel((uint16_t)(x + dot),
                                  (uint16_t)(y + (4U * EPD_CLOCK_SCALE) + dot_y),
                                  1U);
        }
    }
}

static void epd_draw_clock(uint8_t hours, uint8_t minutes, uint8_t seconds)
{
    uint16_t x;

    /* 设备层每次全刷都要求完整的单色图像。 */
    memset(epd_frame_buffer, EPD_TEST_WHITE_BYTE, sizeof(epd_frame_buffer));
    x = EPD_CLOCK_X0;
    epd_draw_clock_glyph(x, EPD_CLOCK_Y0, (uint8_t)(hours / 10U));
    x = (uint16_t)(x + EPD_CLOCK_DIGIT_CELL * EPD_CLOCK_SCALE);
    epd_draw_clock_glyph(x, EPD_CLOCK_Y0, (uint8_t)(hours % 10U));
    x = (uint16_t)(x + EPD_CLOCK_DIGIT_CELL * EPD_CLOCK_SCALE);
    epd_draw_clock_colon(x, EPD_CLOCK_Y0);
    x = (uint16_t)(x + EPD_CLOCK_COLON_CELL * EPD_CLOCK_SCALE);
    epd_draw_clock_glyph(x, EPD_CLOCK_Y0, (uint8_t)(minutes / 10U));
    x = (uint16_t)(x + EPD_CLOCK_DIGIT_CELL * EPD_CLOCK_SCALE);
    epd_draw_clock_glyph(x, EPD_CLOCK_Y0, (uint8_t)(minutes % 10U));
    x = (uint16_t)(x + EPD_CLOCK_DIGIT_CELL * EPD_CLOCK_SCALE);
    epd_draw_clock_colon(x, EPD_CLOCK_Y0);
    x = (uint16_t)(x + EPD_CLOCK_COLON_CELL * EPD_CLOCK_SCALE);
    epd_draw_clock_glyph(x, EPD_CLOCK_Y0, (uint8_t)(seconds / 10U));
    x = (uint16_t)(x + EPD_CLOCK_DIGIT_CELL * EPD_CLOCK_SCALE);
    epd_draw_clock_glyph(x, EPD_CLOCK_Y0, (uint8_t)(seconds % 10U));
}

static uint32_t epd_extract_window(const epd_gdey029t94_rect_t *window)
{
    uint32_t row;
    uint32_t window_bytes;
    uint32_t source_offset;

    if ((window == NULL) ||
        ((window->x % EPD_GDEY029T94_X_ALIGNMENT_PIXELS) != 0U) ||
        ((window->width % EPD_GDEY029T94_X_ALIGNMENT_PIXELS) != 0U) ||
        ((uint32_t)window->x + window->width >
         EPD_GDEY029T94_WIDTH_PIXELS) ||
        ((uint32_t)window->y + window->height >
         EPD_GDEY029T94_HEIGHT_PIXELS)) {
        return 0U;
    }

    window_bytes = window->width / EPD_GDEY029T94_X_ALIGNMENT_PIXELS;
    if ((window_bytes * window->height) > sizeof(epd_window_buffer)) {
        return 0U;
    }

    for (row = 0U; row < window->height; row++) {
        source_offset = ((uint32_t)(window->y + row) *
                         EPD_GDEY029T94_BYTES_PER_ROW) +
                        (window->x / EPD_GDEY029T94_X_ALIGNMENT_PIXELS);
        memcpy(&epd_window_buffer[row * window_bytes],
               &epd_frame_buffer[source_offset],
               window_bytes);
    }

    return window_bytes * window->height;
}

static epd_gdey029t94_status_t epd_clock_start(void)
{
    epd_gdey029t94_io_t io;
    epd_gdey029t94_status_t status;

    /* 将通用设备驱动绑定到当前板子的 SPI 和 GPIO 信号。 */
    memset(&io, 0, sizeof(io));
    status = stm32_gdey029t94_make_io(&io, &epd_platform_context);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    return epd_gdey029t94_init(&epd_device, &io);
}

static epd_gdey029t94_status_t epd_clock_read(RTC_TimeTypeDef *time)
{
    RTC_DateTypeDef date;

    if (time == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    /* 按 STM32 RTC 影子寄存器规则，读取时间后还要读取日期。 */
    if (HAL_RTC_GetTime(&hrtc, time, RTC_FORMAT_BIN) != HAL_OK) {
        return EPD_GDEY029T94_ERROR_IO;
    }
    if (HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN) != HAL_OK) {
        return EPD_GDEY029T94_ERROR_IO;
    }
    if ((time->Hours > 23U) || (time->Minutes > 59U) ||
        (time->Seconds > 59U)) {
        return EPD_GDEY029T94_ERROR_IO;
    }
    return EPD_GDEY029T94_OK;
}

static epd_gdey029t94_status_t epd_clock_refresh(
    const RTC_TimeTypeDef *time)
{
    if (time == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    /* 将绘制和传输分开，便于脱离 SPI 实现单独检查图像内容。 */
    epd_draw_clock(time->Hours, time->Minutes, time->Seconds);
    epd_last_status = epd_gdey029t94_full_refresh(
        &epd_device, epd_frame_buffer, sizeof(epd_frame_buffer));
    return epd_last_status;
}

static epd_gdey029t94_status_t epd_clock_set_background(
    const RTC_TimeTypeDef *time)
{
    if (time == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    epd_draw_clock(time->Hours, time->Minutes, time->Seconds);
    epd_last_status = epd_gdey029t94_set_background(
        &epd_device, epd_frame_buffer, sizeof(epd_frame_buffer));
    return epd_last_status;
}

static epd_gdey029t94_status_t epd_clock_partial_refresh(
    const RTC_TimeTypeDef *time)
{
    epd_gdey029t94_rect_t window;
    uint32_t window_length;

    if (time == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    epd_draw_clock(time->Hours, time->Minutes, time->Seconds);
    window.x = EPD_CLOCK_WINDOW_X;
    window.width = EPD_CLOCK_WINDOW_WIDTH;
    window.y = EPD_SECONDS_WINDOW_Y;
    window.height = EPD_SECONDS_WINDOW_HEIGHT;

    window_length = epd_extract_window(&window);
    if (window_length == 0U) {
        return EPD_GDEY029T94_ERROR_BUFFER_SIZE;
    }
    epd_last_status = epd_gdey029t94_partial_refresh(
        &epd_device, &window, epd_window_buffer, window_length);
    return epd_last_status;
}

static epd_gdey029t94_status_t epd_clock_fast_refresh(
    const RTC_TimeTypeDef *time)
{
    if (time == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    /* 分钟变化可能同时改变多个数字，因此使用整帧快刷，不拆成多个局部窗口。 */
    epd_draw_clock(time->Hours, time->Minutes, time->Seconds);
    epd_last_status = epd_gdey029t94_fast_refresh(
        &epd_device, epd_frame_buffer, sizeof(epd_frame_buffer));
    return epd_last_status;
}

static void epd_clock_update_stats(void)
{
    epd_gdey029t94_refresh_stats_t stats;

    if (epd_gdey029t94_get_refresh_stats(&epd_device, &stats) ==
        EPD_GDEY029T94_OK) {
        epd_refresh_stats.full_refreshes = stats.full_refreshes;
        epd_refresh_stats.fast_refreshes = stats.fast_refreshes;
        epd_refresh_stats.partial_refreshes = stats.partial_refreshes;
        epd_refresh_stats.refreshes_since_full = stats.refreshes_since_full;
    }
}

epd_gdey029t94_status_t epd_bringup_run_once(void)
{
    RTC_TimeTypeDef time;
    epd_gdey029t94_status_t status;

    status = epd_clock_start();
    if (status != EPD_GDEY029T94_OK) {
        epd_last_status = status;
        return status;
    }

    status = epd_clock_read(&time);
    if (status != EPD_GDEY029T94_OK) {
        epd_last_status = status;
        return status;
    }

    status = epd_clock_refresh(&time);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_gdey029t94_sleep(&epd_device);
    epd_last_status = status;

    return status;
}

epd_gdey029t94_status_t epd_bringup_run_clock(void)
{
    RTC_TimeTypeDef time;
    epd_gdey029t94_status_t status;
    uint32_t displayed_second = 0xFFFFFFFFU;
    uint32_t current_second;
    uint16_t displayed_minute = 0xFFFFU;
    uint16_t current_minute;
    uint8_t background_ready = 0U;

    status = epd_clock_start();
    if (status != EPD_GDEY029T94_OK) {
        epd_last_status = status;
        return status;
    }

    /* 保持屏幕唤醒：首帧建立背景，之后使用局刷，并按周期执行清残影全刷。 */
    for (;;) {
        status = epd_clock_read(&time);
        if (status != EPD_GDEY029T94_OK) {
            epd_last_status = status;
            return status;
        }

        current_second = ((uint32_t)time.Hours * 3600U) +
                         ((uint32_t)time.Minutes * 60U) + time.Seconds;
        current_minute = (uint16_t)(((uint16_t)time.Hours * 60U) +
                                    time.Minutes);
        if (current_second != displayed_second) {
            if (background_ready == 0U) {
                /* 局刷要求先建立同步的 0x24/0x26 背景基准。 */
                status = epd_clock_set_background(&time);
            } else if (current_minute != displayed_minute) {
                status = epd_clock_fast_refresh(&time);
            } else {
                status = epd_clock_partial_refresh(&time);
            }
            if (status != EPD_GDEY029T94_OK) {
                return status;
            }
            background_ready = 1U;
            displayed_second = current_second;
            displayed_minute = current_minute;
            epd_clock_update_stats();
        }

        HAL_Delay(EPD_CLOCK_POLL_MS);
    }
}

epd_gdey029t94_status_t epd_bringup_get_last_status(void)
{
    return epd_last_status;
}
