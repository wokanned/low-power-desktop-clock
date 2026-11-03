#include "accel.h"
#include "debug.h"
#include "clock_faults.h"
#include "power.h"
#include "stm32_adxl345.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include <string.h>

#define ACCEL_STACK_WORDS 256U
#define ACCEL_RETRY_MS 1000U
#define ACCEL_BOOT_WAIT_MS 20U
#define ACCEL_WARMUP_MS 40U
#define ACCEL_POLL_MS 1U
#define ACCEL_DATA_TIMEOUT_MS 100U
#define ACCEL_RECOVERY_ERRORS 3U
#define ACCEL_LPF_ALPHA 0.4665119f /* 1-exp(-2*pi*10*0.01) */
#define ACCEL_RAW_MIN (-512)
#define ACCEL_RAW_MAX 511
#define ACCEL_COUNTER_MAX (ACCEL_WIRE_MODULUS - 1U)
#define ACCEL_MOTION_RECHECK_MS 100U
#define ACCEL_MOTION_HIGH_TIMEOUT_MS 1000U
#define ACCEL_MOTION_HEALTH_MS 60000U

static StackType_t s_stack[ACCEL_STACK_WORDS];
static StaticTask_t s_control;
static osThreadId_t s_task;
static adxl345_ctx_t s_ctx;
static accel_snapshot_t s_snapshot;

static void s_publish(adxl345_status_t result, uint32_t flags, TickType_t now)
{
    uint32_t mask = 0U;
    if ((flags & ACCEL_STATUS_IO_ERROR) != 0U) { mask = CLOCK_FAULT_IO; }
    if ((flags & ACCEL_STATUS_CONFIG_ERROR) != 0U) {
        mask = result == ADXL345_ERROR_ID ? CLOCK_FAULT_ID : CLOCK_FAULT_CONFIGURATION;
    }
    if ((flags & ACCEL_STATUS_NOT_READY) != 0U) { mask = CLOCK_FAULT_TIMEOUT; }
    taskENTER_CRITICAL();
    if (mask != 0U) {
        if (s_snapshot.state != CLOCK_DATA_ERROR) { s_snapshot.first_fault_tick = now; }
        s_snapshot.state = CLOCK_DATA_ERROR;
    } else if ((flags & (ACCEL_STATUS_VALID | ACCEL_STATUS_MOTION_ARMED)) != 0U) {
        s_snapshot.state = CLOCK_DATA_READY;
        if ((flags & ACCEL_STATUS_VALID) != 0U) { s_snapshot.last_sample_tick = now; }
    } else if (s_snapshot.state != CLOCK_DATA_ERROR) {
        s_snapshot.state = CLOCK_DATA_INITIALIZING;
    }
    s_snapshot.result = result;
    s_snapshot.flags = flags;
    s_snapshot.generation++;
    taskEXIT_CRITICAL();
    /* Recovery warmup clears neither sampled-data nor motion-source faults. */
    if (mask != 0U || (flags & (ACCEL_STATUS_VALID | ACCEL_STATUS_MOTION_ARMED)) != 0U) {
        clock_fault_report(CLOCK_FAULT_IMU, mask);
    }
}

static void s_motion_notify(void)
{
    BaseType_t higher_priority = pdFALSE;
    if (s_task != NULL) {
        vTaskNotifyGiveFromISR((TaskHandle_t)s_task, &higher_priority);
        portYIELD_FROM_ISR(higher_priority);
    }
}

void accel_power_mode_changed(void)
{
    if (s_task != NULL) { xTaskNotifyGive((TaskHandle_t)s_task); }
}

static void s_motion_wait(TickType_t ticks)
{
    /* No I2C operation is in flight. MOTION remains set until the wake source
       is checked; after arming, an EXTI edge makes the owner runnable. */
    power_set_blocker(POWER_BLOCK_ACCEL, false);
    if (!power_is_hold()) { (void)ulTaskNotifyTake(pdTRUE, ticks); }
    power_set_blocker(POWER_BLOCK_ACCEL, true);
}

