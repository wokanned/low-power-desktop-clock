#ifndef FORECAST_H
#define FORECAST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "clock_model.h"
#include "weather.h"

#define FORECAST_MAX_DAYS 7U
#define FORECAST_FILE_PATH "/forecast.bin"
#define FORECAST_ENCODED_MAX 79U

typedef struct {
    clock_calendar_t date;
    uint8_t condition;
    int16_t minimum_tenths;
    int16_t maximum_tenths;
} forecast_day_t;

typedef struct {
    uint32_t id;
    weather_city_t city;
    uint8_t count;
    forecast_day_t days[FORECAST_MAX_DAYS];
} forecast_batch_t;

/* Only year/month/day are used. NULL permits restoring an older saved batch. */
bool forecast_validate(const forecast_batch_t *p_batch,
                       const clock_calendar_t *p_earliest_date);
bool forecast_equal(const forecast_batch_t *p_left,
                    const forecast_batch_t *p_right);
/* Versioned, explicit little-endian bytes with IEEE CRC32; no C struct dump.
 * Outputs remain unchanged on failure. No RAM publication or IO is performed. */
bool forecast_encode(const forecast_batch_t *p_batch, uint8_t *p_data,
                     size_t capacity, size_t *p_length);
bool forecast_decode(const uint8_t *p_data, size_t length,
                     forecast_batch_t *p_batch);

void forecast_init(void);
/* Publish only after storage has committed/verified the complete file. */
bool forecast_publish(const forecast_batch_t *p_batch);
void forecast_clear(void);
bool forecast_get_batch(forecast_batch_t *p_batch);

#endif
