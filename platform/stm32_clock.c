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

static int32_t s_read_calendar(clock_calendar_t *p_calendar)
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
    return 0;
}

int32_t stm32_clock_read_calendar(clock_calendar_t *p_calendar)
{
    int32_t result = s_read_calendar(p_calendar);
    if (result != 0) { return result; }
    /* The retained/default RTC weekday may disagree with its calendar date. */
    p_calendar->weekday = clock_calendar_weekday(p_calendar->year,
        p_calendar->month, p_calendar->day);
    return 0;
}

static uint8_t s_calendar_equal(const clock_calendar_t *p_left,
                                const clock_calendar_t *p_right)
{
    return (uint8_t)(p_left->year == p_right->year &&
        p_left->month == p_right->month && p_left->day == p_right->day &&
        p_left->weekday == p_right->weekday && p_left->hour == p_right->hour &&
        p_left->minute == p_right->minute && p_left->second == p_right->second);
}

/* STM32L471 uses ISR.INIT and the STM32L4 HAL's exported private helpers.
 * Keep this version-dependent transaction here: SetTime/SetDate each exit INIT
 * separately, so they cannot publish one coherent time/date update. RCC RTC
 * initialization already enables PWR.DBP. Never reset the backup domain here. */
static HAL_StatusTypeDef s_write_calendar_registers(uint32_t time,
                                                    uint32_t date)
{
    HAL_StatusTypeDef enter_status;
    HAL_StatusTypeDef exit_status;
    __HAL_RTC_WRITEPROTECTION_DISABLE(&hrtc);
    enter_status = RTC_EnterInitMode(&hrtc);
    if (enter_status == HAL_OK) {
        hrtc.Instance->TR = time & RTC_TR_RESERVED_MASK;
        hrtc.Instance->DR = date & RTC_DR_RESERVED_MASK;
    }
    /* EnterInitMode leaves INIT set on timeout. Always leave initialization
     * and let the HAL synchronize shadow registers, with interrupts enabled. */
    exit_status = RTC_ExitInitMode(&hrtc);
    __HAL_RTC_WRITEPROTECTION_ENABLE(&hrtc);
    return enter_status != HAL_OK ? enter_status : exit_status;
}

int32_t stm32_clock_write_calendar(const clock_calendar_t *p_calendar,
                                  clock_calendar_t *p_verified)
{
    clock_calendar_t requested;
    clock_calendar_t actual;
    clock_calendar_t next;
    uint32_t time;
    uint32_t date;
    int32_t result;

    if (p_calendar == NULL || p_verified == NULL) { return -2; }
    requested = *p_calendar;
    requested.weekday = clock_calendar_weekday(requested.year,
        requested.month, requested.day);
    if (!clock_calendar_valid(&requested)) { return -2; }
    /* This board configures a 24-hour RTC. Keep CR and alarm semantics intact. */
    if (hrtc.Instance != RTC || (hrtc.Instance->CR & RTC_CR_FMT) != 0U ||
        hrtc.Lock == HAL_LOCKED) { return -1; }
    time = ((uint32_t)RTC_ByteToBcd2(requested.hour) << RTC_TR_HU_Pos) |
        ((uint32_t)RTC_ByteToBcd2(requested.minute) << RTC_TR_MNU_Pos) |
        ((uint32_t)RTC_ByteToBcd2(requested.second) << RTC_TR_SU_Pos);
    date = ((uint32_t)RTC_ByteToBcd2((uint8_t)(requested.year - 2000U)) << RTC_DR_YU_Pos) |
        ((uint32_t)RTC_ByteToBcd2(requested.month) << RTC_DR_MU_Pos) |
        ((uint32_t)RTC_ByteToBcd2(requested.day) << RTC_DR_DU_Pos) |
        ((uint32_t)requested.weekday << RTC_DR_WDU_Pos);
    hrtc.Lock = HAL_LOCKED;
    hrtc.State = HAL_RTC_STATE_BUSY;
    result = s_write_calendar_registers(time, date) == HAL_OK ? 0 : -1;
    if (result == 0) {
        result = s_read_calendar(&actual);
        if (result == -2) { result = -3; }
        if (result == 0 && !s_calendar_equal(&requested, &actual)) {
            next = requested;
            clock_calendar_add_seconds(&next, 1U);
            /* A two-digit hardware year cannot establish a new century. */
            if (next.year < requested.year || !s_calendar_equal(&next, &actual)) {
                result = -3;
            }
        }
    }
    /* An alarm IRQ may update State during the transaction; State alone is
     * never used as evidence that calendar write or readback succeeded. */
    hrtc.State = result == 0 ? HAL_RTC_STATE_READY : HAL_RTC_STATE_ERROR;
    __HAL_UNLOCK(&hrtc);
    if (result == 0) { *p_verified = actual; }
    return result;
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