static void s_motion_pause(uint32_t delay_ms)
{
    TickType_t started = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(delay_ms);
    TickType_t elapsed = 0U;
    /* An old IRQ token cannot shorten settling, burst suppression or retry.
       A mode change still wakes the owner promptly. */
    while (!power_is_hold() && elapsed < duration) {
        s_motion_wait(duration - elapsed);
        elapsed = (TickType_t)(xTaskGetTickCount() - started);
    }
}

static void s_motion_fault(adxl345_status_t result)
{
    uint32_t flags = ACCEL_STATUS_TEMPERATURE_INVALID;
    if (result == ADXL345_NOT_READY) { flags |= ACCEL_STATUS_NOT_READY; }
    else if (result == ADXL345_ERROR_IO) { flags |= ACCEL_STATUS_IO_ERROR; }
    else { flags |= ACCEL_STATUS_CONFIG_ERROR; }
    power_set_blocker(POWER_BLOCK_MOTION, true);
    power_set_fault(POWER_FAULT_MOTION, true);
    stm32_adxl345_motion_disarm();
    s_publish(result, flags, xTaskGetTickCount());
}

static void s_run_motion(void)
{
    adxl345_status_t result;
    TickType_t now;
    TickType_t high_since = 0U;
    TickType_t health_tick = 0U;
    uint8_t source;
    uint8_t high;
    uint8_t tracking_high = 0U;
    uint8_t initialized = 0U;

    power_set_blocker(POWER_BLOCK_ACCEL | POWER_BLOCK_MOTION, true);
    stm32_adxl345_motion_disarm();
    while (!power_is_hold()) {
        source = 0U;
        if (initialized == 0U) {
            stm32_adxl345_motion_take_pending();
            result = adxl345_init(&s_ctx);
            if (result == ADXL345_OK) { result = adxl345_arm_activity(&s_ctx, &source); }
            if ((source & ADXL345_ACTIVITY) != 0U) {
                power_motion_event((uint32_t)xTaskGetTickCount());
            }
            if (result != ADXL345_OK) {
                s_motion_fault(result);
                s_motion_pause(ACCEL_RETRY_MS);
                continue;
            }
            initialized = 1U;
            health_tick = xTaskGetTickCount();
            tracking_high = 0U;
            s_motion_pause(ACCEL_WARMUP_MS);
            continue;
        }
        now = xTaskGetTickCount();
        power_set_blocker(POWER_BLOCK_MOTION, true);
        stm32_adxl345_motion_take_pending();
        result = adxl345_read_interrupt(&s_ctx, &source);
        if ((source & ADXL345_ACTIVITY) != 0U) { power_motion_event((uint32_t)now); }
        if (result == ADXL345_OK &&
            (TickType_t)(now - health_tick) >= pdMS_TO_TICKS(ACCEL_MOTION_HEALTH_MS)) {
            result = adxl345_check_activity(&s_ctx);
            health_tick = xTaskGetTickCount();
        }
        if (result != ADXL345_OK) {
            initialized = 0U;
            s_motion_fault(result);
            s_motion_pause(ACCEL_RETRY_MS);
            continue;
        }
        high = stm32_adxl345_motion_high();
        if (high != 0U) {
            if (tracking_high == 0U) { high_since = now; tracking_high = 1U; }
            if ((TickType_t)(now - high_since) >= pdMS_TO_TICKS(ACCEL_MOTION_HIGH_TIMEOUT_MS)) {
                initialized = 0U;
                s_motion_fault(ADXL345_NOT_READY);
                s_motion_pause(ACCEL_RETRY_MS);
                continue;
            }
        } else { tracking_high = 0U; }
        /* Mask throughout a burst of movement. Processing at most once per
           100 ms extends the awake window without an IRQ storm. */
        if ((source & ADXL345_ACTIVITY) != 0U || high != 0U) {
            s_motion_pause(ACCEL_MOTION_RECHECK_MS);
            continue;
        }
        if (stm32_adxl345_motion_arm() == 0U) {
            s_motion_pause(ACCEL_MOTION_RECHECK_MS);
            continue;
        }
        s_publish(ADXL345_OK, ACCEL_STATUS_INITIALIZED | ACCEL_STATUS_MOTION_ARMED |
                  ACCEL_STATUS_TEMPERATURE_INVALID, now);
        power_set_fault(POWER_FAULT_MOTION, false);
        power_set_blocker(POWER_BLOCK_MOTION, false);
        s_motion_wait(pdMS_TO_TICKS(ACCEL_MOTION_HEALTH_MS));
    }
    stm32_adxl345_motion_disarm();
    power_set_blocker(POWER_BLOCK_MOTION, true);
}

