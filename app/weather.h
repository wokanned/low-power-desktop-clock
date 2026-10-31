#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stdint.h>
#include "clock_model.h"

#define WEATHER_LINE_MAX 127U
#define WEATHER_SOURCE_TIME_LENGTH 16U

typedef enum {
    WEATHER_CITY_SHENZHEN = 0,
    WEATHER_CITY_ZHANJIANG
} weather_city_t;

typedef struct {
    weather_city_t city;
    uint8_t condition;
    int16_t temperature_tenths;
    int16_t minimum_tenths;
    int16_t maximum_tenths;
    uint8_t humidity;
    uint8_t wind_direction;
    uint16_t wind_speed_tenths;
    char source_time[WEATHER_SOURCE_TIME_LENGTH + 1U];
} weather_data_t;

typedef enum {
    WEATHER_VIEW_LIVE = 0,
    WEATHER_VIEW_FORECAST,
    WEATHER_VIEW_NO_TODAY,
    WEATHER_VIEW_CLOCK_UNAVAILABLE
} weather_view_kind_t;

typedef struct {
    weather_data_t data;
    uint32_t revision;
    uint8_t available;
    uint8_t fresh;
    uint8_t expired;
    /* Display selection only; WEATHER continues to publish a live record. */
    weather_view_kind_t kind;
    clock_calendar_t forecast_date;
} weather_snapshot_t;

/* Parses one complete WEATHER line. No state is changed on failure. */
bool weather_parse_line(const char *p_line, weather_data_t *p_data);

void weather_init(void);
/* Publish a fully validated message. now_tick is the monotonic RTOS tick. */
bool weather_publish(const weather_data_t *p_data, uint32_t now_tick);
/* Returns true only when the visible fresh/expired state changes. */
bool weather_poll_expiry(uint32_t now_tick);
void weather_get_snapshot(uint32_t now_tick, weather_snapshot_t *p_snapshot);

#endif
