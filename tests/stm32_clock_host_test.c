#include "../platform/stm32_clock.h"
#include "rtc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

RTC_TypeDef g_fake_rtc;
RTC_HandleTypeDef hrtc;
static RTC_TypeDef s_retained;
static HAL_StatusTypeDef s_enter_status;
static HAL_StatusTypeDef s_exit_status;
static HAL_StatusTypeDef s_time_status;
static HAL_StatusTypeDef s_date_status;
static clock_calendar_t s_requested;
static clock_calendar_t s_override;
static uint8_t s_override_readback;
static uint32_t s_advance_seconds;
static uint32_t s_enter_calls;
static uint32_t s_exit_calls;
static uint32_t s_disable_calls;
static uint32_t s_enable_calls;
static char s_reads[8];
static uint8_t s_read_count;

uint8_t RTC_ByteToBcd2(uint8_t value)
{
    return (uint8_t)((value / 10U) * 16U + value % 10U);
}

static uint8_t s_from_bcd(uint32_t value)
{
    return (uint8_t)(((value >> 4U) & 15U) * 10U + (value & 15U));
}

static uint32_t s_time_register(const clock_calendar_t *p_calendar)
{
    return (uint32_t)RTC_ByteToBcd2(p_calendar->hour) << 16U |
        (uint32_t)RTC_ByteToBcd2(p_calendar->minute) << 8U |
        RTC_ByteToBcd2(p_calendar->second);
}

static uint32_t s_date_register(const clock_calendar_t *p_calendar)
{
    return (uint32_t)RTC_ByteToBcd2((uint8_t)(p_calendar->year - 2000U)) << 16U |
        (uint32_t)p_calendar->weekday << 13U |
        (uint32_t)RTC_ByteToBcd2(p_calendar->month) << 8U |
        RTC_ByteToBcd2(p_calendar->day);
}

void test_rtc_write_protection(RTC_HandleTypeDef *p_handle, uint8_t enabled)
{
    assert(p_handle == &hrtc);
    assert(p_handle->Lock == HAL_LOCKED);
    if (enabled != 0U) {
        assert((p_handle->Instance->ISR & RTC_ISR_INIT) == 0U);
        s_enable_calls++;
        p_handle->Instance->WPR = 0xFFU;
    } else {
        s_disable_calls++;
        p_handle->Instance->WPR = 0x53U;
    }
}

HAL_StatusTypeDef RTC_EnterInitMode(RTC_HandleTypeDef *p_handle)
{
    assert(p_handle->Lock == HAL_LOCKED);
    assert(p_handle->Instance->WPR == 0x53U);
    assert((p_handle->Instance->ISR & RTC_ISR_INIT) == 0U);
    p_handle->Instance->ISR |= RTC_ISR_INIT;
    if (s_enter_status == HAL_OK) { p_handle->Instance->ISR |= RTC_ISR_INITF; }
    s_enter_calls++;
    /* AlarmIRQ may update State even while the calendar transaction fails. */
    p_handle->State = HAL_RTC_STATE_READY;
    return s_enter_status;
}

HAL_StatusTypeDef RTC_ExitInitMode(RTC_HandleTypeDef *p_handle)
{
    clock_calendar_t advanced;
    assert(p_handle->Lock == HAL_LOCKED);
    assert(p_handle->Instance->WPR == 0x53U);
    assert((p_handle->Instance->ISR & RTC_ISR_INIT) != 0U);
    if (s_enter_status == HAL_OK) {
        /* Both complete BCD registers must be present before the sole exit. */
        assert(p_handle->Instance->TR == s_time_register(&s_requested));
        assert(p_handle->Instance->DR == s_date_register(&s_requested));
        if (s_advance_seconds != 0U) {
            advanced = s_requested;
            clock_calendar_add_seconds(&advanced, s_advance_seconds);
            p_handle->Instance->TR = s_time_register(&advanced);
            p_handle->Instance->DR = s_date_register(&advanced);
        }
    } else {
        assert(p_handle->Instance->TR == s_retained.TR);
        assert(p_handle->Instance->DR == s_retained.DR);
    }
    p_handle->Instance->ISR &= ~(RTC_ISR_INIT | RTC_ISR_INITF);
    p_handle->State = HAL_RTC_STATE_READY;
    s_exit_calls++;
    return s_exit_status;
}

