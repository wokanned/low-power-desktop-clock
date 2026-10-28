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
#if !EPD_CLOCK_USE_GRAY4
static uint32_t s_displayed_date;
static uint32_t s_displayed_format_revision;
static uint32_t s_displayed_fault_revision;
static clock_format_t s_displayed_format;
static clock_data_state_t s_displayed_calendar_state;
static uint8_t s_displayed_fault_active;
static uint8_t s_force_full;
#endif
#endif
static uint32_t s_completed_frames;
static volatile epd_gdey029t94_refresh_stats_t s_refresh_stats;
static volatile epd_gdey029t94_status_t s_last_status = EPD_GDEY029T94_ERROR_STATE;
#if (EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF) && \
    (EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_GRAY4_HOLD)
static uint8_t s_demo_started;
#endif

static void s_update_refresh_stats(void)
{
    epd_gdey029t94_refresh_stats_t stats;
    if (epd_gdey029t94_get_refresh_stats(&s_device, &stats) == EPD_GDEY029T94_OK) {
        taskENTER_CRITICAL();
        s_refresh_stats = stats;
        taskEXIT_CRITICAL();
    }
}

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
    s_update_refresh_stats();
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

#if !EPD_CLOCK_USE_GRAY4
static uint32_t s_calendar_date(const clock_view_model_t *p_model)
{
    if (p_model->calendar_state != CLOCK_DATA_READY ||
        !clock_calendar_valid(&p_model->calendar)) { return 0U; }
    return ((uint32_t)p_model->calendar.year << 16) |
        ((uint32_t)p_model->calendar.month << 8) | p_model->calendar.day;
}

#define EPD_PARTIAL_REGION_COUNT 2U
#define EPD_CLOCK_TIME_X 8U
#define EPD_CLOCK_TIME_Y 0U
#define EPD_CLOCK_TIME_WIDTH 148U
#define EPD_CLOCK_TIME_HEIGHT 88U
#define EPD_CLOCK_MEMORY_X 232U
#define EPD_CLOCK_MEMORY_Y 72U
#define EPD_CLOCK_MEMORY_WIDTH 64U
#define EPD_CLOCK_MEMORY_HEIGHT 48U
#define EPD_WEATHER_TIME_X 194U
#define EPD_WEATHER_TIME_Y 0U
#define EPD_WEATHER_TIME_WIDTH 102U
#define EPD_WEATHER_TIME_HEIGHT 24U
/* Renderers map landscape (x,y) to native (127-y,295-x). */
#define EPD_NATIVE_RECT(x, y, width, height) \
    {EPD_GDEY029T94_WIDTH_PIXELS - (y) - (height), \
     EPD_GDEY029T94_HEIGHT_PIXELS - (x) - (width), (height), (width)}
static const epd_gdey029t94_rect_t s_clock_regions[EPD_PARTIAL_REGION_COUNT] = {
    EPD_NATIVE_RECT(EPD_CLOCK_TIME_X, EPD_CLOCK_TIME_Y,
                    EPD_CLOCK_TIME_WIDTH, EPD_CLOCK_TIME_HEIGHT),
    EPD_NATIVE_RECT(EPD_CLOCK_MEMORY_X, EPD_CLOCK_MEMORY_Y,
                    EPD_CLOCK_MEMORY_WIDTH, EPD_CLOCK_MEMORY_HEIGHT)
};
static const epd_gdey029t94_rect_t s_weather_region =
    EPD_NATIVE_RECT(EPD_WEATHER_TIME_X, EPD_WEATHER_TIME_Y,
                    EPD_WEATHER_TIME_WIDTH, EPD_WEATHER_TIME_HEIGHT);

static uint8_t s_partial_allowed(uint8_t continuous)
{
    return (uint8_t)(continuous != 0U && s_force_full == 0U &&
        s_displayed_kind == s_pending_kind && s_device.initialized != 0U &&
        s_device.sleeping == 0U && s_device.background_valid != 0U &&
        s_device.profile == EPD_PROFILE_BW_296 &&
        s_displayed_fault_active == 0U &&
        s_snapshot.clock.faults.active_categories == 0U &&
        s_displayed_fault_revision == s_snapshot.clock.faults.revision &&
        s_displayed_format_revision == s_snapshot.clock.format_revision &&
        s_displayed_format == s_snapshot.clock.format &&
        s_displayed_calendar_state == s_snapshot.clock.calendar_state &&
        s_displayed_date == s_calendar_date(&s_snapshot.clock) &&
        (s_pending_kind != EPD_CONTENT_WEATHER || s_weather_content_equal() != 0U));
}

