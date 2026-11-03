#include "accel.h"
#include "debug.h"
#include "clock_faults.h"
#include "power.h"
#include "stm32_adxl345.h"
#include "task.h"
#include "cmsis_os.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

typedef enum { IDLE, WAKE, STUCK, BUS_ERROR, CONFIG_LOSS, MODE_SWITCH, ARM_RACE,
               CONFIG_ERROR, CONTINUOUS } scenario_t;
typedef enum { EVENT_MOTION, EVENT_HIGH, EVENT_LOW, EVENT_HOLD, EVENT_AUTO } event_kind_t;
typedef struct { uint32_t tick; event_kind_t kind; } event_t;
static const event_t s_wake_events[] = {{120U, EVENT_MOTION}, {220U, EVENT_MOTION}};
static const event_t s_stuck_events[] = {{120U, EVENT_HIGH}, {1520U, EVENT_LOW}};
static const event_t s_switch_events[] = {{120U, EVENT_HOLD}, {300U, EVENT_AUTO},
                                       {500U, EVENT_MOTION}};
static const event_t s_continuous_events[] = {{120U, EVENT_MOTION}, {150U, EVENT_MOTION},
    {180U, EVENT_MOTION}, {210U, EVENT_MOTION}, {240U, EVENT_MOTION}, {270U, EVENT_MOTION}};
static TickType_t s_ticks;
static TickType_t s_start;
static uint32_t s_duration;
static uint32_t s_depth;
static uint32_t s_notifications;
static uint32_t s_blockers;
static uint32_t s_faults;
static uint32_t s_fault_count;
static uint32_t s_recoveries;
static uint32_t s_motion_count;
static uint32_t s_last_motion;
static uint32_t s_init_calls;
static uint32_t s_arm_calls;
static uint32_t s_source_reads;
static uint32_t s_health_calls;
static uint32_t s_samples;
static uint32_t s_frames;
static uint32_t s_safe_waits;
static uint32_t s_irq_count;
static uint32_t s_event_index;
static uint32_t s_event_count;
static uint32_t s_init_tick;
static uint8_t s_source;
static uint8_t s_armed;
static uint8_t s_stuck;
static uint8_t s_hold;
static uint8_t s_raced;
static scenario_t s_scenario;
static const event_t *s_events;
static stm32_adxl345_motion_notify_t s_notify;
static osThreadFunc_t s_entry;
static jmp_buf s_finished;

void test_accel_critical_enter(void) { assert(s_depth == 0U); s_depth++; }
void test_accel_critical_exit(void) { assert(s_depth == 1U); s_depth--; }
TickType_t xTaskGetTickCount(void) { return s_ticks; }
bool power_is_hold(void) { return s_hold != 0U; }
void power_set_blocker(uint32_t mask, bool active)
{
    assert(s_depth == 0U);
    if (active) { s_blockers |= mask; }
    else {
        if ((mask & POWER_BLOCK_MOTION) != 0U) {
            assert(s_armed != 0U && s_stuck == 0U && s_source == 0U);
        }
        s_blockers &= ~mask;
    }
}
void power_set_fault(uint32_t mask, bool active)
{
    assert(mask == POWER_FAULT_MOTION);
    if (active) {
        if ((s_faults & mask) == 0U) { s_fault_count++; }
        s_faults |= mask;
    } else {
        if ((s_faults & mask) != 0U) { s_recoveries++; }
        s_faults &= ~mask;
    }
}
void power_motion_event(uint32_t now)
{
    assert(s_depth == 0U && now == s_ticks);
    s_motion_count++;
    s_last_motion = now - s_start;
}
void clock_fault_report(clock_fault_source_t source, uint32_t mask)
{
    accel_snapshot_t snapshot;
    assert(source == CLOCK_FAULT_IMU && s_depth == 0U);
    accel_get_snapshot(&snapshot);
    if (mask != 0U) { assert(snapshot.state == CLOCK_DATA_ERROR); }
    else {
        assert(snapshot.state == CLOCK_DATA_READY);
        assert((snapshot.flags & (ACCEL_STATUS_MOTION_ARMED | ACCEL_STATUS_VALID)) != 0U);
    }
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task != NULL); s_notifications++; }
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *higher)
{ assert(task != NULL); *higher = pdTRUE; s_notifications++; }

