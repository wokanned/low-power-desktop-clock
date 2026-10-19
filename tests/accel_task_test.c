#include "accel.h"
#include "debug.h"
#include "clock_faults.h"
#include "stm32_adxl345.h"
#include "task.h"
#include "cmsis_os.h"
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#define TEST_FRAME_LIMIT 512U
#define TEST_DURATION_MS 2200U
#define TEST_LPF_ALPHA 0.4665119f

typedef enum { NORMAL, STALLED, IO_FAILURE, INIT_FAILURE } scenario_t;

static TickType_t s_ticks;
static TickType_t s_start;
static TickType_t s_measure_start;
static uint32_t s_last_sample;
static uint32_t s_init_calls;
static uint32_t s_polls;
static uint32_t s_count;
static scenario_t s_scenario;
static float s_frames[TEST_FRAME_LIMIT][VOFA_MAX_CHANNELS];
static accel_snapshot_t s_snapshots[TEST_FRAME_LIMIT];
static uint32_t s_critical_depth;
static uint32_t s_critical_entries;
static uint32_t s_fault_mask;
static uint32_t s_episode_start;
static uint32_t s_episode_count;
static uint32_t s_recovery_count;
static uint32_t s_start_generation;
static osThreadFunc_t s_entry;
static jmp_buf s_finished;

TickType_t xTaskGetTickCount(void) { return s_ticks; }

void test_accel_critical_enter(void)
{
    /* A snapshot copy must not retain a lock across another callback. */
    assert(s_critical_depth == 0U);
    s_critical_depth++;
    s_critical_entries++;
}

void test_accel_critical_exit(void)
{
    assert(s_critical_depth == 1U);
    s_critical_depth--;
}

void clock_fault_report(clock_fault_source_t source, uint32_t mask)
{
    accel_snapshot_t snapshot;
    assert(s_critical_depth == 0U);
    /* Unsupported ADXL345 temperature must never become a TEMP fault. */
    assert(source == CLOCK_FAULT_IMU);
    accel_get_snapshot(&snapshot);
    if (mask != 0U) {
        assert(snapshot.state == CLOCK_DATA_ERROR);
        if (s_fault_mask == 0U) {
            s_episode_start = s_ticks;
            s_episode_count++;
        }
        assert(snapshot.first_fault_tick == s_episode_start);
        if ((snapshot.flags & ACCEL_STATUS_IO_ERROR) != 0U) {
            assert(mask == CLOCK_FAULT_IO);
        } else if ((snapshot.flags & ACCEL_STATUS_NOT_READY) != 0U) {
            assert(mask == CLOCK_FAULT_TIMEOUT);
        } else {
            assert((snapshot.flags & ACCEL_STATUS_CONFIG_ERROR) != 0U);
            assert(mask == CLOCK_FAULT_ID && snapshot.result == ADXL345_ERROR_ID);
        }
    } else {
        /* Only a usable sample ends the episode; warmup cannot clear it. */
        assert(snapshot.state == CLOCK_DATA_READY);
        assert((snapshot.flags & ACCEL_STATUS_VALID) != 0U);
        if (s_fault_mask != 0U) { s_recovery_count++; }
    }
    s_fault_mask = mask;
}

void vTaskDelay(TickType_t ticks)
{
    assert(s_critical_depth == 0U && ticks > 0U && ticks <= 20U);
    s_ticks += ticks;
    if ((TickType_t)(s_ticks - s_start) >= TEST_DURATION_MS) { longjmp(s_finished, 1); }
}

osThreadId_t osThreadNew(osThreadFunc_t function, void *argument,
                         const osThreadAttr_t *attributes)
{
    (void)argument;
    assert(attributes->stack_size >= 1024U);
    s_entry = function;
    return &s_ticks;
}

int32_t stm32_adxl345_bind(adxl345_ctx_t *p_ctx)
{
    memset(p_ctx, 0, sizeof(*p_ctx));
    return 0;
}

adxl345_status_t adxl345_init(const adxl345_ctx_t *p_ctx)
{
    (void)p_ctx;
    assert(s_critical_depth == 0U);
    s_init_calls++;
    s_measure_start = s_ticks;
    s_last_sample = 0U;
    return (s_scenario == INIT_FAILURE && s_init_calls <= 2U) ?
        ADXL345_ERROR_ID : ADXL345_OK;
}