static void s_include_byte(epd_gdey029t94_rect_t *p_window,
                           uint16_t x, uint16_t y)
{
    uint16_t right = (uint16_t)(x + EPD_GDEY029T94_X_ALIGNMENT_PIXELS);
    uint16_t bottom = (uint16_t)(y + 1U);
    if (p_window->width == 0U) {
        p_window->x = x;
        p_window->y = y;
    } else {
        if (right < p_window->x + p_window->width) {
            right = (uint16_t)(p_window->x + p_window->width);
        }
        if (bottom < p_window->y + p_window->height) {
            bottom = (uint16_t)(p_window->y + p_window->height);
        }
        if (x < p_window->x) { p_window->x = x; }
        if (y < p_window->y) { p_window->y = y; }
    }
    p_window->width = (uint16_t)(right - p_window->x);
    p_window->height = (uint16_t)(bottom - p_window->y);
}

static uint8_t s_find_windows(epd_gdey029t94_rect_t *p_windows, uint8_t *p_count)
{
    const epd_gdey029t94_rect_t *p_regions;
    const epd_gdey029t94_rect_t *p_region;
    uint16_t row;
    uint16_t column;
    uint16_t x;
    uint32_t offset;
    uint8_t i;
    *p_count = s_pending_kind == EPD_CONTENT_CLOCK ? EPD_PARTIAL_REGION_COUNT : 1U;
    p_regions = s_pending_kind == EPD_CONTENT_CLOCK ? s_clock_regions : &s_weather_region;
    memset(p_windows, 0, sizeof(*p_windows) * EPD_PARTIAL_REGION_COUNT);
    for (row = 0U; row < EPD_GDEY029T94_HEIGHT_PIXELS; row++) {
        for (column = 0U; column < EPD_GDEY029T94_BYTES_PER_ROW; column++) {
            offset = (uint32_t)row * EPD_GDEY029T94_BYTES_PER_ROW + column;
            if (s_frame[offset] == s_device.background_image[offset]) { continue; }
            x = (uint16_t)(column * EPD_GDEY029T94_X_ALIGNMENT_PIXELS);
            for (i = 0U; i < *p_count; i++) {
                p_region = &p_regions[i];
                if (x >= p_region->x && x < p_region->x + p_region->width &&
                    row >= p_region->y && row < p_region->y + p_region->height) {
                    s_include_byte(&p_windows[i], x, row);
                    break;
                }
            }
            /* An unrelated visible field changed too: refresh the whole frame. */
            if (i == *p_count) { return 0U; }
        }
    }
    return 1U;
}

static epd_gdey029t94_status_t s_submit_bw(uint8_t continuous, uint8_t *p_changed)
{
    epd_gdey029t94_rect_t windows[EPD_PARTIAL_REGION_COUNT];
    const epd_gdey029t94_rect_t *p_window;
    uint8_t *p_patch = &s_frame[EPD_CLOCK_VIEW_BW_BYTES];
    uint32_t row;
    uint32_t row_bytes;
    uint32_t offset;
    uint8_t count;
    uint8_t i;
    epd_gdey029t94_status_t status;
    *p_changed = 0U;
    if (s_partial_allowed(continuous) != 0U && s_find_windows(windows, &count) != 0U) {
        for (i = 0U; i < count; i++) {
            p_window = &windows[i];
            if (p_window->width == 0U) { continue; }
            row_bytes = p_window->width / EPD_GDEY029T94_X_ALIGNMENT_PIXELS;
            for (row = 0U; row < p_window->height; row++) {
                offset = ((uint32_t)p_window->y + row) * EPD_GDEY029T94_BYTES_PER_ROW +
                    p_window->x / EPD_GDEY029T94_X_ALIGNMENT_PIXELS;
                memcpy(&p_patch[row * row_bytes], &s_frame[offset], row_bytes);
            }
            status = epd_gdey029t94_partial_refresh(&s_device, p_window, p_patch,
                                                    row_bytes * p_window->height);
            if (status != EPD_GDEY029T94_OK) { return status; }
            *p_changed = 1U;
        }
        return EPD_GDEY029T94_OK;
    }
    if (s_device.initialized != 0U && s_device.sleeping == 0U) {
        status = epd_gdey029t94_sleep(&s_device);
        if (status != EPD_GDEY029T94_OK) { return status; }
    }
    status = epd_gdey029t94_init_profile(&s_device, &s_io, EPD_PROFILE_BW_296);
    if (status == EPD_GDEY029T94_OK) {
        status = epd_gdey029t94_full_refresh(&s_device, s_frame, EPD_CLOCK_VIEW_BW_BYTES);
    }
    if (status == EPD_GDEY029T94_OK) { *p_changed = 1U; }
    return status;
}
#endif

