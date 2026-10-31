#ifndef STORAGE_FORECAST_H
#define STORAGE_FORECAST_H

#include "forecast.h"
#include <stdint.h>

typedef struct {
    uint8_t ready; /* Initial restore finished, including absent/unformatted media. */
    uint8_t busy;  /* Maintenance ownership or an unconsumed forecast request/result. */
    int32_t result; /* Restore or most recent save result, in file_store domain. */
    uint32_t clear_generation; /* Advances after successful explicit FS format. */
} storage_forecast_snapshot_t;

/* Copies a validated complete candidate. Success means accepted, not persisted.
 * storageTask publishes only after durable commit. No RTC or display changes.
 * One nonzero-token request/result at a time; task context, no borrowed buffers. */
int32_t storage_forecast_request(const forecast_batch_t *p_batch, uint32_t token);
uint8_t storage_forecast_take_result(uint32_t token, int32_t *p_result);
void storage_forecast_get_snapshot(storage_forecast_snapshot_t *p_snapshot);

#endif