HAL_StatusTypeDef HAL_RTC_GetTime(RTC_HandleTypeDef *p_handle,
    RTC_TimeTypeDef *p_time, uint32_t format)
{
    assert(format == RTC_FORMAT_BIN);
    assert(s_read_count < sizeof(s_reads) - 1U);
    assert(p_handle->Instance->WPR == 0xFFU);
    assert((p_handle->Instance->ISR & RTC_ISR_INIT) == 0U);
    s_reads[s_read_count++] = 'T';
    p_time->Hours = s_from_bcd(p_handle->Instance->TR >> 16U);
    p_time->Minutes = s_from_bcd(p_handle->Instance->TR >> 8U);
    p_time->Seconds = s_from_bcd(p_handle->Instance->TR);
    if (s_override_readback != 0U) {
        p_time->Hours = s_override.hour;
        p_time->Minutes = s_override.minute;
        p_time->Seconds = s_override.second;
    }
    return s_time_status;
}

HAL_StatusTypeDef HAL_RTC_GetDate(RTC_HandleTypeDef *p_handle,
    RTC_DateTypeDef *p_date, uint32_t format)
{
    assert(format == RTC_FORMAT_BIN);
    assert(s_read_count != 0U && s_reads[s_read_count - 1U] == 'T');
    assert(s_read_count < sizeof(s_reads) - 1U);
    s_reads[s_read_count++] = 'D';
    p_date->Year = s_from_bcd(p_handle->Instance->DR >> 16U);
    p_date->Month = s_from_bcd((p_handle->Instance->DR >> 8U) & 31U);
    p_date->Date = s_from_bcd(p_handle->Instance->DR & 63U);
    p_date->WeekDay = (uint8_t)((p_handle->Instance->DR >> 13U) & 7U);
    if (s_override_readback != 0U) {
        p_date->Year = (uint8_t)(s_override.year - 2000U);
        p_date->Month = s_override.month;
        p_date->Date = s_override.day;
        p_date->WeekDay = s_override.weekday;
    }
    return s_date_status;
}

static void s_reset(void)
{
    memset(&g_fake_rtc, 0x5A, sizeof(g_fake_rtc));
    g_fake_rtc.CR &= ~RTC_CR_FMT;
    g_fake_rtc.ISR &= ~(RTC_ISR_INIT | RTC_ISR_INITF);
    g_fake_rtc.WPR = 0xFFU;
    g_fake_rtc.BKP0R = 0x52544331U;
    g_fake_rtc.PRER = 0x007F00FFU;
    s_retained = g_fake_rtc;
    hrtc.Instance = RTC;
    hrtc.Lock = HAL_UNLOCKED;
    hrtc.State = HAL_RTC_STATE_READY;
    s_enter_status = HAL_OK;
    s_exit_status = HAL_OK;
    s_time_status = HAL_OK;
    s_date_status = HAL_OK;
    s_enter_calls = 0U;
    s_exit_calls = 0U;
    s_disable_calls = 0U;
    s_enable_calls = 0U;
    s_advance_seconds = 0U;
    s_override_readback = 0U;
    memset(s_reads, 0, sizeof(s_reads));
    s_read_count = 0U;
    s_requested = (clock_calendar_t){2026U, 10U, 27U, 2U, 13U, 14U, 15U};
}

static void s_check_retained(void)
{
    assert(g_fake_rtc.CR == s_retained.CR);
    assert(g_fake_rtc.PRER == s_retained.PRER);
    assert(g_fake_rtc.WUTR == s_retained.WUTR);
    assert(g_fake_rtc.CALIBR == s_retained.CALIBR);
    assert(g_fake_rtc.ALRMAR == s_retained.ALRMAR);
    assert(g_fake_rtc.ALRMBR == s_retained.ALRMBR);
    assert(g_fake_rtc.ALRMASSR == s_retained.ALRMASSR);
    assert(g_fake_rtc.ALRMBSSR == s_retained.ALRMBSSR);
    assert(g_fake_rtc.BKP0R == s_retained.BKP0R);
    assert(g_fake_rtc.BKP1R == s_retained.BKP1R);
    assert(g_fake_rtc.CALR == s_retained.CALR);
    assert(g_fake_rtc.TAFCR == s_retained.TAFCR);
    assert(g_fake_rtc.OR == s_retained.OR);
    assert(hrtc.Lock == HAL_UNLOCKED);
    assert(g_fake_rtc.WPR == 0xFFU);
    assert((g_fake_rtc.ISR & RTC_ISR_INIT) == 0U);
}

