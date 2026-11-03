#include "display.h"
#include "debug.h"
#include "epd_bringup.h"
#include "clock_service.h"
#include "forecast.h"
#include "epd_weather_pages.h"
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

#define DISPLAY_TASK_STACK_BYTES 2048U
#define DISPLAY_TASK_STACK_WORDS (DISPLAY_TASK_STACK_BYTES / sizeof(StackType_t))
#define DISPLAY_START_ATTEMPTS   3U
#define DISPLAY_RETRY_DELAY_MS   1000U
#define DISPLAY_PAGE_INTERVAL_TICKS ((uint32_t)configTICK_RATE_HZ * 180UL)
#define DISPLAY_TICK_HALF_RANGE 0x80000000UL
#define DISPLAY_WEATHER_INTERVAL_TICKS ((uint32_t)configTICK_RATE_HZ * 60UL)
#define DISPLAY_ALERT_HOLD_TICKS ((uint32_t)configTICK_RATE_HZ * 60UL)

/* One owner for the controller, canvas, frame buffers and refresh policy. */
static StackType_t s_display_stack[DISPLAY_TASK_STACK_WORDS];
static StaticTask_t s_display_task_control;
static osThreadId_t s_display_task;
static volatile epd_gdey029t94_status_t s_display_last_status =
    EPD_GDEY029T94_ERROR_STATE;
static volatile bool s_display_stopped;
static display_page_t s_selected_page = DISPLAY_PAGE_CLOCK;
static uint32_t s_page_revision;
static uint32_t s_page_since_tick;
static bool s_page_cycle_started;
static uint32_t s_attention_seen;
static uint32_t s_attention_tick;
static uint8_t s_attention_active;
static display_page_t s_return_page;
static uint8_t s_return_alert;
static uint16_t s_return_text;
static uint8_t s_alert_index;
static uint16_t s_text_page;

static void s_select_page_locked(display_page_t page)
{
    if (s_selected_page != page) {
        s_selected_page = page;
        s_page_revision++;
        if (s_page_revision == 0U) { s_page_revision = 1U; }
    }
}

static void s_restart_cycle_locked(uint32_t now_tick)
{
    s_page_since_tick = now_tick;
    s_page_cycle_started = true;
}

static void s_poll_page_locked(uint32_t now_tick,
                                const weather_feed_snapshot_t *p_feed)
{
    uint32_t elapsed;
    uint32_t interval;
    if (p_feed->attention_revision != 0U &&
        p_feed->attention_revision != s_attention_seen) {
        s_attention_seen = p_feed->attention_revision;
        if (p_feed->alert_state == WEATHER_ALERT_ACTIVE) {
            if (s_attention_active == 0U) {
                s_return_page = s_selected_page;
                s_return_alert = s_alert_index;
                s_return_text = s_text_page;
            }
            s_attention_active = 1U;
            s_attention_tick = now_tick;
            s_alert_index = p_feed->attention_index;
            s_text_page = 0U;
            s_select_page_locked(DISPLAY_PAGE_ALERT);
            s_restart_cycle_locked(now_tick);
        }
    }
    if (s_attention_active != 0U) {
        if (p_feed->attention_revision == s_attention_seen) {
            s_alert_index = p_feed->attention_index;
        }
        elapsed = (uint32_t)(now_tick - s_attention_tick);
        if (p_feed->alert_state == WEATHER_ALERT_ACTIVE &&
            (p_feed->alert_mask & (uint8_t)(1U << s_alert_index)) != 0U &&
            (elapsed < DISPLAY_ALERT_HOLD_TICKS || elapsed >= DISPLAY_TICK_HALF_RANGE)) {
            return;
        }
        s_attention_active = 0U;
        s_alert_index = s_return_alert;
        s_text_page = s_return_text;
        s_select_page_locked(s_return_page);
        s_restart_cycle_locked(now_tick);
        return;
    }
    if (!s_page_cycle_started) {
        s_restart_cycle_locked(now_tick);
        return;
    }
    elapsed = (uint32_t)(now_tick - s_page_since_tick);
    /* A snapshot tick sampled before a concurrent request is stale. */
    interval = p_feed->available != 0U && s_selected_page != DISPLAY_PAGE_CLOCK ?
        DISPLAY_WEATHER_INTERVAL_TICKS : DISPLAY_PAGE_INTERVAL_TICKS;
    if ((elapsed < interval) ||
        (elapsed >= DISPLAY_TICK_HALF_RANGE)) {
        return;
    }
    if (p_feed->available != 0U) {
        switch (s_selected_page) {
        case DISPLAY_PAGE_CLOCK: s_select_page_locked(DISPLAY_PAGE_WEATHER); break;
        case DISPLAY_PAGE_WEATHER: s_select_page_locked(DISPLAY_PAGE_FORECAST); break;
        case DISPLAY_PAGE_FORECAST: s_select_page_locked(DISPLAY_PAGE_DATA); break;
        default: s_select_page_locked(DISPLAY_PAGE_CLOCK); break;
        }
    } else {
        s_select_page_locked(s_selected_page == DISPLAY_PAGE_CLOCK ?
                             DISPLAY_PAGE_WEATHER : DISPLAY_PAGE_CLOCK);
    }
    /* Skip missed turns after a blocking refresh; never replay old pages. */
    s_restart_cycle_locked(now_tick);
}

