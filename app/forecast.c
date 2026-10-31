#include "forecast.h"

#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define FORECAST_VERSION 1U
#define FORECAST_HEADER_BYTES 12U
#define FORECAST_DAY_BYTES 9U
#define FORECAST_CRC_BYTES 4U
#define FORECAST_CRC_POLYNOMIAL 0xEDB88320UL

static forecast_batch_t s_forecast;
static uint8_t s_available;

static uint8_t s_month_days(uint16_t year, uint8_t month)
{
    static const uint8_t s_days[] = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U
    };
    if (month == 0U || month > 12U) { return 0U; }
    return (uint8_t)(s_days[month - 1U] +
        (uint8_t)(month == 2U && year % 4U == 0U));
}

static uint8_t s_date_valid(const clock_calendar_t *p_date)
{
    return (uint8_t)(p_date != NULL && p_date->year >= 2000U &&
        p_date->year <= 2099U && p_date->month >= 1U && p_date->month <= 12U &&
        p_date->day >= 1U && p_date->day <= s_month_days(p_date->year, p_date->month));
}

static uint32_t s_date_key(const clock_calendar_t *p_date)
{
    return (uint32_t)p_date->year * 10000UL +
        (uint32_t)p_date->month * 100UL + p_date->day;
}

static uint8_t s_next_date(clock_calendar_t *p_date)
{
    if (p_date->day < s_month_days(p_date->year, p_date->month)) {
        p_date->day++;
    } else if (p_date->month < 12U) {
        p_date->day = 1U;
        p_date->month++;
    } else if (p_date->year < 2099U) {
        p_date->day = 1U;
        p_date->month = 1U;
        p_date->year++;
    } else { return 0U; }
    return 1U;
}

bool forecast_validate(const forecast_batch_t *p_batch,
                       const clock_calendar_t *p_earliest_date)
{
    clock_calendar_t expected;
    uint8_t i;
    const forecast_day_t *p_day;
    if (p_batch == NULL || p_batch->id == 0U || p_batch->count == 0U ||
        p_batch->count > FORECAST_MAX_DAYS ||
        (p_batch->city != WEATHER_CITY_SHENZHEN &&
         p_batch->city != WEATHER_CITY_ZHANJIANG)) { return false; }
    if (p_earliest_date != NULL && !s_date_valid(p_earliest_date)) { return false; }
    expected = p_batch->days[0].date;
    if (!s_date_valid(&expected) || (p_earliest_date != NULL &&
        s_date_key(&expected) < s_date_key(p_earliest_date))) { return false; }
    for (i = 0U; i < p_batch->count; i++) {
        p_day = &p_batch->days[i];
        if (!s_date_valid(&p_day->date) ||
            s_date_key(&p_day->date) != s_date_key(&expected) ||
            p_day->condition > 13U || p_day->minimum_tenths < -999 ||
            p_day->minimum_tenths > 999 || p_day->maximum_tenths < -999 ||
            p_day->maximum_tenths > 999 ||
            p_day->minimum_tenths > p_day->maximum_tenths) { return false; }
        if (i + 1U < p_batch->count && !s_next_date(&expected)) { return false; }
    }
    return true;
}

bool forecast_equal(const forecast_batch_t *p_left,
                    const forecast_batch_t *p_right)
{
    uint8_t i;
    if (p_left == NULL || p_right == NULL || p_left->count > FORECAST_MAX_DAYS ||
        p_left->id != p_right->id || p_left->city != p_right->city ||
        p_left->count != p_right->count) { return false; }
    for (i = 0U; i < p_left->count; i++) {
        if (s_date_key(&p_left->days[i].date) != s_date_key(&p_right->days[i].date) ||
            p_left->days[i].condition != p_right->days[i].condition ||
            p_left->days[i].minimum_tenths != p_right->days[i].minimum_tenths ||
            p_left->days[i].maximum_tenths != p_right->days[i].maximum_tenths) {
            return false;
        }
    }
    return true;
}

static void s_put_u16(uint8_t *p_data, uint16_t value)
{
    p_data[0] = (uint8_t)value;
    p_data[1] = (uint8_t)(value >> 8U);
}

static uint16_t s_get_u16(const uint8_t *p_data)
{
    return (uint16_t)((uint16_t)p_data[0] | ((uint16_t)p_data[1] << 8U));
}

static int16_t s_get_i16(const uint8_t *p_data)
{
    uint16_t value = s_get_u16(p_data);
    return value <= 32767U ? (int16_t)value : (int16_t)(-(int32_t)(65536UL - value));
}

static void s_put_u32(uint8_t *p_data, uint32_t value)
{
    p_data[0] = (uint8_t)value;
    p_data[1] = (uint8_t)(value >> 8U);
    p_data[2] = (uint8_t)(value >> 16U);
    p_data[3] = (uint8_t)(value >> 24U);
}

static uint32_t s_get_u32(const uint8_t *p_data)
{
    return (uint32_t)p_data[0] | ((uint32_t)p_data[1] << 8U) |
        ((uint32_t)p_data[2] << 16U) | ((uint32_t)p_data[3] << 24U);
}

