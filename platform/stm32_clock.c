#include "stm32_clock.h"
#include "rtc.h"

#define CLOCK_PRECISE_PRER 0x007F00FFUL
#define CLOCK_PRECISE_READ_MS 4U
#define CLOCK_PRECISE_OPERATION_MS 50U
#define CLOCK_PRECISE_VERIFY_MS 20
#define CLOCK_PRECISE_SHIFT_ATTEMPTS 2U
#define CLOCK_MILLISECONDS_PER_SECOND 1000U
#define CLOCK_SYNCHRONIZED_MARKER 0x53594E31UL

uint8_t stm32_clock_is_synchronized(void)
{
    return (uint8_t)(hrtc.Instance == RTC &&
        HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1) == CLOCK_SYNCHRONIZED_MARKER);
}

int32_t stm32_clock_set_synchronized(uint8_t synchronized)
{
    uint32_t marker = synchronized != 0U ? CLOCK_SYNCHRONIZED_MARKER : 0U;
    if (hrtc.Instance != RTC) { return -1; }
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, marker);
    return HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1) == marker ? 0 : -1;
}

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

int32_t stm32_clock_read_timestamp(uint32_t tick_ms, clock_timestamp_t *p_sample)
{
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;
    clock_timestamp_t sample;
    uint32_t started;
    uint32_t span;
    HAL_StatusTypeDef time_result;
    HAL_StatusTypeDef date_result;
    if (p_sample == NULL) { return -2; }
    if (hrtc.Instance != RTC || (hrtc.Instance->CR & (RTC_CR_BYPSHAD | RTC_CR_FMT)) != 0U ||
        hrtc.Instance->PRER != CLOCK_PRECISE_PRER) { return -1; }
    started = HAL_GetTick();
    time_result = HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN);
    date_result = HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);
    span = HAL_GetTick() - started;
    if (time_result != HAL_OK || date_result != HAL_OK) { return -1; }
    if (span > CLOCK_PRECISE_READ_MS) { return -4; }
    sample.calendar.year = (uint16_t)(2000U + date.Year);
    sample.calendar.month = date.Month;
    sample.calendar.day = date.Date;
    sample.calendar.weekday = clock_calendar_weekday(sample.calendar.year,
        sample.calendar.month, sample.calendar.day);
    sample.calendar.hour = time.Hours;
    sample.calendar.minute = time.Minutes;
    sample.calendar.second = time.Seconds;
    if (!clock_calendar_valid(&sample.calendar) || time.SecondFraction != 255U ||
        time.SubSeconds > time.SecondFraction) { return -2; }
    sample.millisecond = (uint16_t)((time.SecondFraction - time.SubSeconds) *
        CLOCK_MILLISECONDS_PER_SECOND / (time.SecondFraction + 1U));
    sample.tick_ms = tick_ms;
    sample.read_span_ms = (uint16_t)span;
    *p_sample = sample;
    return 0;
}

/* Gregorian seconds from 2000; the supported 100 years fit in uint32_t. */
static uint32_t s_calendar_seconds(const clock_calendar_t *p_calendar)
{
    static const uint8_t s_days[] = {31U,28U,31U,30U,31U,30U,31U,31U,30U,31U,30U,31U};
    uint32_t days = p_calendar->day - 1U;
    uint16_t year;
    uint8_t month;
    for (year = 2000U; year < p_calendar->year; year++) {
        days += year % 4U == 0U ? 366U : 365U;
    }
    for (month = 1U; month < p_calendar->month; month++) {
        days += s_days[month - 1U];
        if (month == 2U && p_calendar->year % 4U == 0U) { days++; }
    }
    return ((days * 24U + p_calendar->hour) * 60U + p_calendar->minute) * 60U +
        p_calendar->second;
}