static float s_nan(void)
{
    uint32_t bits = 0x7FC00000U;
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void s_run(void *p_argument)
{
    float frame[VOFA_MAX_CHANNELS];
    float filtered[3] = {0.0f, 0.0f, 0.0f};
    adxl345_sample_t sample;
    adxl345_status_t result = ADXL345_ERROR_IO;
    TickType_t iteration_start;
    TickType_t now;
    TickType_t last_attempt = 0U;
    TickType_t warmup_start = 0U;
    TickType_t last_data = 0U;
    uint32_t sequence = 0U;
    uint32_t errors = 0U;
    uint32_t consecutive_errors = 0U;
    uint32_t flags;
    uint8_t i;
    bool initialized = false;
    bool attempted = false;
    bool filter_valid = false;
    bool late;

    (void)p_argument;
    stm32_adxl345_motion_init(s_motion_notify);
    vTaskDelay(pdMS_TO_TICKS(ACCEL_BOOT_WAIT_MS));
    for (;;) {
        power_set_blocker(POWER_BLOCK_ACCEL, true);
        if (!power_is_hold()) {
            s_run_motion();
            initialized = false;
            attempted = false;
            filter_valid = false;
            consecutive_errors = 0U;
        }
        now = xTaskGetTickCount();
        iteration_start = now;
        flags = ACCEL_STATUS_TEMPERATURE_INVALID;
        for (i = 0U; i < VOFA_MAX_CHANNELS; i++) { frame[i] = s_nan(); }
        if (!initialized && (!attempted ||
            (now - last_attempt) >= pdMS_TO_TICKS(ACCEL_RETRY_MS))) {
            result = adxl345_init(&s_ctx);
            last_attempt = xTaskGetTickCount();
            attempted = true;
            initialized = result == ADXL345_OK;
            filter_valid = false;
            consecutive_errors = 0U;
            warmup_start = last_attempt;
            last_data = last_attempt;
            if (!initialized && errors < ACCEL_COUNTER_MAX) { errors++; }
        }
        if (initialized) {
            flags |= ACCEL_STATUS_INITIALIZED;
            sample.source = 0U;
            result = adxl345_sample(&s_ctx, &sample);
            if ((sample.source & ADXL345_ACTIVITY) != 0U) {
                power_motion_event((uint32_t)xTaskGetTickCount());
            }
            now = xTaskGetTickCount();
            if (result == ADXL345_OK) {
                last_data = now;
                consecutive_errors = 0U;
                if ((sample.source & ADXL345_OVERRUN) != 0U) { flags |= ACCEL_STATUS_OVERRUN; }
                if ((xTaskGetTickCount() - warmup_start) < pdMS_TO_TICKS(ACCEL_WARMUP_MS)) {
                    flags |= ACCEL_STATUS_STARTUP;
                    filter_valid = false;
                } else {
                    flags |= ACCEL_STATUS_VALID;
                    for (i = 0U; i < 3U; i++) {
                        frame[2U + i] = (float)sample.raw[i];
                        frame[5U + i] = (float)sample.raw[i] * ADXL345_SCALE_G_PER_LSB;
                        if (!filter_valid) { filtered[i] = frame[5U + i]; }
                        else { filtered[i] += ACCEL_LPF_ALPHA * (frame[5U + i] - filtered[i]); }
                        frame[8U + i] = filtered[i];
                        if (sample.raw[i] <= ACCEL_RAW_MIN || sample.raw[i] >= ACCEL_RAW_MAX) {
                            flags |= ACCEL_STATUS_SATURATED;
                        }
                    }
                    filter_valid = true;
                }
            } else if (result == ADXL345_NOT_READY) {
                /* Separate asynchronous sample timing from an actual sensor stall. */
                consecutive_errors = 0U;
                if ((now - last_data) < pdMS_TO_TICKS(ACCEL_DATA_TIMEOUT_MS)) {
                    vTaskDelay(pdMS_TO_TICKS(ACCEL_POLL_MS));
                    continue;
                }
                flags |= ACCEL_STATUS_NOT_READY;
                filter_valid = false;
                if (errors < ACCEL_COUNTER_MAX) { errors++; }
                initialized = false;
                last_attempt = now;
            } else {
                filter_valid = false;
                if (errors < ACCEL_COUNTER_MAX) { errors++; }
                consecutive_errors++;
                flags |= ACCEL_STATUS_IO_ERROR;
                if (consecutive_errors >= ACCEL_RECOVERY_ERRORS) {
                    initialized = false;
                    last_attempt = xTaskGetTickCount();
                }
            }
        } else {
            if (result == ADXL345_NOT_READY) { flags |= ACCEL_STATUS_NOT_READY; }
            else if (result == ADXL345_ERROR_IO) { flags |= ACCEL_STATUS_IO_ERROR; }
            else { flags |= ACCEL_STATUS_CONFIG_ERROR; }
        }
        now = xTaskGetTickCount();
        late = (now - iteration_start) >= pdMS_TO_TICKS(ACCEL_SAMPLE_MS);
        if (late) { flags |= ACCEL_STATUS_LATE; }
        s_publish(result, flags, now);
        frame[0] = (float)((now * (1000U / configTICK_RATE_HZ)) % ACCEL_WIRE_MODULUS);
        frame[1] = (float)sequence;
        frame[12] = (float)errors;
        frame[13] = (float)Debug_GetQueueDropCount();
        frame[14] = (float)Debug_GetUartErrorCount();
        frame[15] = (float)flags;
        (void)Debug_SendFloats(frame, VOFA_MAX_CHANNELS);
        sequence = (sequence + 1U) % ACCEL_WIRE_MODULUS;
        /* Fresh samples drive telemetry; retain bounded-rate fault heartbeats. */
        vTaskDelay(pdMS_TO_TICKS(result == ADXL345_OK ? ACCEL_POLL_MS : ACCEL_SAMPLE_MS));
    }
}

bool accel_start(void)
{
    const osThreadAttr_t attributes = {
        .name = "accelTask", .cb_mem = &s_control, .cb_size = sizeof(s_control),
        .stack_mem = s_stack, .stack_size = sizeof(s_stack),
        .priority = osPriorityAboveNormal
    };
    if (s_task != NULL) { return true; }
    if (!Debug_IsReady() || stm32_adxl345_bind(&s_ctx) != 0) {
        power_set_fault(POWER_FAULT_MOTION, true);
        s_snapshot.state = CLOCK_DATA_ERROR;
        clock_fault_report(CLOCK_FAULT_IMU, CLOCK_FAULT_START);
        return false;
    }
    s_task = osThreadNew(s_run, NULL, &attributes);
    if (s_task == NULL) {
        power_set_fault(POWER_FAULT_MOTION, true);
        s_snapshot.state = CLOCK_DATA_ERROR;
        clock_fault_report(CLOCK_FAULT_IMU, CLOCK_FAULT_START);
    }
    return s_task != NULL;
}

void accel_get_snapshot(accel_snapshot_t *p_snapshot)
{
    if (p_snapshot != NULL) {
        taskENTER_CRITICAL();
        *p_snapshot = s_snapshot;
        taskEXIT_CRITICAL();
    }
}
