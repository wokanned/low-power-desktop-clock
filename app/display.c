#include "display.h"
#include "debug.h"
#include "epd_bringup.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

#define DISPLAY_TASK_STACK_BYTES 2048U
#define DISPLAY_TASK_STACK_WORDS (DISPLAY_TASK_STACK_BYTES / sizeof(StackType_t))
#define DISPLAY_START_ATTEMPTS   3U
#define DISPLAY_RETRY_DELAY_MS   1000U

/* One owner for the controller, canvas, frame buffers and refresh policy. */
static StackType_t s_display_stack[DISPLAY_TASK_STACK_WORDS];
static StaticTask_t s_display_task_control;
static osThreadId_t s_display_task;
static volatile epd_gdey029t94_status_t s_display_last_status =
    EPD_GDEY029T94_ERROR_STATE;
static volatile bool s_display_stopped;

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
