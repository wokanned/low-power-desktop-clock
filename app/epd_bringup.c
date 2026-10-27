#include "epd_bringup.h"
#include "epd_clock_view.h"
#include "epd_weather_view.h"
#include "display.h"
#include "clock_service.h"
#include "clock_schedule.h"
#include "../platform/stm32_gdey029t94.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include <string.h>

#define EPD_CLOCK_POLL_MS 200U
#define EPD_CONTENT_CLOCK 1U
#define EPD_CONTENT_WEATHER 2U

/* All buffers and controller state belong exclusively to displayTask. */
static epd_gdey029t94_t s_device;
static stm32_gdey029t94_context_t s_platform;
static epd_gdey029t94_io_t s_io;
static uint8_t s_frame[EPD_GDEY029T94_GRAY4_SIZE];
#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_GRAY4_HOLD
static display_snapshot_t s_snapshot;
static clock_schedule_t s_schedule;
static uint8_t s_pending;
static uint8_t s_pending_kind;
static uint8_t s_displayed_kind;
static weather_snapshot_t s_displayed_weather;
static uint32_t s_weather_minute_key;
static clock_format_t s_weather_format;
#endif
static uint32_t s_completed_frames;
#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_GRAY4_HOLD
static volatile epd_gdey029t94_refresh_stats_t s_refresh_stats;
#endif
static volatile epd_gdey029t94_status_t s_last_status = EPD_GDEY029T94_ERROR_STATE;
#if (EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF) && \
    (EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_GRAY4_HOLD)
static uint8_t s_demo_started;
#endif

#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_GRAY4_HOLD
static uint32_t s_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * (1000U / configTICK_RATE_HZ));
}
#endif

static epd_gdey029t94_status_t s_bind(void)
{
    epd_gdey029t94_status_t status;
    memset(&s_io, 0, sizeof(s_io));
    status = stm32_gdey029t94_make_io(&s_io, &s_platform);
    if (status == EPD_GDEY029T94_OK) { status = stm32_gdey029t94_prepare(&s_platform); }
    return status;
}

#if EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_GRAY4_HOLD
static epd_gdey029t94_status_t s_run_gray4_diagnostic(void)
{
    epd_gdey029t94_status_t status;
    status = s_bind();
    if (status == EPD_GDEY029T94_OK) {
        status = epd_vendor_demo_run(&s_device, &s_io, s_frame);
    }
    s_last_status = status;
    return status;
}
#else
static uint8_t s_draw_clock(void)
{
#if EPD_CLOCK_USE_GRAY4
    return epd_clock_view_draw_gray4(s_frame, EPD_CLOCK_VIEW_GRAY4_BYTES,
                                     &s_snapshot.clock);
#else
    return epd_clock_view_draw_bw(s_frame, EPD_CLOCK_VIEW_BW_BYTES,
                                  &s_snapshot.clock);
#endif
}

static uint8_t s_draw_weather(void)
{
#if EPD_CLOCK_USE_GRAY4
    return epd_weather_view_draw_gray4(s_frame, EPD_CLOCK_VIEW_GRAY4_BYTES,
        &s_snapshot.weather, &s_snapshot.clock);
#else
    return epd_weather_view_draw_bw(s_frame, EPD_CLOCK_VIEW_BW_BYTES,
        &s_snapshot.weather, &s_snapshot.clock);
#endif
}

static uint32_t s_weather_current_minute(const clock_view_model_t *p_model)
{
    if ((p_model->calendar_state != CLOCK_DATA_READY) ||
        !clock_calendar_valid(&p_model->calendar)) { return 0xFFFFFFFFUL; }
    return ((uint32_t)(p_model->calendar.year - 2000U) << 20) |
        ((uint32_t)p_model->calendar.month << 16) |
        ((uint32_t)p_model->calendar.day << 11) |
        ((uint32_t)p_model->calendar.hour << 6) | p_model->calendar.minute;
}

static uint8_t s_clock_content_due(void)
{
    /* Faults cover the selected page. Background page requests cannot change
       that frame; the final selection takes effect when the fault clears. */
    return (uint8_t)(s_displayed_kind != EPD_CONTENT_CLOCK ||
        clock_schedule_due(&s_schedule, &s_snapshot.clock, s_now_ms()) != 0U);
}

static uint8_t s_weather_content_equal(void)
{
    const weather_data_t *p_displayed = &s_displayed_weather.data;
    const weather_data_t *p_current = &s_snapshot.weather.data;
    if (s_displayed_weather.available != s_snapshot.weather.available) { return 0U; }
    if (s_snapshot.weather.available == 0U) { return 1U; }
    /* Revisions record intermediate requests, including A -> B -> A while
       BUSY. Compare the final fields against the last completed frame. */
    return (uint8_t)(s_displayed_weather.expired == s_snapshot.weather.expired &&
        p_displayed->city == p_current->city &&
        p_displayed->condition == p_current->condition &&
        p_displayed->temperature_tenths == p_current->temperature_tenths &&
        p_displayed->minimum_tenths == p_current->minimum_tenths &&
        p_displayed->maximum_tenths == p_current->maximum_tenths &&
        p_displayed->humidity == p_current->humidity &&
        p_displayed->wind_direction == p_current->wind_direction &&
        p_displayed->wind_speed_tenths == p_current->wind_speed_tenths &&
        memcmp(p_displayed->source_time, p_current->source_time,
               WEATHER_SOURCE_TIME_LENGTH + 1U) == 0);
}