bool display_select_page(display_page_t page)
{
    if ((uint32_t)page > (uint32_t)DISPLAY_PAGE_SOURCE) {
        return false;
    }
    taskENTER_CRITICAL();
    s_attention_active = 0U;
    s_alert_index = 0U;
    s_text_page = 0U;
    s_select_page_locked(page);
    s_restart_cycle_locked((uint32_t)xTaskGetTickCount());
    taskEXIT_CRITICAL();
    return true;
}

static uint8_t s_page_number(const char **pp_text, uint16_t *p_number)
{
    const char *p_text = *pp_text;
    uint32_t value = 0U;
    uint8_t count = 0U;
    while (*p_text >= '0' && *p_text <= '9') {
        value = value * 10U + (uint32_t)(*p_text++ - '0');
        if (++count > 5U || value > UINT16_MAX) { return 0U; }
    }
    if (count == 0U || value == 0U) { return 0U; }
    *pp_text = p_text;
    *p_number = (uint16_t)value;
    return 1U;
}

bool display_page_command(const char *p_line)
{
    display_page_t page;
    weather_feed_lease_t lease;
    const char *p_args;
    uint16_t alert = 1U;
    uint16_t text = 1U;
    uint16_t count;
    if (p_line == NULL) { return false; }
    if (strcmp(p_line, "PAGE LIVE") == 0) { return display_select_page(DISPLAY_PAGE_WEATHER); }
    if (strcmp(p_line, "PAGE FORECAST") == 0) { return display_select_page(DISPLAY_PAGE_FORECAST); }
    if (strcmp(p_line, "PAGE DATA") == 0) { return display_select_page(DISPLAY_PAGE_DATA); }
    if (strcmp(p_line, "PAGE ALERT") == 0) { return display_select_page(DISPLAY_PAGE_ALERT); }
    if (strncmp(p_line, "PAGE ALERT ", 11U) == 0) {
        page = DISPLAY_PAGE_ALERT; p_args = p_line + 11U;
    } else if (strncmp(p_line, "PAGE TEXT ", 10U) == 0) {
        page = DISPLAY_PAGE_TEXT; p_args = p_line + 10U;
    } else if (strncmp(p_line, "PAGE SOURCE ", 12U) == 0) {
        page = DISPLAY_PAGE_SOURCE; p_args = p_line + 12U;
    } else { return false; }
    if (s_page_number(&p_args, &alert) == 0U) { return false; }
    if (page == DISPLAY_PAGE_TEXT) {
        if (*p_args++ != ' ' || s_page_number(&p_args, &text) == 0U) { return false; }
    }
    if (*p_args != '\0' || !weather_feed_acquire(0U, &lease)) { return false; }
    count = alert <= lease.p_batch->alert_count ?
        epd_weather_pages_text_count(lease.p_batch, (uint8_t)(alert - 1U)) : 0U;
    weather_feed_release(&lease);
    if (alert > WEATHER_FEED_MAX_ALERTS || count == 0U ||
        (page == DISPLAY_PAGE_TEXT && text > count)) { return false; }
    taskENTER_CRITICAL();
    s_attention_active = 0U;
    s_alert_index = (uint8_t)(alert - 1U);
    s_text_page = (uint16_t)(text - 1U);
    s_select_page_locked(page);
    s_restart_cycle_locked((uint32_t)xTaskGetTickCount());
    taskEXIT_CRITICAL();
    return true;
}

