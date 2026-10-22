#ifndef STORAGE_H
#define STORAGE_H

#include "flash_w25q128.h"
#include "clock_model.h"

#define STORAGE_TEST_ADDRESS 0x0000F000UL
#define STORAGE_TEST_SIZE    W25Q128_SECTOR_SIZE
#define STORAGE_USAGE_INTERVAL_MS 3600000UL

/* Destructive once per boot, enabled by the selected bring-up policy. */
#ifndef STORAGE_BOOT_TEST_ENABLE
#define STORAGE_BOOT_TEST_ENABLE 1
#endif

typedef enum {
    STORAGE_TEST_NOT_RUN = 0,
    STORAGE_TEST_PASSED = 1,
    STORAGE_TEST_RUNNING = 2,
    STORAGE_TEST_SKIPPED = 3,
    STORAGE_TEST_FAILED = -1
} storage_test_status_t;

typedef struct {
    storage_test_status_t test_status;
    w25q128_status_t result;
    uint32_t jedec_id;
    w25q128_registers_t registers;
    uint8_t id_valid;
    uint8_t registers_valid;
    int32_t config_result;
    uint8_t config_degraded;
} storage_diagnostics_t;

typedef struct {
    clock_format_t format;
    uint32_t revision;
    uint8_t ready;
    uint8_t busy;
    uint32_t completed_token;
    int32_t completed_result;
} storage_config_snapshot_t;

typedef struct {
    clock_data_state_t state;
    uint32_t used_bytes;
    uint32_t total_bytes;
    w25q128_status_t result;
} storage_usage_snapshot_t;

#define STORAGE_REQUEST_OK 0
#define STORAGE_REQUEST_ARGUMENT (-1)
#define STORAGE_REQUEST_BUSY (-2)
#define STORAGE_REQUEST_UNAVAILABLE (-3)

/* Call once during RTOS initialization, before scheduler start. */
int32_t storage_start(void);
storage_test_status_t storage_get_test_status(void);
/* Task context; coherent snapshot, also retained in s_diagnostics for debugger. */
void storage_get_diagnostics(storage_diagnostics_t *p_diagnostics);
void storage_get_config_snapshot(storage_config_snapshot_t *p_snapshot);
/* RAM only, task context. INITIALIZING means a scan is in progress.
 * READY counts whole 4 KiB sectors containing any byte other than 0xFF,
 * including configuration, self-test and stale data; not filesystem usage.
 * ERROR discards the failed scan. NULL is allowed. */
void storage_get_usage_snapshot(storage_usage_snapshot_t *p_snapshot);
/* Nonblocking task-context request; use a new nonzero token. One request in flight.
 * Completion is retained until the next request completes; no acknowledgment. */
int32_t storage_request_format(clock_format_t format, uint32_t token);

#endif
