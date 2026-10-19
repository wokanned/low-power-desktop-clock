#include "stm32_clock.h"
#include "rtc.h"

#if defined(__CC_ARM)
extern const uint8_t Image$$RW_IRAM1$$Base[];
extern const uint8_t Image$$RW_IRAM1$$ZI$$Limit[];
extern const uint8_t Image$$RW_IRAM2$$Base[];
extern const uint8_t Image$$RW_IRAM2$$ZI$$Limit[];
extern const uint8_t Load$$LR$$LR_IROM1$$Base[];
extern const uint8_t Load$$LR$$LR_IROM1$$Limit[];
#endif

int32_t stm32_clock_read_calendar(clock_calendar_t *p_calendar)
{
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;
    HAL_StatusTypeDef time_result;
    HAL_StatusTypeDef date_result;
    if (p_calendar == NULL) { return -2; }
    time_result = HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN);
    /* Reading date unlocks the shadow registers, including failure paths. */
    date_result = HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);
    if (time_result != HAL_OK || date_result != HAL_OK) { return -1; }
    p_calendar->year = (uint16_t)(2000U + date.Year);
    p_calendar->month = date.Month;
    p_calendar->day = date.Date;
    p_calendar->weekday = date.WeekDay;
    p_calendar->hour = time.Hours;
    p_calendar->minute = time.Minutes;
    p_calendar->second = time.Seconds;
    if (!clock_calendar_valid(p_calendar)) { return -2; }
    /* The retained/default RTC weekday may disagree with its calendar date. */
    p_calendar->weekday = clock_calendar_weekday(p_calendar->year,
        p_calendar->month, p_calendar->day);
    return 0;
}

int32_t stm32_clock_image_sizes(uint32_t *p_sram_bytes, uint32_t *p_flash_bytes)
{
    if (p_sram_bytes == NULL || p_flash_bytes == NULL) { return -1; }
#if defined(__CC_ARM)
    *p_sram_bytes = (uint32_t)((uintptr_t)Image$$RW_IRAM1$$ZI$$Limit -
        (uintptr_t)Image$$RW_IRAM1$$Base) +
        (uint32_t)((uintptr_t)Image$$RW_IRAM2$$ZI$$Limit -
        (uintptr_t)Image$$RW_IRAM2$$Base);
    *p_flash_bytes = (uint32_t)((uintptr_t)Load$$LR$$LR_IROM1$$Limit -
        (uintptr_t)Load$$LR$$LR_IROM1$$Base);
    return 0;
#else
    *p_sram_bytes = 0U;
    *p_flash_bytes = 0U;
    return -1;
#endif
}