static int32_t s_precise_difference(const clock_timestamp_t *p_reference,
                                   uint32_t elapsed_ms,
                                   const clock_timestamp_t *p_actual)
{
    int64_t difference = ((int64_t)s_calendar_seconds(&p_reference->calendar) -
        (int64_t)s_calendar_seconds(&p_actual->calendar)) * 1000 +
        p_reference->millisecond + elapsed_ms - p_actual->millisecond;
    if (difference < -2000 || difference > 2000) { return 2001; }
    return (int32_t)difference;
}

int32_t stm32_clock_write_timestamp(const clock_timestamp_t *p_requested,
                                   uint32_t tick_ms,
                                   clock_timestamp_t *p_verified)
{
    clock_timestamp_t requested;
    clock_timestamp_t actual;
    clock_calendar_t calendar;
    uint32_t started;
    uint32_t sampled;
    uint32_t fractions;
    uint32_t shift_ms;
    uint32_t attempt;
    int32_t difference;
    int32_t result;
    if (p_requested == NULL || p_verified == NULL ||
        p_requested->millisecond >= CLOCK_MILLISECONDS_PER_SECOND) { return -2; }
    requested = *p_requested;
    requested.calendar.weekday = clock_calendar_weekday(requested.calendar.year,
        requested.calendar.month, requested.calendar.day);
    if (!clock_calendar_valid(&requested.calendar)) { return -2; }
    if (hrtc.Instance != RTC || hrtc.Instance->PRER != CLOCK_PRECISE_PRER ||
        (hrtc.Instance->CR & (RTC_CR_REFCKON | RTC_CR_BYPSHAD)) != 0U ||
        (hrtc.Instance->ISR & RTC_ISR_SHPF) != 0U) { return -1; }
    started = HAL_GetTick();
    /* INIT resets the synchronous divider. The following observations include
     * time spent entering INIT, shadow synchronization and shift application. */
    result = stm32_clock_write_calendar(&requested.calendar, &calendar);
    if (result != 0) { return result; }
    for (attempt = 0U; attempt < CLOCK_PRECISE_SHIFT_ATTEMPTS; attempt++) {
        sampled = HAL_GetTick() - started;
        if (sampled > CLOCK_PRECISE_OPERATION_MS) { return -4; }
        result = stm32_clock_read_timestamp(tick_ms + sampled, &actual);
        if (result != 0) { return result; }
        difference = s_precise_difference(&requested, sampled, &actual);
        if (difference >= -CLOCK_PRECISE_VERIFY_MS &&
            difference <= CLOCK_PRECISE_VERIFY_MS) {
            *p_verified = actual;
            return 0;
        }
        if (difference < 0 || difference > 2000) { return -3; }
        shift_ms = (uint32_t)difference > CLOCK_MILLISECONDS_PER_SECOND ?
            CLOCK_MILLISECONDS_PER_SECOND : (uint32_t)difference;
        fractions = (shift_ms * 256U + 500U) / CLOCK_MILLISECONDS_PER_SECOND;
        if (HAL_RTCEx_SetSynchroShift(&hrtc, RTC_SHIFTADD1S_SET,
            256U - fractions) != HAL_OK) { return -1; }
        while ((hrtc.Instance->ISR & RTC_ISR_SHPF) != 0U) {
            if (HAL_GetTick() - started > CLOCK_PRECISE_OPERATION_MS) { return -4; }
        }
        if (HAL_RTC_WaitForSynchro(&hrtc) != HAL_OK) { return -1; }
    }
    sampled = HAL_GetTick() - started;
    if (sampled > CLOCK_PRECISE_OPERATION_MS) { return -4; }
    result = stm32_clock_read_timestamp(tick_ms + sampled, &actual);
    if (result != 0) { return result; }
    difference = s_precise_difference(&requested, sampled, &actual);
    if (difference < -CLOCK_PRECISE_VERIFY_MS ||
        difference > CLOCK_PRECISE_VERIFY_MS) { return -3; }
    *p_verified = actual;
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