static void s_check_transaction(int32_t expected)
{
    clock_calendar_t output;
    clock_calendar_t sentinel;
    clock_calendar_t request;
    memset(&sentinel, 0xA5, sizeof(sentinel));
    output = sentinel;
    request = s_requested;
    request.weekday = 0U; /* Caller supplies a date, not a trusted weekday. */
    assert(stm32_clock_write_calendar(&request, &output) == expected);
    assert(s_enter_calls == 1U && s_exit_calls == 1U);
    assert(s_disable_calls == 1U && s_enable_calls == 1U);
    s_check_retained();
    if (expected == 0) {
        request.weekday = s_requested.weekday;
        if (s_advance_seconds != 0U) {
            clock_calendar_add_seconds(&request, 1U);
        }
        assert(clock_calendar_valid(&output));
        assert(output.year == request.year && output.month == request.month &&
            output.day == request.day && output.hour == request.hour &&
            output.minute == request.minute && output.second == request.second);
        assert(output.weekday == clock_calendar_weekday(output.year,
            output.month, output.day));
        assert(strcmp(s_reads, "TD") == 0);
        assert(hrtc.State == HAL_RTC_STATE_READY);
    } else {
        assert(memcmp(&output, &sentinel, sizeof(output)) == 0);
        assert(hrtc.State == HAL_RTC_STATE_ERROR);
    }
}

static void s_test_success_boundaries(void)
{
    static const clock_calendar_t s_cases[] = {
        {2026U, 10U, 27U, 2U, 13U, 14U, 15U},
        {2026U, 10U, 27U, 2U, 23U, 59U, 59U},
        {2028U, 2U, 28U, 1U, 23U, 59U, 59U},
        {2028U, 2U, 29U, 2U, 23U, 59U, 59U},
        {2026U, 12U, 31U, 4U, 23U, 59U, 59U},
        {2000U, 1U, 1U, 6U, 0U, 0U, 0U},
        {2099U, 12U, 31U, 4U, 23U, 59U, 59U}
    };
    size_t i;
    for (i = 0U; i < sizeof(s_cases) / sizeof(s_cases[0]); i++) {
        s_reset();
        s_requested = s_cases[i];
        s_check_transaction(0);
        s_reset();
        s_requested = s_cases[i];
        s_advance_seconds = 1U;
        s_check_transaction(i == sizeof(s_cases) / sizeof(s_cases[0]) - 1U ? -3 : 0);
    }
}

static void s_test_invalid_input(void)
{
    static const clock_calendar_t s_cases[] = {
        {1999U, 12U, 31U, 1U, 0U, 0U, 0U},
        {2100U, 1U, 1U, 1U, 0U, 0U, 0U},
        {2026U, 0U, 1U, 1U, 0U, 0U, 0U},
        {2026U, 13U, 1U, 1U, 0U, 0U, 0U},
        {2026U, 2U, 29U, 1U, 0U, 0U, 0U},
        {2028U, 2U, 30U, 1U, 0U, 0U, 0U},
        {2026U, 4U, 31U, 1U, 0U, 0U, 0U},
        {2026U, 10U, 0U, 1U, 0U, 0U, 0U},
        {2026U, 10U, 27U, 1U, 24U, 0U, 0U},
        {2026U, 10U, 27U, 1U, 0U, 60U, 0U},
        {2026U, 10U, 27U, 1U, 0U, 0U, 60U}
    };
    clock_calendar_t output;
    size_t i;
    for (i = 0U; i < sizeof(s_cases) / sizeof(s_cases[0]); i++) {
        s_reset();
        assert(stm32_clock_write_calendar(&s_cases[i], &output) == -2);
        assert(memcmp(&g_fake_rtc, &s_retained, sizeof(g_fake_rtc)) == 0);
        assert(s_enter_calls == 0U && s_disable_calls == 0U);
    }
    assert(stm32_clock_write_calendar(NULL, &output) == -2);
    assert(stm32_clock_write_calendar(&s_requested, NULL) == -2);
    s_reset();
    hrtc.Lock = HAL_LOCKED;
    assert(stm32_clock_write_calendar(&s_requested, &output) == -1);
    assert(hrtc.Lock == HAL_LOCKED && s_enter_calls == 0U);
    s_reset();
    g_fake_rtc.CR |= RTC_CR_FMT;
    assert(stm32_clock_write_calendar(&s_requested, &output) == -1);
    assert((g_fake_rtc.CR & RTC_CR_FMT) != 0U && s_enter_calls == 0U);
    s_reset();
    hrtc.Instance = NULL;
    assert(stm32_clock_write_calendar(&s_requested, &output) == -1);
    assert(s_enter_calls == 0U);
}

