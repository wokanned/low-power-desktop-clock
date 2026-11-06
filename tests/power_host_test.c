#include "power.h"
#include <assert.h>
#include <stdio.h>

static unsigned s_critical;
static unsigned s_accel_notifications;
static uint8_t s_scan_paused;
void clock_test_enter(void) { s_critical++; }
void clock_test_exit(void) { assert(s_critical != 0U); s_critical--; }
void accel_power_mode_changed(void) { assert(s_critical == 0U); s_accel_notifications++; }
void storage_set_scan_paused(uint8_t paused) { assert(s_critical == 0U); s_scan_paused = paused; }

int main(void)
{
    power_status_t status;
    uint32_t i;
    const uint32_t owners[] = { POWER_BLOCK_DEBUG, POWER_BLOCK_STORAGE,
        POWER_BLOCK_DISPLAY, POWER_BLOCK_CLOCK, POWER_BLOCK_ACCEL,
        POWER_BLOCK_MOTION, POWER_BLOCK_UART, POWER_BLOCK_INPUT };
    assert(!power_can_sleep(40000U));
    power_init(0U);
    assert(!power_can_sleep(30000U)); /* startup owners have not acknowledged */
    power_set_blocker(UINT32_MAX, false);
    assert(power_can_sleep(30000U));
    power_init(UINT32_MAX - 9999U);
    power_set_blocker(UINT32_MAX, false);
    assert(!power_can_sleep(19999U));
    assert(power_can_sleep(20000U));
    for (i = 0U; i < sizeof(owners) / sizeof(owners[0]); i++) {
        power_set_blocker(owners[i], true);
        assert(!power_can_sleep(20000U));
        power_set_blocker(owners[i], false);
        assert(power_can_sleep(20000U));
    }
    power_motion_event(20000U);
    assert(!power_can_sleep(29999U));
    power_motion_event(29999U);
    assert(!power_can_sleep(39998U));
    assert(power_can_sleep(39999U));
    power_set_fault(POWER_FAULT_MOTION, true);
    power_set_fault(POWER_FAULT_MOTION, true);
    assert(!power_can_sleep(50000U));
    power_get_status(50000U, &status);
    assert(status.fault_count == 1U);
    power_set_fault(POWER_FAULT_MOTION, false);
    assert(power_can_sleep(50000U));
    power_set_mode(POWER_MODE_HOLD);
    assert(power_is_hold() && s_scan_paused == 0U && s_accel_notifications == 1U);
    assert(!power_can_sleep(50000U));
    power_set_mode(POWER_MODE_AUTO);
    assert(!power_is_hold() && s_scan_paused == 1U && s_accel_notifications == 2U);
    assert(!power_can_sleep(50000U)); /* needs accel owner acknowledgment */
    power_set_blocker(POWER_BLOCK_ACCEL | POWER_BLOCK_MOTION, false);
    assert(power_can_sleep(50000U));
    for (i = 0U; i < 32768U; i++) { power_record_sleep(1U, POWER_WAKE_TIMER); }
    power_record_sleep(32768U, POWER_WAKE_MOTION);
    power_get_status(50000U, &status);
    assert(status.sleep_ms == 2000U && status.sleep_count == 32769U);
    assert(status.motion_count == 2U && status.last_wake == POWER_WAKE_MOTION);
    assert(status.wake_sources == (POWER_WAKE_TIMER | POWER_WAKE_MOTION));
    assert(power_can_sleep(50000U)); /* timed wakes do not renew motion window */
    power_init(100U);
    assert(!power_is_hold() && !power_can_sleep(101U));
    power_get_status(101U, &status);
    assert(status.sleep_count == 0U && status.fault_count == 0U);
    assert(status.window_remaining_ms == 29999U && s_critical == 0U);
    puts("PASS: startup veto, all owners, wrap, motion renewal, mode acknowledgment, faults and LSE accumulation");
    return 0;
}