static uint32_t s_crc32(const uint8_t *p_data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint8_t bit;
    size_t i;
    for (i = 0U; i < length; i++) {
        crc ^= p_data[i];
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? FORECAST_CRC_POLYNOMIAL : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

bool forecast_encode(const forecast_batch_t *p_batch, uint8_t *p_data,
                     size_t capacity, size_t *p_length)
{
    size_t length;
    size_t offset;
    uint8_t i;
    const forecast_day_t *p_day;
    if (p_data == NULL || p_length == NULL || !forecast_validate(p_batch, NULL)) {
        return false;
    }
    length = FORECAST_HEADER_BYTES + (size_t)p_batch->count * FORECAST_DAY_BYTES +
        FORECAST_CRC_BYTES;
    if (capacity < length) { return false; }
    memcpy(p_data, "DCFC", 4U);
    p_data[4] = FORECAST_VERSION;
    p_data[5] = (uint8_t)p_batch->city;
    p_data[6] = p_batch->count;
    p_data[7] = 0U;
    s_put_u32(&p_data[8], p_batch->id);
    for (i = 0U; i < p_batch->count; i++) {
        offset = FORECAST_HEADER_BYTES + (size_t)i * FORECAST_DAY_BYTES;
        p_day = &p_batch->days[i];
        s_put_u16(&p_data[offset], p_day->date.year);
        p_data[offset + 2U] = p_day->date.month;
        p_data[offset + 3U] = p_day->date.day;
        p_data[offset + 4U] = p_day->condition;
        s_put_u16(&p_data[offset + 5U], (uint16_t)p_day->minimum_tenths);
        s_put_u16(&p_data[offset + 7U], (uint16_t)p_day->maximum_tenths);
    }
    s_put_u32(&p_data[length - FORECAST_CRC_BYTES], s_crc32(p_data, length - FORECAST_CRC_BYTES));
    *p_length = length;
    return true;
}

bool forecast_decode(const uint8_t *p_data, size_t length,
                     forecast_batch_t *p_batch)
{
    forecast_batch_t decoded;
    size_t offset;
    uint8_t i;
    if (p_data == NULL || p_batch == NULL || length < FORECAST_HEADER_BYTES +
        FORECAST_DAY_BYTES + FORECAST_CRC_BYTES || length > FORECAST_ENCODED_MAX ||
        memcmp(p_data, "DCFC", 4U) != 0 || p_data[4] != FORECAST_VERSION ||
        p_data[7] != 0U || p_data[6] == 0U || p_data[6] > FORECAST_MAX_DAYS ||
        length != FORECAST_HEADER_BYTES + (size_t)p_data[6] * FORECAST_DAY_BYTES +
            FORECAST_CRC_BYTES ||
        s_get_u32(&p_data[length - FORECAST_CRC_BYTES]) !=
            s_crc32(p_data, length - FORECAST_CRC_BYTES)) { return false; }
    memset(&decoded, 0, sizeof(decoded));
    decoded.id = s_get_u32(&p_data[8]);
    decoded.city = (weather_city_t)p_data[5];
    decoded.count = p_data[6];
    for (i = 0U; i < decoded.count; i++) {
        offset = FORECAST_HEADER_BYTES + (size_t)i * FORECAST_DAY_BYTES;
        decoded.days[i].date.year = s_get_u16(&p_data[offset]);
        decoded.days[i].date.month = p_data[offset + 2U];
        decoded.days[i].date.day = p_data[offset + 3U];
        decoded.days[i].condition = p_data[offset + 4U];
        decoded.days[i].minimum_tenths = s_get_i16(&p_data[offset + 5U]);
        decoded.days[i].maximum_tenths = s_get_i16(&p_data[offset + 7U]);
    }
    if (!forecast_validate(&decoded, NULL)) { return false; }
    for (i = 0U; i < decoded.count; i++) {
        decoded.days[i].date.weekday = clock_calendar_weekday(decoded.days[i].date.year,
            decoded.days[i].date.month, decoded.days[i].date.day);
    }
    *p_batch = decoded;
    return true;
}

void forecast_init(void)
{
    forecast_clear();
}

bool forecast_publish(const forecast_batch_t *p_batch)
{
    forecast_batch_t normalized;
    uint8_t i;
    if (!forecast_validate(p_batch, NULL)) { return false; }
    memset(&normalized, 0, sizeof(normalized));
    normalized.id = p_batch->id;
    normalized.city = p_batch->city;
    normalized.count = p_batch->count;
    for (i = 0U; i < normalized.count; i++) {
        normalized.days[i] = p_batch->days[i];
        normalized.days[i].date.hour = 0U;
        normalized.days[i].date.minute = 0U;
        normalized.days[i].date.second = 0U;
        normalized.days[i].date.weekday = clock_calendar_weekday(
            normalized.days[i].date.year, normalized.days[i].date.month,
            normalized.days[i].date.day);
    }
    taskENTER_CRITICAL();
    s_forecast = normalized;
    s_available = 1U;
    taskEXIT_CRITICAL();
    return true;
}

void forecast_clear(void)
{
    taskENTER_CRITICAL();
    memset(&s_forecast, 0, sizeof(s_forecast));
    s_available = 0U;
    taskEXIT_CRITICAL();
}

bool forecast_get_batch(forecast_batch_t *p_batch)
{
    uint8_t available;
    if (p_batch == NULL) { return false; }
    taskENTER_CRITICAL();
    available = s_available;
    *p_batch = s_forecast;
    taskEXIT_CRITICAL();
    return available != 0U;
}