adxl345_status_t adxl345_sample(const adxl345_ctx_t *p_ctx, adxl345_sample_t *p_sample)
{
    uint32_t sample;
    uint32_t elapsed = s_ticks - s_start;
    (void)p_ctx;
    assert(s_critical_depth == 0U);
    s_polls++;
    if (s_scenario == IO_FAILURE && elapsed >= 200U && elapsed < 250U) {
        return ADXL345_ERROR_IO;
    }
    if (s_scenario == STALLED && s_init_calls == 1U) { return ADXL345_NOT_READY; }
    /* An independent 10.4 ms sensor clock: ready intervals alternate 10/11 ms. */
    sample = (s_ticks - s_measure_start) * 5U / 52U;
    if (sample == s_last_sample) { return ADXL345_NOT_READY; }
    assert(sample == s_last_sample + 1U);
    s_last_sample = sample;
    p_sample->raw[0] = (sample & 1U) != 0U ? 100 : -100;
    p_sample->raw[1] = -p_sample->raw[0];
    p_sample->raw[2] = 256;
    p_sample->source = ADXL345_DATA_READY;
    return ADXL345_OK;
}

bool Debug_IsReady(void) { return true; }
uint32_t Debug_GetQueueDropCount(void) { return 0U; }
uint32_t Debug_GetUartErrorCount(void) { return 0U; }

bool Debug_SendFloats(const float *p_data, uint8_t ch_cnt)
{
    uint32_t flags;
    uint32_t i;
    accel_snapshot_t snapshot;
    assert(s_critical_depth == 0U);
    assert(ch_cnt == VOFA_MAX_CHANNELS && s_count < TEST_FRAME_LIMIT);
    flags = (uint32_t)p_data[15];
    accel_get_snapshot(&snapshot);
    assert(snapshot.flags == flags && snapshot.generation == s_start_generation + s_count + 1U);
    if ((flags & ACCEL_STATUS_VALID) != 0U) {
        assert(snapshot.state == CLOCK_DATA_READY && snapshot.last_sample_tick == s_ticks);
        assert(s_fault_mask == 0U);
    } else if ((flags & (ACCEL_STATUS_IO_ERROR | ACCEL_STATUS_NOT_READY |
                         ACCEL_STATUS_CONFIG_ERROR)) != 0U) {
        assert(snapshot.state == CLOCK_DATA_ERROR && s_fault_mask != 0U);
        assert(snapshot.first_fault_tick == s_episode_start);
    } else if (s_fault_mask != 0U) {
        assert(snapshot.state == CLOCK_DATA_ERROR);
        assert(snapshot.first_fault_tick == s_episode_start);
    } else {
        assert(snapshot.state == CLOCK_DATA_INITIALIZING);
    }
    assert((uint32_t)p_data[1] == s_count);
    assert(isnan(p_data[11]) && (flags & ACCEL_STATUS_TEMPERATURE_INVALID) != 0U);
    for (i = 2U; i < 11U; i++) {
        assert((flags & ACCEL_STATUS_VALID) != 0U ? isfinite(p_data[i]) : isnan(p_data[i]));
    }
    s_snapshots[s_count] = snapshot;
    memcpy(s_frames[s_count++], p_data, ch_cnt * sizeof(float));
    return true;
}

static void s_run(scenario_t scenario, TickType_t start)
{
    accel_snapshot_t snapshot;
    s_scenario = scenario;
    s_start = start;
    s_ticks = start;
    s_init_calls = 0U;
    s_polls = 0U;
    s_count = 0U;
    s_fault_mask = 0U;
    s_episode_start = 0U;
    s_episode_count = 0U;
    s_recovery_count = 0U;
    accel_get_snapshot(&snapshot);
    s_start_generation = snapshot.generation;
    if (setjmp(s_finished) == 0) { s_entry(NULL); }
    assert(s_count > 0U);
    assert(s_critical_depth == 0U && s_critical_entries > s_count * 2U);
    assert(s_fault_mask == 0U);
    assert(s_episode_count == (scenario == NORMAL ? 0U : 1U));
    assert(s_recovery_count == s_episode_count);
}

