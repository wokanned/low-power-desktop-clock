#include "weather.h"

#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>
#include <string.h>

#define WEATHER_EXPIRY_TICKS ((uint32_t)configTICK_RATE_HZ * 10800UL)
#define WEATHER_TICK_HALF_RANGE 0x80000000UL

static weather_snapshot_t s_weather;
static uint32_t s_last_valid_tick;

static uint8_t s_digits(const char *p_text, uint8_t length, uint16_t *p_value)
{
    uint8_t i;
    uint16_t value = 0U;
    if ((p_text == NULL) || (p_value == NULL) || (length == 0U)) { return 0U; }
    for (i = 0U; i < length; i++) {
        if ((p_text[i] < '0') || (p_text[i] > '9')) { return 0U; }
        value = (uint16_t)(value * 10U + (uint16_t)(p_text[i] - '0'));
    }
    *p_value = value;
    return 1U;
}

static uint8_t s_integer(const char *p_text, uint8_t length,
                         uint8_t allow_negative, int32_t *p_value)
{
    uint8_t i = 0U;
    uint8_t negative = 0U;
    uint32_t magnitude = 0U;
    if ((p_text == NULL) || (p_value == NULL) || (length == 0U)) { return 0U; }
    if (p_text[0] == '-') {
        if ((allow_negative == 0U) || (length == 1U)) { return 0U; }
        negative = 1U;
        i = 1U;
    }
    for (; i < length; i++) {
        if ((p_text[i] < '0') || (p_text[i] > '9')) { return 0U; }
        magnitude = magnitude * 10U + (uint32_t)(p_text[i] - '0');
        if (magnitude > 9999U) { return 0U; }
    }
    *p_value = negative != 0U ? -(int32_t)magnitude : (int32_t)magnitude;
    return 1U;
}

static uint8_t s_leap_year(uint16_t year)
{
    return (uint8_t)((year % 4U == 0U) &&
                     ((year % 100U != 0U) || (year % 400U == 0U)));
}

static uint8_t s_valid_source_time(const char *p_text, uint8_t length)
{
    static const uint8_t s_days[12] = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U
    };
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t days;
    if ((length != WEATHER_SOURCE_TIME_LENGTH) ||
        (p_text[4] != '-') || (p_text[7] != '-') ||
        (p_text[10] != 'T') || (p_text[13] != ':')) { return 0U; }
    if (!s_digits(&p_text[0], 4U, &year) ||
        !s_digits(&p_text[5], 2U, &month) ||
        !s_digits(&p_text[8], 2U, &day) ||
        !s_digits(&p_text[11], 2U, &hour) ||
        !s_digits(&p_text[14], 2U, &minute)) { return 0U; }
    if ((year < 2000U) || (year > 2099U) || (month == 0U) || (month > 12U) ||
        (hour > 23U) || (minute > 59U)) { return 0U; }
    days = s_days[month - 1U];
    if ((month == 2U) && (s_leap_year(year) != 0U)) { days = 29U; }
    return (uint8_t)((day >= 1U) && (day <= days));
}

bool weather_parse_line(const char *p_line, weather_data_t *p_data)
{
    const char *fields[9];
    uint8_t lengths[9];
    const char *p_cursor;
    uint8_t i;
    uint8_t line_length = 0U;
    int32_t values[7];
    weather_data_t parsed;

    if ((p_line == NULL) || (p_data == NULL)) { return false; }
    while ((line_length <= WEATHER_LINE_MAX) && (p_line[line_length] != '\0')) {
        line_length++;
    }
    if ((line_length > WEATHER_LINE_MAX) || (line_length < 8U) ||
        (memcmp(p_line, "WEATHER ", 8U) != 0)) { return false; }

    p_cursor = &p_line[8];
    for (i = 0U; i < 9U; i++) {
        fields[i] = p_cursor;
        while ((*p_cursor != '\0') && (*p_cursor != ',')) { p_cursor++; }
        lengths[i] = (uint8_t)(p_cursor - fields[i]);
        if (lengths[i] == 0U) { return false; }
        if (i < 8U) {
            if (*p_cursor != ',') { return false; }
            p_cursor++;
        } else if (*p_cursor != '\0') {
            return false;
        }
    }

    memset(&parsed, 0, sizeof(parsed));
    if ((lengths[0] == 6U) &&
        (memcmp(fields[0], "\xE6\xB7\xB1\xE5\x9C\xB3", 6U) == 0)) {
        parsed.city = WEATHER_CITY_SHENZHEN;
    } else if ((lengths[0] == 6U) &&
               (memcmp(fields[0], "\xE6\xB9\x9B\xE6\xB1\x9F", 6U) == 0)) {
        parsed.city = WEATHER_CITY_ZHANJIANG;
    } else {
        return false;
    }
    if (!s_integer(fields[1], lengths[1], 0U, &values[0]) ||
        !s_integer(fields[2], lengths[2], 1U, &values[1]) ||
        !s_integer(fields[3], lengths[3], 1U, &values[2]) ||
        !s_integer(fields[4], lengths[4], 1U, &values[3]) ||
        !s_integer(fields[5], lengths[5], 0U, &values[4]) ||
        !s_integer(fields[6], lengths[6], 0U, &values[5]) ||
        !s_integer(fields[7], lengths[7], 0U, &values[6])) { return false; }
    if ((values[0] > 13) || (values[1] < -999) || (values[1] > 999) ||
        (values[2] < -999) || (values[2] > 999) ||
        (values[3] < -999) || (values[3] > 999) ||
        (values[4] > 100) || (values[5] > 8) || (values[6] > 999) ||
        (values[3] < values[2]) ||
        ((values[5] == 0) && (values[6] != 0)) ||
        !s_valid_source_time(fields[8], lengths[8])) { return false; }

    parsed.condition = (uint8_t)values[0];
    parsed.temperature_tenths = (int16_t)values[1];
    parsed.minimum_tenths = (int16_t)values[2];
    parsed.maximum_tenths = (int16_t)values[3];
    parsed.humidity = (uint8_t)values[4];
    parsed.wind_direction = (uint8_t)values[5];
    parsed.wind_speed_tenths = (uint16_t)values[6];
    memcpy(parsed.source_time, fields[8], WEATHER_SOURCE_TIME_LENGTH);
    parsed.source_time[WEATHER_SOURCE_TIME_LENGTH] = '\0';
    *p_data = parsed;
    return true;
}

