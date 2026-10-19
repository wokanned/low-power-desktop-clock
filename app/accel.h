#ifndef ACCEL_H
#define ACCEL_H
#include <stdbool.h>
#include <stdint.h>
#include "clock_model.h"

#define ACCEL_SCHEMA_VERSION 1U
#define ACCEL_SAMPLE_MS 10U /* Nominal sensor period and fault-frame interval. */
#define ACCEL_WIRE_MODULUS 0x01000000U

enum {
    ACCEL_STATUS_INITIALIZED = 1U << 0,
    ACCEL_STATUS_VALID = 1U << 1,
    ACCEL_STATUS_TEMPERATURE_INVALID = 1U << 2,
    ACCEL_STATUS_NOT_READY = 1U << 3,
    ACCEL_STATUS_IO_ERROR = 1U << 4,
    ACCEL_STATUS_CONFIG_ERROR = 1U << 5,
    ACCEL_STATUS_STARTUP = 1U << 6,
    ACCEL_STATUS_OVERRUN = 1U << 7,
    ACCEL_STATUS_LATE = 1U << 8,
    ACCEL_STATUS_SATURATED = 1U << 9
};

typedef struct {
    clock_data_state_t state;
    int32_t result;
    uint32_t flags;
    uint32_t last_sample_tick;
    uint32_t first_fault_tick;
    uint32_t generation;
} accel_snapshot_t;

bool accel_start(void);
void accel_get_snapshot(accel_snapshot_t *p_snapshot);
#endif
