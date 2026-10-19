#include "clock_model.h"

static uint8_t s_month_days(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {31U,28U,31U,30U,31U,30U,31U,31U,30U,31U,30U,31U};
    if (month == 0U || month > 12U) { return 0U; }
    return (uint8_t)(days[month - 1U] +
        ((month == 2U && year % 4U == 0U) ? 1U : 0U));
}

uint8_t clock_calendar_valid(const clock_calendar_t *p_calendar)
{
    return (uint8_t)(p_calendar != NULL && p_calendar->year >= 2000U &&
        p_calendar->year <= 2099U && p_calendar->month >= 1U &&
        p_calendar->month <= 12U && p_calendar->day >= 1U &&
        p_calendar->day <= s_month_days(p_calendar->year, p_calendar->month) &&
        p_calendar->hour < 24U && p_calendar->minute < 60U &&
        p_calendar->second < 60U && p_calendar->weekday >= 1U &&
        p_calendar->weekday <= 7U);
}

uint8_t clock_calendar_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    uint32_t days;
    uint16_t current;
    uint8_t index;
    if (year < 2000U || year > 2099U || month == 0U || month > 12U ||
        day == 0U || day > s_month_days(year, month)) { return 0U; }
    days = day - 1U;
    for (current = 2000U; current < year; current++) {
        days += (current % 4U == 0U) ? 366U : 365U;
    }
    for (index = 1U; index < month; index++) { days += s_month_days(year, index); }
    return (uint8_t)((days + 5U) % 7U + 1U);
}

void clock_calendar_add_seconds(clock_calendar_t *p_calendar, uint32_t seconds)
{
    uint32_t total;
    uint32_t days;
    if (!clock_calendar_valid(p_calendar)) { return; }
    /* Callers use short monotonic deltas, not arbitrary multi-century spans. */
    total = p_calendar->second + seconds;
    p_calendar->second = (uint8_t)(total % 60U);
    total = p_calendar->minute + total / 60U;
    p_calendar->minute = (uint8_t)(total % 60U);
    total = p_calendar->hour + total / 60U;
    p_calendar->hour = (uint8_t)(total % 24U);
    days = total / 24U;
    while (days != 0U) {
        p_calendar->weekday = (uint8_t)(p_calendar->weekday % 7U + 1U);
        p_calendar->day++;
        if (p_calendar->day > s_month_days(p_calendar->year, p_calendar->month)) {
            p_calendar->day = 1U;
            p_calendar->month++;
            if (p_calendar->month > 12U) {
                p_calendar->month = 1U;
                p_calendar->year = p_calendar->year == 2099U ? 2000U :
                    (uint16_t)(p_calendar->year + 1U);
                if (p_calendar->year == 2000U) {
                    p_calendar->weekday = clock_calendar_weekday(2000U, 1U, 1U);
                }
            }
        }
        days--;
    }
}

const char *clock_weekday_label(uint8_t weekday)
{
    static const char *const labels[] = {"N/A","MON","TUE","WED","THU","FRI","SAT","SUN"};
    return labels[weekday <= 7U ? weekday : 0U];
}

const char *clock_fault_source_label(clock_fault_source_t source)
{
    static const char *const labels[] = {"RTC","TEMP","BAT","WIFI","IMU","BARO",
        "LIGHT","W25Q","CFG","TASK","DBG"};
    return (unsigned)source < CLOCK_FAULT_SOURCE_COUNT ? labels[source] : "?";
}

const char *clock_fault_category_label(clock_fault_category_t category)
{
    static const char *const labels[] = {"RTC","TEMP","BAT","WIFI","SENSOR","STORAGE","SYSTEM"};
    return (unsigned)category < CLOCK_CATEGORY_COUNT ? labels[category] : "?";
}

clock_fault_category_t clock_fault_category(clock_fault_source_t source)
{
    static const clock_fault_category_t categories[] = {
        CLOCK_CATEGORY_RTC, CLOCK_CATEGORY_TEMP, CLOCK_CATEGORY_BAT, CLOCK_CATEGORY_WIFI,
        CLOCK_CATEGORY_SENSOR, CLOCK_CATEGORY_SENSOR, CLOCK_CATEGORY_SENSOR,
        CLOCK_CATEGORY_STORAGE, CLOCK_CATEGORY_STORAGE, CLOCK_CATEGORY_SYSTEM, CLOCK_CATEGORY_SYSTEM
    };
    return (unsigned)source < CLOCK_FAULT_SOURCE_COUNT ? categories[source] : CLOCK_CATEGORY_SYSTEM;
}

const char *clock_fault_reason_label(uint32_t mask)
{
    if ((mask & CLOCK_FAULT_START) != 0U) { return "START-ERR"; }
    if ((mask & CLOCK_FAULT_TIMEOUT) != 0U) { return "TIMEOUT"; }
    if ((mask & CLOCK_FAULT_ID) != 0U) { return "ID-ERR"; }
    if ((mask & CLOCK_FAULT_VERIFY) != 0U) { return "CRC-ERR"; }
    if ((mask & CLOCK_FAULT_CONFIGURATION) != 0U) { return "CFG-ERR"; }
    if ((mask & CLOCK_FAULT_IO) != 0U) { return "IO-ERR"; }
    if ((mask & CLOCK_FAULT_TX) != 0U) { return "TX-ERR"; }
    if ((mask & CLOCK_FAULT_RX) != 0U) { return "RX-ERR"; }
    if ((mask & CLOCK_FAULT_OVERFLOW) != 0U) { return "OVERFLOW"; }
    return "ERROR";
}