static epd_gdey029t94_status_t s_submit_frame(uint8_t continuous)
{
    epd_gdey029t94_status_t status;
    clock_memory_mode_t next_memory = s_schedule.next_memory;
    uint8_t changed = 1U;
#if EPD_CLOCK_USE_GRAY4
    (void)continuous;
    status = epd_gdey029t94_init_profile(&s_device, &s_io, EPD_PROFILE_GRAY4);
    if (status == EPD_GDEY029T94_OK) {
        status = epd_gdey029t94_gray4_refresh(&s_device, s_frame,
            EPD_CLOCK_VIEW_GRAY4_BYTES);
    }
#else
    status = s_submit_bw(continuous, &changed);
#endif
    s_update_refresh_stats();
    if (status != EPD_GDEY029T94_OK) {
#if !EPD_CLOCK_USE_GRAY4
        s_force_full = 1U;
#endif
        s_last_status = status;
        return status;
    }
    /* Commit only after every window completed. A partial failure leaves the
       application snapshot and memory phase untouched until full recovery. */
    if (s_pending_kind == EPD_CONTENT_CLOCK) {
        clock_schedule_commit(&s_schedule, &s_snapshot.clock, s_now_ms());
        if (changed == 0U) { s_schedule.next_memory = next_memory; }
    } else {
        s_displayed_weather = s_snapshot.weather;
        s_weather_minute_key = s_weather_current_minute(&s_snapshot.clock);
        s_weather_format = s_snapshot.clock.format;
    }
    s_displayed_kind = s_pending_kind;
#if !EPD_CLOCK_USE_GRAY4
    s_displayed_date = s_calendar_date(&s_snapshot.clock);
    s_displayed_format = s_snapshot.clock.format;
    s_displayed_format_revision = s_snapshot.clock.format_revision;
    s_displayed_fault_revision = s_snapshot.clock.faults.revision;
    s_displayed_calendar_state = s_snapshot.clock.calendar_state;
    s_displayed_fault_active = s_snapshot.clock.faults.active_categories;
    s_force_full = 0U;
#endif
    s_pending = 0U;
    if (changed != 0U) { s_completed_frames++; }
#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF
    if (epd_vendor_demo_state.status != EPD_GDEY029T94_OK) {
        epd_vendor_demo_state.recovered_to_clock = 1U;
    }
#endif
#if !EPD_CLOCK_USE_GRAY4
    if (continuous != 0U && s_snapshot.clock.faults.active_categories == 0U) {
        s_last_status = EPD_GDEY029T94_OK;
        return EPD_GDEY029T94_OK;
    }
#endif
    s_last_status = epd_gdey029t94_sleep(&s_device);
    s_update_refresh_stats();
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
    if (status == EPD_GDEY029T94_OK) { status = s_submit_frame(0U); }
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
        s_update_refresh_stats();
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
            status = s_submit_frame(1U);
            if (status != EPD_GDEY029T94_OK) { return status; }
        }
        (void)osDelay(pdMS_TO_TICKS(EPD_CLOCK_POLL_MS));
    }
#endif
#endif
}

epd_gdey029t94_status_t epd_bringup_get_last_status(void) { return s_last_status; }
uint32_t epd_bringup_get_completed_frames(void) { return s_completed_frames; }

void epd_bringup_get_refresh_stats(epd_gdey029t94_refresh_stats_t *p_stats)
{
    if (p_stats == NULL) { return; }
    taskENTER_CRITICAL();
    *p_stats = s_refresh_stats;
    taskEXIT_CRITICAL();
}
