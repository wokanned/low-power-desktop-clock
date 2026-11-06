#ifndef POWER_H
#define POWER_H

#include <stdint.h>
#include <stdbool.h>

#define POWER_BOOT_WINDOW_MS 30000UL
#define POWER_MOTION_WINDOW_MS 10000UL
#define POWER_BLOCK_DEBUG   (1UL << 0)
#define POWER_BLOCK_STORAGE (1UL << 1)
#define POWER_BLOCK_DISPLAY (1UL << 2)
#define POWER_BLOCK_CLOCK   (1UL << 3)
#define POWER_BLOCK_ACCEL   (1UL << 4)
#define POWER_BLOCK_MOTION  (1UL << 5)
#define POWER_BLOCK_UART    (1UL << 6)
#define POWER_BLOCK_INPUT   (1UL << 7)
#define POWER_BLOCK_BOOT    (1UL << 16)
#define POWER_BLOCK_HOLD    (1UL << 17)
#define POWER_BLOCK_WINDOW  (1UL << 18)
#define POWER_BLOCK_FAULT   (1UL << 19)

#define POWER_FAULT_MOTION  (1UL << 0)
#define POWER_FAULT_CLOCK   (1UL << 1)
#define POWER_FAULT_TIMER   (1UL << 2)
#define POWER_WAKE_TIMER    (1UL << 0)
#define POWER_WAKE_MOTION   (1UL << 1)
#define POWER_WAKE_OTHER    (1UL << 2)

typedef enum { POWER_MODE_AUTO = 0, POWER_MODE_HOLD = 1 } power_mode_t;
typedef struct {
    power_mode_t mode;
    uint32_t blockers;
    uint32_t faults;
    uint32_t fault_count;
    uint32_t sleep_count;
    uint32_t sleep_ms;
    uint32_t last_wake;
    uint32_t wake_sources;
    uint32_t motion_count;
    uint32_t window_remaining_ms;
} power_status_t;

/* Initialize before creating tasks. All owners start with a sleep veto. */
void power_init(uint32_t now_ms);
/* Task context. Switching mode wakes accel/storage owners; no flash writes. */
void power_set_mode(power_mode_t mode);
bool power_is_hold(void);
/* Each bit has one owner; clear only at a safe transaction boundary. */
void power_set_blocker(uint32_t mask, bool active);
void power_set_fault(uint32_t mask, bool active);
void power_motion_event(uint32_t now_ms);
/* Idle-port calls with IRQs masked are supported. No IO or blocking. */
bool power_can_sleep(uint32_t now_ms);
void power_get_status(uint32_t now_ms, power_status_t *p_status);
void power_record_sleep(uint32_t elapsed_lse_counts, uint32_t wake_source);

#endif