static uint8_t s_weather_content_due(void)
{
    uint32_t minute_key = s_weather_current_minute(&s_snapshot.clock);
    return (uint8_t)(s_displayed_kind != EPD_CONTENT_WEATHER ||
        s_weather_content_equal() == 0U ||
        s_weather_minute_key != minute_key ||
        s_weather_format != s_snapshot.clock.format);
}

static uint8_t s_capture_and_draw(uint8_t force)
{
    uint8_t clock_content;
    display_get_snapshot((uint32_t)xTaskGetTickCount(), &s_snapshot);
    if (s_snapshot.clock.startup_ready == 0U) { return 0U; }
    clock_content = (uint8_t)(s_snapshot.clock.faults.active_categories != 0U ||
                              s_snapshot.page == DISPLAY_PAGE_CLOCK);
    if (force == 0U) {
        if (clock_content != 0U) {
            if (s_clock_content_due() == 0U) { return 0U; }
        } else if (s_weather_content_due() == 0U) {
            return 0U;
        }
    }
    s_snapshot.clock.memory_mode = s_schedule.next_memory;
    if (clock_content != 0U) {
        if (!s_draw_clock()) { return 0U; }
        s_pending_kind = EPD_CONTENT_CLOCK;
    } else {
        if (!s_draw_weather()) { return 0U; }
        s_pending_kind = EPD_CONTENT_WEATHER;
    }
    s_pending = 1U;
    return 1U;
}

static epd_gdey029t94_status_t s_submit_frame(void)
{
    epd_gdey029t94_status_t status;
#if EPD_CLOCK_USE_GRAY4
    status = epd_gdey029t94_init_profile(&s_device, &s_io, EPD_PROFILE_GRAY4);
    if (status == EPD_GDEY029T94_OK) {
        status = epd_gdey029t94_gray4_refresh(&s_device, s_frame,
            EPD_CLOCK_VIEW_GRAY4_BYTES);
    }
#else
    status = epd_gdey029t94_init_profile(&s_device, &s_io, EPD_PROFILE_BW_296);
    if (status == EPD_GDEY029T94_OK) {
        status = epd_gdey029t94_full_refresh(&s_device, s_frame,
            EPD_CLOCK_VIEW_BW_BYTES);
    }
#endif
    if (status != EPD_GDEY029T94_OK) { s_last_status = status; return status; }
    /* BUSY has completed: commit once, even when the following sleep fails. */
    if (s_pending_kind == EPD_CONTENT_CLOCK) {
        clock_schedule_commit(&s_schedule, &s_snapshot.clock, s_now_ms());
    } else {
        s_displayed_weather = s_snapshot.weather;
        s_weather_minute_key = s_weather_current_minute(&s_snapshot.clock);
        s_weather_format = s_snapshot.clock.format;
    }
    s_displayed_kind = s_pending_kind;
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
#endif

epd_gdey029t94_status_t epd_bringup_run_once(void)
{
#if EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_GRAY4_HOLD
    return s_run_gray4_diagnostic();
#else
    epd_gdey029t94_status_t status;
    if (!s_capture_and_draw(1U)) {
        return s_snapshot.clock.startup_ready == 0U ?
            EPD_GDEY029T94_ERROR_STATE : EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    status = s_bind();
    if (status == EPD_GDEY029T94_OK) { status = s_submit_frame(); }
    return status;
#endif
}

epd_gdey029t94_status_t epd_bringup_run_clock(void)
{
#if EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_GRAY4_HOLD
    return s_run_gray4_diagnostic();
#else
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
    clock_view_model_t model;
    status = epd_gdey029t94_init_profile(&s_device, &s_io, EPD_PROFILE_BW_296);
    if (status != EPD_GDEY029T94_OK) { s_last_status = status; return status; }
    for (;;) {
#if EPD_GUI_DEMO
        epd_clock_view_draw_demo(s_frame);
#elif EPD_CLOCK_DIAGNOSTIC_PATTERN
        epd_clock_view_draw_orientation(s_frame);
#else
        clock_service_get_model(&model);
        epd_clock_view_draw(s_frame, model.calendar.hour,
            model.calendar.minute, model.calendar.second);
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
        if (!s_capture_and_draw(1U)) {
            s_last_status = EPD_GDEY029T94_ERROR_ARGUMENT;
            return s_last_status;
        }
    }
    for (;;) {
        if (s_pending == 0U) {
            (void)s_capture_and_draw(0U);
        }
        if (s_pending != 0U) {
            status = s_submit_frame();
            if (status != EPD_GDEY029T94_OK) { return status; }
        }
        (void)osDelay(pdMS_TO_TICKS(EPD_CLOCK_POLL_MS));
    }
#endif
#endif
}

epd_gdey029t94_status_t epd_bringup_get_last_status(void) { return s_last_status; }
uint32_t epd_bringup_get_completed_frames(void) { return s_completed_frames; }