static uint8_t s_data_equal(const weather_data_t *p_left,
                            const weather_data_t *p_right)
{
    return (uint8_t)(p_left->city == p_right->city &&
        p_left->condition == p_right->condition &&
        p_left->temperature_tenths == p_right->temperature_tenths &&
        p_left->minimum_tenths == p_right->minimum_tenths &&
        p_left->maximum_tenths == p_right->maximum_tenths &&
        p_left->humidity == p_right->humidity &&
        p_left->wind_direction == p_right->wind_direction &&
        p_left->wind_speed_tenths == p_right->wind_speed_tenths &&
        memcmp(p_left->source_time, p_right->source_time,
               WEATHER_SOURCE_TIME_LENGTH + 1U) == 0);
}

static void s_revision_bump(void)
{
    s_weather.revision++;
    if (s_weather.revision == 0U) { s_weather.revision = 1U; }
}

static uint8_t s_expire_locked(uint32_t now_tick)
{
    uint32_t elapsed = (uint32_t)(now_tick - s_last_valid_tick);
    /* A reader may sample its tick just before another task publishes.
       Ignore that older sample instead of expiring the new record. */
    if ((s_weather.available == 0U) || (s_weather.expired != 0U) ||
        (elapsed < WEATHER_EXPIRY_TICKS) || (elapsed >= WEATHER_TICK_HALF_RANGE)) {
        return 0U;
    }
    s_weather.expired = 1U;
    s_weather.fresh = 0U;
    s_revision_bump();
    return 1U;
}

static uint8_t s_data_valid(const weather_data_t *p_data)
{
    uint8_t time_length = 0U;
    if ((p_data == NULL) ||
        ((p_data->city != WEATHER_CITY_SHENZHEN) &&
         (p_data->city != WEATHER_CITY_ZHANJIANG)) ||
        (p_data->condition > 13U) ||
        (p_data->temperature_tenths < -999) ||
        (p_data->temperature_tenths > 999) ||
        (p_data->minimum_tenths < -999) || (p_data->minimum_tenths > 999) ||
        (p_data->maximum_tenths < -999) || (p_data->maximum_tenths > 999) ||
        (p_data->maximum_tenths < p_data->minimum_tenths) ||
        (p_data->humidity > 100U) || (p_data->wind_direction > 8U) ||
        (p_data->wind_speed_tenths > 999U) ||
        ((p_data->wind_direction == 0U) && (p_data->wind_speed_tenths != 0U))) {
        return 0U;
    }
    while ((time_length <= WEATHER_SOURCE_TIME_LENGTH) &&
           (p_data->source_time[time_length] != '\0')) { time_length++; }
    return (uint8_t)(time_length == WEATHER_SOURCE_TIME_LENGTH &&
        s_valid_source_time(p_data->source_time, time_length) != 0U);
}

void weather_init(void)
{
    taskENTER_CRITICAL();
    memset(&s_weather, 0, sizeof(s_weather));
    s_last_valid_tick = 0U;
    taskEXIT_CRITICAL();
}

bool weather_publish(const weather_data_t *p_data, uint32_t now_tick)
{
    uint8_t changed;
    if (!s_data_valid(p_data)) { return false; }
    taskENTER_CRITICAL();
    changed = (uint8_t)(s_weather.available == 0U || s_weather.expired != 0U ||
                        s_data_equal(&s_weather.data, p_data) == 0U);
    s_weather.data = *p_data;
    s_weather.available = 1U;
    s_weather.expired = 0U;
    s_weather.fresh = 1U;
    s_last_valid_tick = now_tick;
    if (changed != 0U) { s_revision_bump(); }
    taskEXIT_CRITICAL();
    return true;
}

bool weather_poll_expiry(uint32_t now_tick)
{
    uint8_t changed;
    taskENTER_CRITICAL();
    changed = s_expire_locked(now_tick);
    taskEXIT_CRITICAL();
    return changed != 0U;
}

void weather_get_snapshot(uint32_t now_tick, weather_snapshot_t *p_snapshot)
{
    if (p_snapshot == NULL) { return; }
    taskENTER_CRITICAL();
    (void)s_expire_locked(now_tick);
    *p_snapshot = s_weather;
    taskEXIT_CRITICAL();
}
