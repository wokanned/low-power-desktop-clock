#include "epd_bringup.h"
#include "epd_clock_view.h"

#include "../Core/Inc/rtc.h"
#include "../platform/stm32_gdey029t94.h"
#include "FreeRTOS.h"
#include "cmsis_os.h"

#include <string.h>

/*
 * 应用层：
 * - 负责时钟业务和帧缓冲区绘制策略。
 * - 通过设备层公开接口操作墨水屏。
 * - 不直接访问 SPI 或 GPIO；硬件回调由 platform/stm32_gdey029t94.c 提供。
 */
#define EPD_CLOCK_POLL_MS 200U
#define EPD_CLOCK_WINDOW_X        EPD_CLOCK_VIEW_SECONDS_NATIVE_X
#define EPD_CLOCK_WINDOW_WIDTH    EPD_CLOCK_VIEW_SECONDS_NATIVE_WIDTH
#define EPD_SECONDS_WINDOW_Y      EPD_CLOCK_VIEW_SECONDS_NATIVE_Y
#define EPD_SECONDS_WINDOW_HEIGHT EPD_CLOCK_VIEW_SECONDS_NATIVE_HEIGHT
#define EPD_CLOCK_WINDOW_BUFFER_SIZE \
    ((EPD_CLOCK_WINDOW_WIDTH / 8U) * EPD_SECONDS_WINDOW_HEIGHT)

static epd_gdey029t94_t epd_device;
static stm32_gdey029t94_context_t epd_platform_context;
static uint8_t epd_frame_buffer[EPD_GDEY029T94_FRAME_BUFFER_SIZE];
#if !(EPD_GUI_DEMO || EPD_CLOCK_DIAGNOSTIC_PATTERN || EPD_CLOCK_DIAGNOSTIC_FORCE_FULL)
static uint8_t epd_window_buffer[EPD_CLOCK_WINDOW_BUFFER_SIZE];
#endif
/* Debugger-visible counters; drawing remains independent of transport. */
static volatile epd_gdey029t94_refresh_stats_t epd_refresh_stats;
static volatile epd_gdey029t94_status_t epd_last_status =
    EPD_GDEY029T94_ERROR_STATE;

static void epd_draw_clock(uint8_t hours, uint8_t minutes, uint8_t seconds)
{
    epd_clock_view_draw(epd_frame_buffer, hours, minutes, seconds);
}

#if !(EPD_GUI_DEMO || EPD_CLOCK_DIAGNOSTIC_PATTERN || EPD_CLOCK_DIAGNOSTIC_FORCE_FULL)
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
#endif

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
    status = stm32_gdey029t94_prepare(&epd_platform_context);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    return epd_gdey029t94_init(&epd_device, &io);
}

#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF
static epd_gdey029t94_status_t epd_run_startup_demo(void)
{
    epd_gdey029t94_io_t io;
    epd_gdey029t94_status_t status;
    status = stm32_gdey029t94_make_io(&io, &epd_platform_context);
    if (status != EPD_GDEY029T94_OK) return status;
    status = stm32_gdey029t94_prepare(&epd_platform_context);
    if (status != EPD_GDEY029T94_OK) return status;
    status = epd_vendor_demo_run(&epd_device, &io, epd_frame_buffer);
    /* Exactly one bounded recovery attempt. Preserve the failed demo stage
       and status even when the normal clock can resume. */
    if (status != EPD_GDEY029T94_OK) {
        status = epd_gdey029t94_init_profile(&epd_device, &io, EPD_PROFILE_BW_296);
        if (status == EPD_GDEY029T94_OK) epd_vendor_demo_state.recovered_to_clock = 1U;
        return status;
    }
    return epd_gdey029t94_init_profile(&epd_device, &io, EPD_PROFILE_BW_296);
}
#endif

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

#if !(EPD_GUI_DEMO || EPD_CLOCK_DIAGNOSTIC_PATTERN || EPD_CLOCK_DIAGNOSTIC_FORCE_FULL)
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

#endif

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
    epd_gdey029t94_status_t status;
#if !(EPD_GUI_DEMO || EPD_CLOCK_DIAGNOSTIC_PATTERN)
    RTC_TimeTypeDef time;
    uint32_t displayed_second = 0xFFFFFFFFU;
    uint32_t current_second;
#if !EPD_CLOCK_DIAGNOSTIC_FORCE_FULL
    uint16_t displayed_minute = 0xFFFFU;
    uint16_t current_minute;
    uint8_t background_ready = 0U;
#endif
#endif

#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF
    status = epd_run_startup_demo();
#else
    status = epd_clock_start();
#endif
    if (status != EPD_GDEY029T94_OK) {
        epd_last_status = status;
        return status;
    }

#if EPD_GUI_DEMO || EPD_CLOCK_DIAGNOSTIC_PATTERN
#if EPD_GUI_DEMO
    epd_clock_view_draw_demo(epd_frame_buffer);
#else
    epd_clock_view_draw_orientation(epd_frame_buffer);
#endif
    epd_last_status = epd_gdey029t94_full_refresh(
        &epd_device, epd_frame_buffer, sizeof(epd_frame_buffer));
    if (epd_last_status != EPD_GDEY029T94_OK) {
        return epd_last_status;
    }
    epd_clock_update_stats();
    /* A static acceptance page: one full refresh, then no screen commands. */
    for (;;) {
        (void)osDelay((uint32_t)pdMS_TO_TICKS(1000U));
    }
#else
    /* Keep the validated background / seconds / minute refresh policy. */
    for (;;) {
        status = epd_clock_read(&time);
        if (status != EPD_GDEY029T94_OK) {
            epd_last_status = status;
            return status;
        }

        current_second = ((uint32_t)time.Hours * 3600U) +
                         ((uint32_t)time.Minutes * 60U) + time.Seconds;
#if !EPD_CLOCK_DIAGNOSTIC_FORCE_FULL
        current_minute = (uint16_t)(((uint16_t)time.Hours * 60U) +
                                    time.Minutes);
#endif
        if (current_second != displayed_second) {
#if EPD_CLOCK_DIAGNOSTIC_FORCE_FULL
            status = epd_clock_refresh(&time);
#else
            if (background_ready == 0U) {
                status = epd_clock_set_background(&time);
            } else if (current_minute != displayed_minute) {
                status = epd_clock_fast_refresh(&time);
            } else {
                status = epd_clock_partial_refresh(&time);
            }
#endif
            if (status != EPD_GDEY029T94_OK) {
                return status;
            }
#if !EPD_CLOCK_DIAGNOSTIC_FORCE_FULL
            background_ready = 1U;
            displayed_minute = current_minute;
#endif
            displayed_second = current_second;
            epd_clock_update_stats();
        }
        (void)osDelay((uint32_t)pdMS_TO_TICKS(EPD_CLOCK_POLL_MS));
    }
#endif
}

epd_gdey029t94_status_t epd_bringup_get_last_status(void)
{
    return epd_last_status;
}
