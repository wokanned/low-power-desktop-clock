#include "accel.h"
#include "debug.h"
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

static StackType_t s_stack[ACCEL_STACK_WORDS];
static StaticTask_t s_control;
static osThreadId_t s_task;
static adxl345_ctx_t s_ctx;

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
    vTaskDelay(pdMS_TO_TICKS(ACCEL_BOOT_WAIT_MS));
    for (;;) {
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
            result = adxl345_sample(&s_ctx, &sample);
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
    if (!Debug_IsReady() || stm32_adxl345_bind(&s_ctx) != 0) { return false; }
    s_task = osThreadNew(s_run, NULL, &attributes);
    return s_task != NULL;
}