static void s_event(event_kind_t kind)
{
    if (kind == EVENT_HOLD || kind == EVENT_AUTO) {
        s_hold = (uint8_t)(kind == EVENT_HOLD);
        s_blockers |= POWER_BLOCK_ACCEL | POWER_BLOCK_MOTION;
        accel_power_mode_changed();
    } else if (kind == EVENT_LOW) { s_stuck = 0U; }
    else {
        if (kind == EVENT_HIGH) { s_stuck = 1U; }
        else { s_source |= ADXL345_ACTIVITY; }
        if (s_armed != 0U) {
            s_armed = 0U;
            s_irq_count++;
            s_notify();
        }
    }
}

static void s_advance(TickType_t ticks, uint8_t interruptible)
{
    uint32_t target = s_ticks - s_start + ticks;
    assert(s_depth == 0U);
    while (s_event_index < s_event_count && s_events[s_event_index].tick <= target) {
        s_ticks = s_start + s_events[s_event_index].tick;
        s_event(s_events[s_event_index++].kind);
        if (interruptible != 0U && s_notifications != 0U) { return; }
    }
    s_ticks = s_start + target;
    if (target >= s_duration) { longjmp(s_finished, 1); }
}
void vTaskDelay(TickType_t ticks) { s_advance(ticks, 0U); }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    uint32_t value;
    assert(clear == pdTRUE && ticks > 0U);
    assert((s_blockers & POWER_BLOCK_ACCEL) == 0U);
    if ((s_blockers & POWER_BLOCK_MOTION) == 0U) {
        assert(s_armed != 0U && s_stuck == 0U && s_faults == 0U);
        assert(ticks == 60000U);
        s_safe_waits++;
    }
    if (s_notifications == 0U) { s_advance(ticks, 1U); }
    value = s_notifications;
    s_notifications = 0U;
    return value;
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
{ memset(p_ctx, 0, sizeof(*p_ctx)); return 0; }
void stm32_adxl345_motion_init(stm32_adxl345_motion_notify_t notify)
{ s_notify = notify; s_armed = 0U; }
void stm32_adxl345_motion_disarm(void) { s_armed = 0U; }
void stm32_adxl345_motion_take_pending(void) { s_armed = 0U; }
uint8_t stm32_adxl345_motion_high(void) { return (uint8_t)(s_stuck != 0U || s_source != 0U); }
uint8_t stm32_adxl345_motion_arm(void)
{
    assert(s_ticks - s_init_tick >= 40U);
    if (s_scenario == ARM_RACE && s_raced == 0U) {
        s_raced = 1U;
        s_source = ADXL345_ACTIVITY;
        return 0U;
    }
    if (stm32_adxl345_motion_high() != 0U) { return 0U; }
    s_armed = 1U;
    return 1U;
}
adxl345_status_t adxl345_init(const adxl345_ctx_t *p_ctx)
{
    (void)p_ctx;
    assert((s_blockers & POWER_BLOCK_ACCEL) != 0U && s_depth == 0U);
    s_init_calls++;
    s_init_tick = s_ticks;
    return ADXL345_OK;
}
adxl345_status_t adxl345_arm_activity(const adxl345_ctx_t *p_ctx, uint8_t *p_source)
{
    (void)p_ctx;
    assert(s_hold == 0U && s_armed == 0U);
    s_arm_calls++;
    *p_source = s_source;
    s_source = 0U;
    if (s_scenario == CONFIG_ERROR && s_arm_calls == 1U) {
        return ADXL345_ERROR_CONFIG;
    }
    return ADXL345_OK;
}
adxl345_status_t adxl345_check_activity(const adxl345_ctx_t *p_ctx)
{
    (void)p_ctx;
    s_health_calls++;
    if (s_scenario == CONFIG_LOSS && s_health_calls == 1U) {
        return ADXL345_ERROR_CONFIG;
    }
    return ADXL345_OK;
}
adxl345_status_t adxl345_read_interrupt(const adxl345_ctx_t *p_ctx, uint8_t *p_source)
{
    uint32_t elapsed = s_ticks - s_start;
    (void)p_ctx;
    assert((s_blockers & POWER_BLOCK_ACCEL) != 0U && s_armed == 0U);
    s_source_reads++;
    *p_source = 0U;
    if (s_scenario == BUS_ERROR && elapsed >= 120U && elapsed < 250U) {
        return ADXL345_ERROR_IO;
    }
    *p_source = s_source;
    s_source = 0U;
    return ADXL345_OK;
}
adxl345_status_t adxl345_sample(const adxl345_ctx_t *p_ctx, adxl345_sample_t *p_sample)
{
    (void)p_ctx;
    assert(s_hold != 0U && s_armed == 0U);
    s_samples++;
    memset(p_sample, 0, sizeof(*p_sample));
    p_sample->source = ADXL345_DATA_READY;
    p_sample->raw[2] = 256;
    return ADXL345_OK;
}
bool Debug_IsReady(void) { return true; }
uint32_t Debug_GetQueueDropCount(void) { return 0U; }
uint32_t Debug_GetUartErrorCount(void) { return 0U; }
bool Debug_SendFloats(const float *p_data, uint8_t channels)
{
    assert(s_hold != 0U && channels == VOFA_MAX_CHANNELS && p_data != NULL);
    s_frames++;
    return true;
}

