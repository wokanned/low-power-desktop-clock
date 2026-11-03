#include "power.h"
#include "accel.h"
#include "storage_files.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <limits.h>

#define POWER_LSE_HZ 32768UL
#define POWER_MS_PER_SECOND 1000UL
#define POWER_OWNER_BLOCKERS (POWER_BLOCK_DEBUG | POWER_BLOCK_STORAGE | \
    POWER_BLOCK_DISPLAY | POWER_BLOCK_CLOCK | POWER_BLOCK_ACCEL | POWER_BLOCK_MOTION)

static power_status_t s_status;
static uint32_t s_boot_start;
static uint32_t s_motion_start;
static uint32_t s_sleep_fraction;
static uint8_t s_boot_window;
static uint8_t s_motion_window;
static uint8_t s_initialized;

static uint32_t s_blockers(uint32_t now_ms, uint32_t *p_remaining)
{
    uint32_t blockers = s_status.blockers;
    uint32_t elapsed;
    uint32_t remaining = 0U;
    if (s_boot_window != 0U) {
        elapsed = now_ms - s_boot_start;
        if (elapsed >= POWER_BOOT_WINDOW_MS) { s_boot_window = 0U; }
        else {
            blockers |= POWER_BLOCK_BOOT;
            remaining = POWER_BOOT_WINDOW_MS - elapsed;
        }
    }
    if (s_motion_window != 0U) {
        elapsed = now_ms - s_motion_start;
        if (elapsed >= POWER_MOTION_WINDOW_MS) { s_motion_window = 0U; }
        else {
            blockers |= POWER_BLOCK_WINDOW;
            if (POWER_MOTION_WINDOW_MS - elapsed > remaining) {
                remaining = POWER_MOTION_WINDOW_MS - elapsed;
            }
        }
    }
    if (s_status.mode == POWER_MODE_HOLD) { blockers |= POWER_BLOCK_HOLD; }
    if (s_status.faults != 0U || s_initialized == 0U) { blockers |= POWER_BLOCK_FAULT; }
    *p_remaining = remaining;
    return blockers;
}

void power_init(uint32_t now_ms)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = POWER_MODE_AUTO;
    s_status.blockers = POWER_OWNER_BLOCKERS;
    s_boot_start = now_ms;
    s_motion_start = now_ms;
    s_sleep_fraction = 0U;
    s_boot_window = 1U;
    s_motion_window = 0U;
    s_initialized = 1U;
}

void power_set_mode(power_mode_t mode)
{
    if (mode != POWER_MODE_AUTO && mode != POWER_MODE_HOLD) { return; }
    taskENTER_CRITICAL();
    s_status.mode = mode;
    /* The accel owner must acknowledge the new hardware mode before sleeping. */
    s_status.blockers |= POWER_BLOCK_ACCEL | POWER_BLOCK_MOTION;
    taskEXIT_CRITICAL();
    storage_set_scan_paused(mode == POWER_MODE_AUTO ? 1U : 0U);
    accel_power_mode_changed();
}

bool power_is_hold(void)
{
    bool hold;
    taskENTER_CRITICAL();
    hold = s_status.mode == POWER_MODE_HOLD;
    taskEXIT_CRITICAL();
    return hold;
}

void power_set_blocker(uint32_t mask, bool active)
{
    taskENTER_CRITICAL();
    if (active) { s_status.blockers |= mask; }
    else { s_status.blockers &= ~mask; }
    taskEXIT_CRITICAL();
}

void power_set_fault(uint32_t mask, bool active)
{
    taskENTER_CRITICAL();
    if (active) {
        if ((s_status.faults & mask) != mask && s_status.fault_count != UINT32_MAX) {
            s_status.fault_count++;
        }
        s_status.faults |= mask;
    } else { s_status.faults &= ~mask; }
    taskEXIT_CRITICAL();
}

void power_motion_event(uint32_t now_ms)
{
    taskENTER_CRITICAL();
    s_motion_start = now_ms;
    s_motion_window = 1U;
    if (s_status.motion_count != UINT32_MAX) { s_status.motion_count++; }
    taskEXIT_CRITICAL();
}

bool power_can_sleep(uint32_t now_ms)
{
    uint32_t remaining;
    uint32_t blockers;
    taskENTER_CRITICAL();
    blockers = s_blockers(now_ms, &remaining);
    taskEXIT_CRITICAL();
    return blockers == 0U;
}

void power_get_status(uint32_t now_ms, power_status_t *p_status)
{
    if (p_status == NULL) { return; }
    taskENTER_CRITICAL();
    *p_status = s_status;
    p_status->blockers = s_blockers(now_ms, &p_status->window_remaining_ms);
    taskEXIT_CRITICAL();
}

void power_record_sleep(uint32_t elapsed_lse_counts, uint32_t wake_source)
{
    uint64_t scaled;
    uint32_t milliseconds;
    taskENTER_CRITICAL();
    scaled = (uint64_t)elapsed_lse_counts * POWER_MS_PER_SECOND + s_sleep_fraction;
    milliseconds = (uint32_t)(scaled / POWER_LSE_HZ);
    s_sleep_fraction = (uint32_t)(scaled % POWER_LSE_HZ);
    if (UINT32_MAX - s_status.sleep_ms < milliseconds) { s_status.sleep_ms = UINT32_MAX; }
    else { s_status.sleep_ms += milliseconds; }
    if (s_status.sleep_count != UINT32_MAX) { s_status.sleep_count++; }
    s_status.last_wake = wake_source;
    s_status.wake_sources |= wake_source;
    taskEXIT_CRITICAL();
}