static void s_normal_tests(TickType_t start)
{
    uint32_t i;
    uint32_t interval;
    uint32_t valid = 0U;
    bool previous_valid = false;
    float previous_filtered = 0.0f;
    float expected;
    s_run(NORMAL, start);
    assert(s_init_calls == 1U && s_polls > s_count * 5U);
    for (i = 0U; i < s_count; i++) {
        assert(s_frames[i][12] == 0.0f);
        assert(((uint32_t)s_frames[i][15] &
            (ACCEL_STATUS_NOT_READY | ACCEL_STATUS_IO_ERROR | ACCEL_STATUS_CONFIG_ERROR)) == 0U);
        if (i > 0U) {
            interval = ((uint32_t)s_frames[i][0] - (uint32_t)s_frames[i - 1U][0]) &
                (ACCEL_WIRE_MODULUS - 1U);
            assert(interval == 10U || interval == 11U);
        }
        if (((uint32_t)s_frames[i][15] & ACCEL_STATUS_VALID) != 0U) {
            valid++;
            expected = previous_valid ? previous_filtered +
                TEST_LPF_ALPHA * (s_frames[i][5] - previous_filtered) : s_frames[i][5];
            assert(fabsf(s_frames[i][8] - expected) < 0.00001f);
            previous_filtered = s_frames[i][8];
            previous_valid = true;
        } else {
            assert(!previous_valid);
            assert(((uint32_t)s_frames[i][15] & ACCEL_STATUS_STARTUP) != 0U);
        }
    }
    assert(valid > 200U);
}

static void s_recovery_tests(void)
{
    uint32_t i;
    uint32_t fault_frames;
    uint32_t flags;
    bool recovered;
    s_run(STALLED, 0U);
    assert(s_init_calls == 2U);
    fault_frames = 0U;
    recovered = false;
    for (i = 0U; i < s_count; i++) {
        flags = (uint32_t)s_frames[i][15];
        if ((flags & ACCEL_STATUS_NOT_READY) != 0U) {
            fault_frames++;
            assert(s_frames[i][0] >= 120.0f && s_frames[i][12] == 1.0f);
            assert((flags & (ACCEL_STATUS_CONFIG_ERROR | ACCEL_STATUS_IO_ERROR)) == 0U);
        }
        if ((flags & ACCEL_STATUS_VALID) != 0U) {
            recovered = true;
            assert(s_frames[i][0] >= 1160.0f && s_frames[i][12] == 1.0f);
        }
    }
    assert(fault_frames == 100U && recovered);
    assert(s_episode_start == 120U);
    /* A repeated heartbeat keeps the onset, including after retry warmup. */
    for (i = 0U; i < s_count; i++) {
        if (s_snapshots[i].state == CLOCK_DATA_ERROR) {
            assert(s_snapshots[i].first_fault_tick == 120U);
        }
    }

    s_run(IO_FAILURE, 0U);
    assert(s_init_calls == 2U && s_frames[s_count - 1U][12] == 3.0f);
    fault_frames = 0U;
    recovered = false;
    for (i = 0U; i < s_count; i++) {
        flags = (uint32_t)s_frames[i][15];
        assert((flags & ACCEL_STATUS_CONFIG_ERROR) == 0U);
        if ((flags & ACCEL_STATUS_IO_ERROR) != 0U) { fault_frames++; }
        if ((flags & ACCEL_STATUS_VALID) != 0U && s_frames[i][12] == 3.0f) {
            recovered = true;
            assert(s_frames[i][8] == s_frames[i][5]);
            break;
        }
    }
    assert(fault_frames >= 100U && recovered);
    assert(s_episode_start == 200U);

    s_run(INIT_FAILURE, 0U);
    assert(s_init_calls == 3U && s_frames[s_count - 1U][12] == 2.0f);
    assert(((uint32_t)s_frames[0][15] & ACCEL_STATUS_CONFIG_ERROR) != 0U);
    assert(((uint32_t)s_frames[s_count - 1U][15] & ACCEL_STATUS_VALID) != 0U);
    assert(s_frames[s_count - 1U][0] >= 2060.0f);
    assert(s_episode_start == 20U);
}

int main(void)
{
    assert(accel_start() && s_entry != NULL);
    s_normal_tests(0U);
    s_normal_tests(UINT32_MAX - 100U);
    s_recovery_tests();
    puts("PASS: ready-driven telemetry, coherent snapshots, first fault ticks, warmup recovery and no TEMP fault");
    return 0;
}
