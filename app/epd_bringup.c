#include "epd_bringup.h"
#include "epd_clock_view.h"
#include "clock_service.h"
#include "clock_schedule.h"
#include "../platform/stm32_gdey029t94.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include <string.h>

#define EPD_CLOCK_POLL_MS 200U

/* All buffers and controller state belong exclusively to displayTask. */
static epd_gdey029t94_t s_device;
static stm32_gdey029t94_context_t s_platform;
static epd_gdey029t94_io_t s_io;
static uint8_t s_frame[EPD_GDEY029T94_GRAY4_SIZE];
static clock_view_model_t s_model;
static clock_schedule_t s_schedule;
static uint8_t s_pending;
static uint32_t s_completed_frames;
static volatile epd_gdey029t94_refresh_stats_t s_refresh_stats;
static volatile epd_gdey029t94_status_t s_last_status = EPD_GDEY029T94_ERROR_STATE;
#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF
static uint8_t s_demo_started;
#endif

static uint32_t s_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * (1000U / configTICK_RATE_HZ));
}

static epd_gdey029t94_status_t s_bind(void)
{
    epd_gdey029t94_status_t status;
    memset(&s_io, 0, sizeof(s_io));
    status = stm32_gdey029t94_make_io(&s_io, &s_platform);
    if (status == EPD_GDEY029T94_OK) { status = stm32_gdey029t94_prepare(&s_platform); }
    return status;
}

static epd_gdey029t94_status_t s_submit_gray_frame(void)
{
    epd_gdey029t94_status_t status;
    status = epd_gdey029t94_init_profile(&s_device, &s_io, EPD_PROFILE_GRAY4);
    if (status == EPD_GDEY029T94_OK) {
        status = epd_gdey029t94_gray4_refresh(&s_device, s_frame, sizeof(s_frame));
    }
    if (status != EPD_GDEY029T94_OK) { s_last_status = status; return status; }
    /* BUSY has completed: commit once, even when the following sleep fails. */
    clock_schedule_commit(&s_schedule, &s_model, s_now_ms());
    s_pending = 0U;
    s_completed_frames++;
    s_refresh_stats.full_refreshes = s_completed_frames;
    s_refresh_stats.fast_refreshes = 0U;
    s_refresh_stats.partial_refreshes = 0U;
    s_refresh_stats.refreshes_since_full = 0U;
#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF
    if (epd_vendor_demo_state.status != EPD_GDEY029T94_OK) {
        epd_vendor_demo_state.recovered_to_clock = 1U;
    }
#endif
    s_last_status = epd_gdey029t94_sleep(&s_device);
    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t epd_bringup_run_once(void)
{
    epd_gdey029t94_status_t status;
    clock_service_get_model(&s_model);
    if (s_model.startup_ready == 0U) { return EPD_GDEY029T94_ERROR_STATE; }
    s_model.memory_mode = s_schedule.next_memory;
    if (!epd_clock_view_draw_gray4(s_frame, sizeof(s_frame), &s_model)) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    status = s_bind();
    if (status == EPD_GDEY029T94_OK) { status = s_submit_gray_frame(); }
    return status;
}

epd_gdey029t94_status_t epd_bringup_run_clock(void)
{
    epd_gdey029t94_status_t status;
    status = s_bind();
    if (status != EPD_GDEY029T94_OK) { s_last_status = status; return status; }
#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF
    if (s_demo_started == 0U) {
        s_demo_started = 1U;
        status = epd_vendor_demo_run(&s_device, &s_io, s_frame);
        if (status != EPD_GDEY029T94_OK) { s_last_status = status; return status; }
    }
#endif
#if EPD_GUI_DEMO || EPD_CLOCK_DIAGNOSTIC_PATTERN || EPD_CLOCK_DIAGNOSTIC_FORCE_FULL
    status = epd_gdey029t94_init_profile(&s_device, &s_io, EPD_PROFILE_BW_296);
    if (status != EPD_GDEY029T94_OK) { s_last_status = status; return status; }
    for (;;) {
#if EPD_GUI_DEMO
        epd_clock_view_draw_demo(s_frame);
#elif EPD_CLOCK_DIAGNOSTIC_PATTERN
        epd_clock_view_draw_orientation(s_frame);
#else
        clock_service_get_model(&s_model);
        epd_clock_view_draw(s_frame, s_model.calendar.hour,
            s_model.calendar.minute, s_model.calendar.second);
#endif
        status = epd_gdey029t94_full_refresh(&s_device, s_frame,
            EPD_GDEY029T94_FRAME_BUFFER_SIZE);
        s_last_status = status;
        if (status != EPD_GDEY029T94_OK) { return status; }
        s_completed_frames++;
#if !EPD_CLOCK_DIAGNOSTIC_FORCE_FULL
        for (;;) { (void)osDelay(pdMS_TO_TICKS(1000U)); }
#else
        (void)osDelay(pdMS_TO_TICKS(1000U));
#endif
    }
#else
    if (s_pending != 0U) {
        /* A failed frame was never committed. Retry the newest state while
           retaining the same memory phase and bounded transport budget. */
        clock_service_get_model(&s_model);
        s_model.memory_mode = s_schedule.next_memory;
        if (!epd_clock_view_draw_gray4(s_frame, sizeof(s_frame), &s_model)) {
            s_last_status = EPD_GDEY029T94_ERROR_ARGUMENT;
            return s_last_status;
        }
    }
    for (;;) {
        if (s_pending == 0U) {
            clock_service_get_model(&s_model);
            if (clock_schedule_due(&s_schedule, &s_model, s_now_ms()) != 0U) {
                s_model.memory_mode = s_schedule.next_memory;
                if (!epd_clock_view_draw_gray4(s_frame, sizeof(s_frame), &s_model)) {
                    s_last_status = EPD_GDEY029T94_ERROR_ARGUMENT;
                    return s_last_status;
                }
                s_pending = 1U;
            }
        }
        if (s_pending != 0U) {
            status = s_submit_gray_frame();
            if (status != EPD_GDEY029T94_OK) { return status; }
        }
        (void)osDelay(pdMS_TO_TICKS(EPD_CLOCK_POLL_MS));
    }
#endif
}

epd_gdey029t94_status_t epd_bringup_get_last_status(void) { return s_last_status; }
uint32_t epd_bringup_get_completed_frames(void) { return s_completed_frames; }