static void s_retry_after_failure(void)
{
    clock_calendar_t output;
    s_enter_status = HAL_OK;
    s_exit_status = HAL_OK;
    s_time_status = HAL_OK;
    s_date_status = HAL_OK;
    s_override_readback = 0U;
    s_advance_seconds = 0U;
    memset(s_reads, 0, sizeof(s_reads));
    s_read_count = 0U;
    assert(stm32_clock_write_calendar(&s_requested, &output) == 0);
    s_check_retained();
    assert(strcmp(s_reads, "TD") == 0);
}

static void s_test_failures_and_retry(void)
{
    s_reset();
    s_enter_status = HAL_TIMEOUT;
    s_check_transaction(-1);
    assert(s_read_count == 0U);
    s_retry_after_failure();
    s_reset();
    s_exit_status = HAL_TIMEOUT;
    s_check_transaction(-1);
    assert(s_read_count == 0U);
    s_retry_after_failure();
    s_reset();
    s_time_status = HAL_ERROR;
    s_check_transaction(-1);
    assert(strcmp(s_reads, "TD") == 0);
    s_retry_after_failure();
    s_reset();
    s_date_status = HAL_ERROR;
    s_check_transaction(-1);
    assert(strcmp(s_reads, "TD") == 0);
    s_retry_after_failure();
    s_reset();
    s_advance_seconds = 2U;
    s_check_transaction(-3);
    s_retry_after_failure();
    s_reset();
    s_override_readback = 1U;
    s_override = s_requested;
    s_override.day = 0U;
    s_check_transaction(-3);
    s_retry_after_failure();
    s_reset();
    s_override_readback = 1U;
    s_override = s_requested;
    s_override.second = 60U;
    s_check_transaction(-3);
    s_retry_after_failure();
    s_reset();
    s_override_readback = 1U;
    s_override = s_requested;
    s_override.weekday = 3U;
    s_check_transaction(-3);
    s_retry_after_failure();
}

static void s_test_existing_read(void)
{
    clock_calendar_t output;
    s_reset();
    g_fake_rtc.TR = s_time_register(&s_requested);
    g_fake_rtc.DR = s_date_register(&s_requested);
    g_fake_rtc.DR = (g_fake_rtc.DR & ~(7U << 13U)) | (1U << 13U);
    assert(stm32_clock_read_calendar(&output) == 0);
    assert(output.weekday == 2U);
    assert(strcmp(s_reads, "TD") == 0);
    memset(s_reads, 0, sizeof(s_reads));
    s_read_count = 0U;
    s_time_status = HAL_ERROR;
    assert(stm32_clock_read_calendar(&output) == -1);
    assert(strcmp(s_reads, "TD") == 0);
    assert(stm32_clock_read_calendar(NULL) == -2);
}

int main(void)
{
    s_test_success_boundaries();
    s_test_invalid_input();
    s_test_failures_and_retry();
    s_test_existing_read();
    puts("PASS: atomic RTC INIT calendar, verified readback, rollover, cleanup, retry and retained configuration");
    return 0;
}