static void s_run(scenario_t scenario, TickType_t start)
{
    s_scenario = scenario;
    s_start = start;
    s_ticks = start;
    s_duration = (scenario == IDLE || scenario == CONFIG_LOSS) ? 125000U : 4000U;
    s_blockers = POWER_BLOCK_ACCEL | POWER_BLOCK_MOTION;
    s_depth = 0U; s_notifications = 0U; s_faults = 0U; s_fault_count = 0U;
    s_recoveries = 0U; s_motion_count = 0U; s_last_motion = 0U;
    s_init_calls = 0U; s_arm_calls = 0U; s_source_reads = 0U; s_health_calls = 0U;
    s_samples = 0U; s_frames = 0U; s_safe_waits = 0U; s_irq_count = 0U;
    s_event_index = 0U; s_event_count = 0U; s_source = 0U;
    s_armed = 0U; s_stuck = 0U; s_hold = 0U; s_raced = 0U;
    s_events = NULL;
    if (scenario == WAKE || scenario == BUS_ERROR) {
        s_events = s_wake_events; s_event_count = 2U;
    } else if (scenario == STUCK) {
        s_events = s_stuck_events; s_event_count = 2U;
    } else if (scenario == MODE_SWITCH) {
        s_events = s_switch_events; s_event_count = 3U;
    } else if (scenario == CONTINUOUS) {
        s_events = s_continuous_events; s_event_count = 6U;
    }
    if (setjmp(s_finished) == 0) { s_entry(NULL); }
    assert(s_depth == 0U && s_faults == 0U && s_armed != 0U);
    assert((s_blockers & (POWER_BLOCK_ACCEL | POWER_BLOCK_MOTION)) == 0U);
    assert(s_safe_waits > 0U && s_source_reads < 30U);
    assert(s_fault_count == s_recoveries);
    if (scenario != MODE_SWITCH) { assert(s_samples == 0U && s_frames == 0U); }
}

int main(void)
{
    assert(accel_start() && s_entry != NULL);
    s_run(IDLE, 0U);
    assert(s_init_calls == 1U && s_source_reads == 3U && s_health_calls == 2U);
    s_run(IDLE, UINT32_MAX - 100U);
    assert(s_health_calls == 2U);
    s_run(WAKE, 0U);
    assert(s_motion_count == 2U && s_last_motion == 220U && s_irq_count == 1U);
    s_run(STUCK, 0U);
    assert(s_fault_count == 1U && s_init_calls == 2U && s_motion_count == 0U);
    s_run(BUS_ERROR, 0U);
    assert(s_fault_count == 1U && s_init_calls == 2U && s_motion_count == 1U);
    s_run(CONFIG_LOSS, 0U);
    assert(s_fault_count == 1U && s_init_calls == 2U);
    s_run(CONFIG_ERROR, 0U);
    assert(s_fault_count == 1U && s_arm_calls == 2U);
    s_run(MODE_SWITCH, 0U);
    assert(s_init_calls == 3U && s_arm_calls == 2U && s_samples > 0U && s_frames > 0U);
    assert(s_motion_count == 1U && s_last_motion == 500U);
    s_run(ARM_RACE, 0U);
    assert(s_motion_count == 1U && s_raced != 0U && s_fault_count == 0U);
    s_run(CONTINUOUS, 0U);
    assert(s_irq_count == 1U && s_motion_count == 3U && s_last_motion == 320U);
    puts("PASS: AUTO motion waits, IRQ burst suppression, arm race, HOLD transitions, health, stuck-line and IO recovery");
    return 0;
}