bool display_publish_weather(const weather_data_t *p_data, uint32_t now_tick)
{
    bool published;
    taskENTER_CRITICAL();
    published = weather_publish(p_data, now_tick);
    if (published) {
        clock_service_note_sync_success();
        s_select_page_locked(DISPLAY_PAGE_WEATHER);
        s_restart_cycle_locked(now_tick);
    }
    taskEXIT_CRITICAL();
    return published;
}

void display_forecast_committed(void)
{
    taskENTER_CRITICAL();
    clock_service_note_sync_success();
    s_select_page_locked(DISPLAY_PAGE_WEATHER);
    s_restart_cycle_locked((uint32_t)xTaskGetTickCount());
    taskEXIT_CRITICAL();
}

static uint8_t s_live_date_matches(const weather_data_t *p_live,
                                   const clock_calendar_t *p_date)
{
    uint16_t year = (uint16_t)((p_live->source_time[0] - '0') * 1000 +
        (p_live->source_time[1] - '0') * 100 + (p_live->source_time[2] - '0') * 10 +
        p_live->source_time[3] - '0');
    uint8_t month = (uint8_t)((p_live->source_time[5] - '0') * 10 +
        p_live->source_time[6] - '0');
    uint8_t day = (uint8_t)((p_live->source_time[8] - '0') * 10 +
        p_live->source_time[9] - '0');
    return (uint8_t)(year == p_date->year && month == p_date->month && day == p_date->day);
}

/* Called under the snapshot lock. All inputs are RAM; no storage work occurs. */
static void s_select_weather_locked(const clock_view_model_t *p_clock,
                                    weather_snapshot_t *p_weather)
{
    forecast_batch_t batch;
    uint8_t i;
    if (!forecast_get_batch(&batch)) { return; }
    if (p_clock->calendar_state == CLOCK_DATA_READY &&
        clock_calendar_valid(&p_clock->calendar)) {
        if (p_weather->available != 0U && p_weather->fresh != 0U &&
            p_weather->data.city == batch.city &&
            s_live_date_matches(&p_weather->data, &p_clock->calendar)) { return; }
        memset(p_weather, 0, sizeof(*p_weather));
        p_weather->kind = WEATHER_VIEW_NO_TODAY;
        p_weather->data.city = batch.city;
        p_weather->forecast_date = p_clock->calendar;
        for (i = 0U; i < batch.count; i++) {
            if (batch.days[i].date.year == p_clock->calendar.year &&
                batch.days[i].date.month == p_clock->calendar.month &&
                batch.days[i].date.day == p_clock->calendar.day) {
                p_weather->kind = WEATHER_VIEW_FORECAST;
                p_weather->available = 1U;
                p_weather->fresh = 1U;
                p_weather->revision = batch.id;
                p_weather->forecast_date = batch.days[i].date;
                p_weather->data.condition = batch.days[i].condition;
                p_weather->data.minimum_tenths = batch.days[i].minimum_tenths;
                p_weather->data.maximum_tenths = batch.days[i].maximum_tenths;
                return;
            }
        }
    } else {
        memset(p_weather, 0, sizeof(*p_weather));
        p_weather->kind = WEATHER_VIEW_CLOCK_UNAVAILABLE;
        p_weather->data.city = batch.city;
    }
}

