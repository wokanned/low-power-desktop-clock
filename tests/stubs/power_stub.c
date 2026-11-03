/* Legacy module tests run in HOLD. Dedicated power tests use the real manager. */
#include "../../app/power.h"
#include <string.h>
void power_init(uint32_t now_ms) { (void)now_ms; }
bool g_test_power_hold = true;
uint32_t g_test_power_blockers;
void power_set_mode(power_mode_t mode) { g_test_power_hold = mode == POWER_MODE_HOLD; }
bool power_is_hold(void) { return g_test_power_hold; }
void power_set_blocker(uint32_t mask, bool active)
{
    if (active) { g_test_power_blockers |= mask; }
    else { g_test_power_blockers &= ~mask; }
}
void power_set_fault(uint32_t mask, bool active) { (void)mask; (void)active; }
void power_motion_event(uint32_t now_ms) { (void)now_ms; }
bool power_can_sleep(uint32_t now_ms) { (void)now_ms; return false; }
void power_get_status(uint32_t now_ms, power_status_t *p_status)
{
    (void)now_ms;
    memset(p_status, 0, sizeof(*p_status));
    p_status->mode = g_test_power_hold ? POWER_MODE_HOLD : POWER_MODE_AUTO;
    p_status->blockers = g_test_power_blockers;
}
void power_record_sleep(uint32_t elapsed, uint32_t source) { (void)elapsed; (void)source; }
