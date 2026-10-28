#include "display.h"
#include "debug.h"
#include "epd_bringup.h"
#include "clock_service.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

#define DISPLAY_TASK_STACK_BYTES 2048U
#define DISPLAY_TASK_STACK_WORDS (DISPLAY_TASK_STACK_BYTES / sizeof(StackType_t))
#define DISPLAY_START_ATTEMPTS   3U
#define DISPLAY_RETRY_DELAY_MS   1000U
#define DISPLAY_PAGE_INTERVAL_TICKS ((uint32_t)configTICK_RATE_HZ * 180UL)
#define DISPLAY_TICK_HALF_RANGE 0x80000000UL

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

static void s_poll_page_locked(uint32_t now_tick)
{
    uint32_t elapsed;
    if (!s_page_cycle_started) {
        s_restart_cycle_locked(now_tick);
        return;
    }
    elapsed = (uint32_t)(now_tick - s_page_since_tick);
    /* A snapshot tick sampled before a concurrent request is stale. */
    if ((elapsed < DISPLAY_PAGE_INTERVAL_TICKS) ||
        (elapsed >= DISPLAY_TICK_HALF_RANGE)) {
        return;
    }
    s_select_page_locked(s_selected_page == DISPLAY_PAGE_CLOCK ?
                         DISPLAY_PAGE_WEATHER : DISPLAY_PAGE_CLOCK);
    /* Skip missed turns after a blocking refresh; never replay old pages. */
    s_restart_cycle_locked(now_tick);
}

bool display_select_page(display_page_t page)
{
    if ((page != DISPLAY_PAGE_CLOCK) && (page != DISPLAY_PAGE_WEATHER)) {
        return false;
    }
    taskENTER_CRITICAL();
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

void display_get_snapshot(uint32_t now_tick, display_snapshot_t *p_snapshot)
{
    if (p_snapshot == NULL) { return; }
    taskENTER_CRITICAL();
    s_poll_page_locked(now_tick);
    p_snapshot->page = s_selected_page;
    p_snapshot->page_revision = s_page_revision;
    clock_service_get_model(&p_snapshot->clock);
    weather_get_snapshot(now_tick, &p_snapshot->weather);
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