void display_get_snapshot(uint32_t now_tick, display_snapshot_t *p_snapshot)
{
    if (p_snapshot == NULL) { return; }
    taskENTER_CRITICAL();
    weather_feed_get_snapshot(now_tick, &p_snapshot->feed);
    s_poll_page_locked(now_tick, &p_snapshot->feed);
    p_snapshot->page = s_selected_page;
    p_snapshot->page_revision = s_page_revision;
    p_snapshot->alert_index = s_alert_index;
    p_snapshot->text_page = s_text_page;
    clock_service_get_model(&p_snapshot->clock);
    weather_get_snapshot(now_tick, &p_snapshot->weather);
    s_select_weather_locked(&p_snapshot->clock, &p_snapshot->weather);
    taskEXIT_CRITICAL();
}

static void s_display_log_error(epd_gdey029t94_status_t status)
{
    switch (status) {
    case EPD_GDEY029T94_ERROR_BUSY_TIMEOUT:
        Debug_Log("display: BUSY timeout\r\n");
        break;
    case EPD_GDEY029T94_ERROR_IO:
        Debug_Log("display: transport IO error\r\n");
        break;
    case EPD_GDEY029T94_ERROR_ARGUMENT:
        Debug_Log("display: invalid argument\r\n");
        break;
    case EPD_GDEY029T94_ERROR_BUFFER_SIZE:
        Debug_Log("display: invalid buffer size\r\n");
        break;
    case EPD_GDEY029T94_ERROR_GEOMETRY:
        Debug_Log("display: invalid geometry\r\n");
        break;
    case EPD_GDEY029T94_ERROR_ALIGNMENT:
        Debug_Log("display: invalid alignment\r\n");
        break;
    default:
        Debug_Log("display: unexpected state\r\n");
        break;
    }
}

static void s_display_run(void *p_argument)
{
    uint32_t attempt;
#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_GRAY4_HOLD
    uint32_t completed;
#endif
    epd_gdey029t94_status_t status;

    (void)p_argument;
#if EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_GRAY4_HOLD
    Debug_Log("display: starting gray4 diagnostic\r\n");
#else
    Debug_Log("display: starting clock\r\n");
#endif
    attempt = 0U;
    while (attempt < DISPLAY_START_ATTEMPTS) {
#if EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_GRAY4_HOLD
        completed = epd_bringup_get_completed_frames();
        /* The clock stays here on success and returns a concrete failure. */
#endif
        status = epd_bringup_run_clock();
#if EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_GRAY4_HOLD
        s_display_last_status = status;
        if (status == EPD_GDEY029T94_OK) {
            s_display_stopped = true;
            Debug_Log("display: gray4 diagnostic asleep; page retained\r\n");
            for (;;) {
                (void)osThreadSuspend(osThreadGetId());
            }
        }
#else
        if (epd_bringup_get_completed_frames() != completed) { attempt = 0U; }
#endif
        attempt++;
        s_display_last_status = status;
        s_display_log_error(status);
        if (attempt < DISPLAY_START_ATTEMPTS) {
            Debug_Log("display: retry in 1000 ms\r\n");
            (void)osDelay((uint32_t)pdMS_TO_TICKS(DISPLAY_RETRY_DELAY_MS));
        }
    }

    s_display_stopped = true;
    Debug_Log("display: retries exhausted; task suspended\r\n");
    for (;;) {
        (void)osThreadSuspend(osThreadGetId());
    }
}

bool display_start(void)
{
    const osThreadAttr_t attributes = {
        .name = "displayTask",
        .cb_mem = &s_display_task_control,
        .cb_size = sizeof(s_display_task_control),
        .stack_mem = s_display_stack,
        .stack_size = sizeof(s_display_stack),
        .priority = osPriorityNormal
    };

    if (s_display_task != NULL) {
        return true;
    }
    taskENTER_CRITICAL();
    if (!s_page_cycle_started) {
        s_restart_cycle_locked((uint32_t)xTaskGetTickCount());
    }
    taskEXIT_CRITICAL();
    s_display_task = osThreadNew(s_display_run, NULL, &attributes);
    if (s_display_task == NULL) {
        s_display_last_status = EPD_GDEY029T94_ERROR_STATE;
        s_display_stopped = true;
        return false;
    }
    s_display_stopped = false;
    return true;
}

epd_gdey029t94_status_t display_get_last_status(void)
{
    if (s_display_stopped) {
        return s_display_last_status;
    }
    return epd_bringup_get_last_status();
}
