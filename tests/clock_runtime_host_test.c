#include "clock_model.h"
#include "clock_faults.h"
#include "clock_schedule.h"
#include "clock_service.h"
#include "stm32_clock.h"
#include "storage.h"
#include "task.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

static TickType_t s_ticks;
static uint32_t s_critical;
static uint8_t s_preempt_anchor;
static uint8_t s_probe_coherence;
static clock_calendar_t s_calendar = {2026U, 10U, 18U, 7U, 21U, 30U, 58U};
static int32_t s_rtc_result;
static int32_t s_image_result;
static uint32_t s_rtc_reads;
static uint32_t s_image_reads;
static uint32_t s_linked_sram;
static uint32_t s_image_flash;
static size_t s_free_heap;
static storage_config_snapshot_t s_config;
static storage_usage_snapshot_t s_usage;
static uint32_t s_usage_reads;
static uint8_t s_task_marker;
static jmp_buf s_run_exit;
static void (*s_wait_hook)(TickType_t timeout);
static uint32_t s_wait_count;
static uint32_t s_notifications;
static uint32_t s_notify_calls;
static uint32_t s_rtc_writes;
static int32_t s_write_result;
static uint8_t s_bad_verified;
static uint8_t s_preempt_notify;
static uint8_t s_probe_write;
static uint8_t s_inject_on_read;
static uint8_t s_inject_on_exit;
static TickType_t s_write_delay;
static clock_calendar_t s_requested;
static clock_calendar_t s_injected_calendar;
static TickType_t s_loop_origin;

TickType_t xTaskGetTickCount(void) { return s_ticks; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &s_task_marker; }
BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    assert(s_critical == 0U && task == &s_task_marker);
    s_notifications++;
    s_notify_calls++;
    if (s_preempt_notify != 0U) {
        s_preempt_notify = 0U;
        clock_service_poll();
    }
    return 1;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    uint32_t count;
    assert(s_critical == 0U && clear == pdTRUE && timeout > 0U && timeout <= 1000U);
    s_wait_count++;
    if (s_wait_hook == NULL) { longjmp(s_run_exit, 1); }
    s_wait_hook(timeout);
    count = s_notifications;
    s_notifications = 0U;
    if (count == 0U) { s_ticks += timeout; }
    return count;
}
size_t xPortGetFreeHeapSize(void) { return s_free_heap; }
void clock_test_enter(void)
{
    s_critical++;
    /* Model a task switch immediately before this critical section begins. */
    if (s_preempt_anchor != 0U) {
        s_preempt_anchor = 0U;
        s_ticks += 1000U;
        clock_faults_set_clock(&s_calendar, 1U);
    }
}
void clock_test_exit(void)
{
    assert(s_critical != 0U);
    s_critical--;
    if (s_critical == 0U && s_inject_on_exit != 0U) {
        s_inject_on_exit = 0U;
        assert(clock_service_request_time(&s_injected_calendar, 900U) == 0);
    }
}

int32_t stm32_clock_read_calendar(clock_calendar_t *p_calendar)
{
    assert(s_critical == 0U);
    s_rtc_reads++;
    if (s_rtc_result == 0) { *p_calendar = s_calendar; }
    if (s_inject_on_read != 0U) {
        s_inject_on_read = 0U;
        s_inject_on_exit = 1U;
    }
    return s_rtc_result;
}
int32_t stm32_clock_write_calendar(const clock_calendar_t *p_calendar,
                                   clock_calendar_t *p_verified)
{
    int32_t result = 99;
    clock_view_model_t model;
    assert(s_critical == 0U);
    assert(clock_calendar_valid(p_calendar));
    assert(p_calendar->weekday == clock_calendar_weekday(p_calendar->year,
        p_calendar->month, p_calendar->day));
    s_rtc_writes++;
    s_requested = *p_calendar;
    if (s_probe_write != 0U) {
        s_probe_write = 0U;
        assert(clock_service_take_time_result(101U, &result) == 0U && result == 99);
        assert(clock_service_request_time(p_calendar, 102U) == -1);
        clock_service_get_model(&model);
        assert(model.calendar_state == CLOCK_DATA_READY);
        assert(model.calendar.year != p_calendar->year);
    }
    /* Even failed writes can leave a plausible changed hardware calendar. */
    s_calendar = *p_calendar;
    *p_verified = s_calendar;
    if (s_bad_verified != 0U) { p_verified->day = 0U; }
    s_ticks += s_write_delay;
    return s_write_result;
}
int32_t stm32_clock_image_sizes(uint32_t *p_sram, uint32_t *p_flash)
{
    clock_view_model_t model;
    assert(s_critical == 0U);
    s_image_reads++;
    if (s_probe_coherence != 0U) {
        s_probe_coherence = 0U;
        clock_service_get_model(&model);
        assert((model.calendar_state == CLOCK_DATA_ERROR) ==
               (model.faults.source[CLOCK_FAULT_RTC].active_mask != 0U));
    }
    *p_sram = s_linked_sram; *p_flash = s_image_flash;
    return s_image_result;
}
void storage_get_config_snapshot(storage_config_snapshot_t *p_config)
{
    taskENTER_CRITICAL(); *p_config = s_config; taskEXIT_CRITICAL();
}
void storage_get_usage_snapshot(storage_usage_snapshot_t *p_usage)
{
    assert(s_critical != 0U);
    taskENTER_CRITICAL();
    s_usage_reads++;
    *p_usage = s_usage;
    taskEXIT_CRITICAL();
}

static void s_test_calendar(void)
{
    clock_calendar_t calendar = {2000U, 1U, 1U, 6U, 23U, 59U, 59U};
    uint32_t day;
    uint8_t expected_weekday;
    for (day = 0U; day < 36525U; day++) {
        expected_weekday = (uint8_t)((day + 5U) % 7U + 1U);
        assert(clock_calendar_valid(&calendar) != 0U);
        assert(calendar.weekday == expected_weekday);
        assert(clock_calendar_weekday(calendar.year, calendar.month, calendar.day) ==
               expected_weekday);
        clock_calendar_add_seconds(&calendar, day == 0U ? 1U : 86400U);
    }
    assert(calendar.year == 2000U && calendar.month == 1U && calendar.day == 1U);
    assert(calendar.weekday == 6U);
    /* RTC's two-digit year wraps to 2000, whose Jan 1 is Saturday rather
       than the Friday that a linear 2099 -> 2100 weekday increment gives. */
    calendar = (clock_calendar_t){2099U, 12U, 31U, 4U, 23U, 59U, 59U};
    clock_calendar_add_seconds(&calendar, 1U);
    assert(calendar.year == 2000U && calendar.month == 1U && calendar.day == 1U);
    assert(calendar.weekday == 6U && calendar.hour == 0U &&
           calendar.minute == 0U && calendar.second == 0U);
    clock_calendar_add_seconds(&calendar, 86400U);
    assert(calendar.year == 2000U && calendar.month == 1U &&
           calendar.day == 2U && calendar.weekday == 7U);
    /* A single multi-day addition must also resume weekday progression
       after the wrap, then honor the new century's leap day. */
    calendar = (clock_calendar_t){2099U, 12U, 30U, 3U, 23U, 59U, 58U};
    clock_calendar_add_seconds(&calendar, 2U * 86400U + 3U);
    assert(calendar.year == 2000U && calendar.month == 1U && calendar.day == 2U);
    assert(calendar.weekday == 7U && calendar.hour == 0U &&
           calendar.minute == 0U && calendar.second == 1U);
    clock_calendar_add_seconds(&calendar, 58U * 86400U);
    assert(calendar.year == 2000U && calendar.month == 2U &&
           calendar.day == 29U && calendar.weekday == 2U);
    clock_calendar_add_seconds(&calendar, 86400U);
    assert(calendar.month == 3U && calendar.day == 1U && calendar.weekday == 3U);
    calendar.month = 2U; calendar.day = 29U;
    assert(clock_calendar_valid(&calendar) != 0U);
    calendar.year = 2001U;
    assert(clock_calendar_valid(&calendar) == 0U);
    assert(clock_calendar_weekday(2001U, 2U, 29U) == 0U);
    assert(clock_calendar_weekday(2026U, 10U, 18U) == 7U);
    assert(strcmp(clock_weekday_label(8U), "N/A") == 0);
}

static void s_test_registry(void)
{
    clock_fault_snapshot_t snapshot;
    uint32_t source;
    clock_faults_init();
    s_ticks = 0U;
    clock_faults_set_clock(&s_calendar, 1U);
    s_ticks = 1000U;
    clock_fault_report(CLOCK_FAULT_IMU, CLOCK_FAULT_IO);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.revision == 1U && snapshot.active_categories == 1U);
    assert(snapshot.source[CLOCK_FAULT_IMU].first_time.second == 59U);
    assert(snapshot.source[CLOCK_FAULT_IMU].first_tick == 1000U);
    assert(snapshot.category[CLOCK_CATEGORY_SENSOR].first_tick == 1000U);
    s_ticks = 1500U;
    clock_fault_report(CLOCK_FAULT_IMU, CLOCK_FAULT_IO);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.revision == 1U);
    clock_fault_report(CLOCK_FAULT_IMU, CLOCK_FAULT_TIMEOUT);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.revision == 2U && snapshot.source[CLOCK_FAULT_IMU].first_tick == 1000U);
    assert(snapshot.source[CLOCK_FAULT_IMU].first_mask == CLOCK_FAULT_IO);
    s_ticks = 2500U;
    clock_fault_report(CLOCK_FAULT_BARO, CLOCK_FAULT_ID);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.active_categories == 1U);
    assert(snapshot.source[CLOCK_FAULT_BARO].time_valid == 0U);
    assert(snapshot.category[CLOCK_CATEGORY_SENSOR].time_valid == 1U);
    clock_fault_report(CLOCK_FAULT_IMU, 0U);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.category[CLOCK_CATEGORY_SENSOR].first_tick == 1000U);
    assert(snapshot.category[CLOCK_CATEGORY_SENSOR].source_bits == (1UL << CLOCK_FAULT_BARO));
    clock_fault_report(CLOCK_FAULT_BARO, 0U);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.active_categories == 0U);
    clock_faults_set_clock(&s_calendar, 1U);
    s_ticks = 2600U;
    clock_fault_report(CLOCK_FAULT_IMU, CLOCK_FAULT_TIMEOUT);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.category[CLOCK_CATEGORY_SENSOR].first_tick == 2600U);
    assert(snapshot.source[CLOCK_FAULT_IMU].first_mask == CLOCK_FAULT_TIMEOUT);
    clock_faults_init();
    clock_fault_report(CLOCK_FAULT_RTC, CLOCK_FAULT_IO);
    clock_faults_set_clock(&s_calendar, 1U);
    clock_fault_report(CLOCK_FAULT_RTC, CLOCK_FAULT_IO);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.source[CLOCK_FAULT_RTC].time_valid == 0U);
    clock_fault_report(CLOCK_FAULT_RTC, 0U);
    clock_fault_report(CLOCK_FAULT_RTC, CLOCK_FAULT_IO);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.source[CLOCK_FAULT_RTC].time_valid == 1U);
    clock_faults_init();
    s_ticks = 0xFFFFFFF0UL;
    clock_faults_set_clock(&s_calendar, 1U);
    s_ticks = 0x00000400UL;
    clock_fault_report(CLOCK_FAULT_TEMP, CLOCK_FAULT_IO);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.source[CLOCK_FAULT_TEMP].time_valid != 0U);
    assert(snapshot.source[CLOCK_FAULT_TEMP].first_time.second == 59U);
    clock_faults_init();
    for (source = 0U; source < CLOCK_FAULT_SOURCE_COUNT; source++) {
        clock_fault_report((clock_fault_source_t)source, CLOCK_FAULT_IO);
    }
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.active_categories == 7U && snapshot.revision == CLOCK_FAULT_SOURCE_COUNT);
    for (source = 0U; source < CLOCK_FAULT_SOURCE_COUNT; source++) {
        clock_fault_report((clock_fault_source_t)source, 0U);
    }
    clock_fault_report((clock_fault_source_t)99, CLOCK_FAULT_IO);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.active_categories == 0U && snapshot.revision == 2U * CLOCK_FAULT_SOURCE_COUNT);
    s_ticks = 5000U;
    clock_faults_set_clock(&s_calendar, 1U);
    s_preempt_anchor = 1U;
    clock_fault_report(CLOCK_FAULT_WIFI, CLOCK_FAULT_IO);
    clock_faults_get_snapshot(&snapshot);
    assert(snapshot.source[CLOCK_FAULT_WIFI].time_valid != 0U);
}

static void s_test_schedule(void)
{
    clock_schedule_t schedule;
    clock_schedule_t before;
    clock_view_model_t model;
    (void)memset(&model, 0, sizeof(model));
    model.calendar = s_calendar;
    model.calendar_state = CLOCK_DATA_READY;
    model.format = CLOCK_FORMAT_24;
    clock_schedule_init(&schedule);
    assert(clock_schedule_due(&schedule, &model, 0U) == 0U);
    model.startup_ready = 1U;
    assert(clock_schedule_due(&schedule, &model, 0U) != 0U);
    before = schedule;
    /* A failed frame has no commit and cannot advance rotation or revisions. */
    assert(clock_schedule_due(&schedule, &model, 1000U) != 0U);
    assert(memcmp(&before, &schedule, sizeof(schedule)) == 0);
    model.memory_mode = schedule.next_memory;
    clock_schedule_commit(&schedule, &model, 1000U);
    assert(schedule.next_memory == CLOCK_MEMORY_IFLASH);
    model.calendar.second = 59U;
    assert(clock_schedule_due(&schedule, &model, 2000U) == 0U);
    model.memory[CLOCK_MEMORY_W25Q].state = CLOCK_DATA_INITIALIZING;
    assert(clock_schedule_due(&schedule, &model, 2000U) == 0U);
    model.memory[CLOCK_MEMORY_W25Q].state = CLOCK_DATA_READY;
    model.memory[CLOCK_MEMORY_W25Q].value = 1;
    assert(clock_schedule_due(&schedule, &model, 2000U) == 0U);
    model.calendar.minute++;
    assert(clock_schedule_due(&schedule, &model, 2001U) != 0U);
    model.memory_mode = schedule.next_memory;
    clock_schedule_commit(&schedule, &model, 2001U);
    assert(schedule.next_memory == CLOCK_MEMORY_W25Q);
    model.format = CLOCK_FORMAT_12; model.format_revision++;
    assert(clock_schedule_due(&schedule, &model, 2100U) != 0U);
    model.memory_mode = schedule.next_memory;
    clock_schedule_commit(&schedule, &model, 2100U);
    assert(schedule.next_memory == CLOCK_MEMORY_SRAM);
    model.faults.revision++; model.faults.active_categories = 1U;
    assert(clock_schedule_due(&schedule, &model, 2200U) != 0U);
    model.memory_mode = schedule.next_memory;
    clock_schedule_commit(&schedule, &model, 2200U);
    assert(schedule.next_memory == CLOCK_MEMORY_SRAM);
    assert(clock_schedule_due(&schedule, &model, 59999U) == 0U);
    model.faults.revision += 3U;
    assert(clock_schedule_due(&schedule, &model, 2300U) != 0U);
    clock_schedule_commit(&schedule, &model, 2300U);
    assert(clock_schedule_due(&schedule, &model, 2400U) == 0U);
    model.faults.revision++; model.faults.active_categories = 0U;
    assert(clock_schedule_due(&schedule, &model, 2401U) != 0U);
    clock_schedule_commit(&schedule, &model, 2401U);
    assert(schedule.next_memory == CLOCK_MEMORY_IFLASH);
    model.calendar = (clock_calendar_t){2026U, 10U, 18U, 7U, 23U, 59U, 59U};
    clock_schedule_commit(&schedule, &model, 3000U);
    clock_calendar_add_seconds(&model.calendar, 1U);
    assert(clock_schedule_due(&schedule, &model, 3001U) != 0U);
    clock_schedule_commit(&schedule, &model, 3001U);
    model.calendar = (clock_calendar_t){2027U, 10U, 19U, 2U, 0U, 0U, 0U};
    assert(clock_schedule_due(&schedule, &model, 3002U) != 0U);
    model.calendar_state = CLOCK_DATA_ERROR;
    assert(clock_schedule_due(&schedule, &model, 3003U) != 0U);
    clock_schedule_commit(&schedule, &model, 0xFFFFFF00UL);
    assert(clock_schedule_due(&schedule, &model, 0x00000010UL) == 0U);
    assert(clock_schedule_due(&schedule, &model, 0x00020000UL) == 0U);
    model.faults.revision++;
    assert(clock_schedule_due(&schedule, &model, 0x00000011UL) != 0U);
}

static void s_test_service(void)
{
    clock_view_model_t model;
    uint32_t second;
    s_ticks = 0U; s_rtc_result = 0; s_image_result = 0;
    s_rtc_reads = 0U; s_image_reads = 0U;
    s_linked_sram = 32768U; s_image_flash = 65536U; s_free_heap = 2048U;
    (void)memset(&s_config, 0, sizeof(s_config));
    s_config.format = CLOCK_FORMAT_24;
    clock_service_init();
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_INITIALIZING && model.startup_ready == 0U);
    clock_service_poll();
    clock_service_get_model(&model);
    assert(model.startup_ready == 0U && model.calendar_state == CLOCK_DATA_READY);
    assert(model.memory[CLOCK_MEMORY_SRAM].value == 23);
    assert(model.memory[CLOCK_MEMORY_IFLASH].value == 13);
    assert(model.memory[CLOCK_MEMORY_W25Q].state == CLOCK_DATA_UNAVAILABLE);
    assert(model.temperature.state == CLOCK_DATA_UNAVAILABLE);
    assert(model.battery.state == CLOCK_DATA_UNAVAILABLE && model.wifi.state == CLOCK_DATA_UNAVAILABLE);
    assert(model.faults.active_categories == 0U);
    s_config.ready = 1U;
    s_config.format = CLOCK_FORMAT_12; s_config.revision = 2U;
    s_usage.state = CLOCK_DATA_INITIALIZING;
    clock_service_get_model(&model);
    assert(model.startup_ready == 1U && model.format == CLOCK_FORMAT_12 && model.format_revision == 2U);
    assert(model.memory[CLOCK_MEMORY_W25Q].state == CLOCK_DATA_INITIALIZING);
    s_usage.state = CLOCK_DATA_UNAVAILABLE;
    s_free_heap = 0U; s_image_flash = 262144U;
    for (second = 1U; second < 10U; second++) {
        s_ticks = second * 1000U;
        clock_service_poll();
    }
    clock_service_get_model(&model);
    assert(s_rtc_reads == 10U && s_image_reads == 1U);
    assert(model.memory[CLOCK_MEMORY_SRAM].value == 23 && model.memory[CLOCK_MEMORY_IFLASH].value == 13);
    s_ticks = 10000U;
    clock_service_poll();
    clock_service_get_model(&model);
    assert(s_image_reads == 2U && model.memory[CLOCK_MEMORY_SRAM].value == 25);
    assert(model.memory[CLOCK_MEMORY_IFLASH].value == 50);
    s_ticks = 20000U; s_rtc_result = -1; s_probe_coherence = 1U;
    clock_service_poll();
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_ERROR && model.faults.active_categories == 1U);
    assert(model.faults.source[CLOCK_FAULT_RTC].active_mask == CLOCK_FAULT_IO);
    s_ticks = 30000U; s_rtc_result = 0; s_probe_coherence = 1U;
    s_linked_sram = 131073U; s_image_flash = 524289U;
    clock_service_poll();
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_READY && model.faults.active_categories == 0U);
    assert(model.memory[CLOCK_MEMORY_SRAM].state == CLOCK_DATA_UNAVAILABLE);
    assert(model.memory[CLOCK_MEMORY_IFLASH].state == CLOCK_DATA_UNAVAILABLE);
    s_ticks = 40000U; s_linked_sram = 1024U; s_free_heap = 2048U;
    clock_service_poll();
    clock_service_get_model(&model);
    assert(model.memory[CLOCK_MEMORY_SRAM].state == CLOCK_DATA_UNAVAILABLE);
    s_ticks = 0xFFFFF000UL;
    clock_service_poll();
    second = s_image_reads;
    s_ticks += 9999U;
    clock_service_poll();
    assert(s_image_reads == second);
    s_ticks++;
    clock_service_poll();
    assert(s_image_reads == second + 1U);
    s_ticks += 10000U; s_image_result = -1;
    clock_service_poll();
    clock_service_get_model(&model);
    assert(model.memory[CLOCK_MEMORY_SRAM].state == CLOCK_DATA_UNAVAILABLE);
    assert(model.memory[CLOCK_MEMORY_IFLASH].state == CLOCK_DATA_UNAVAILABLE);
    s_rtc_result = -2;
    clock_service_poll();
    clock_service_get_model(&model);
    assert(model.faults.source[CLOCK_FAULT_RTC].active_mask == CLOCK_FAULT_CONFIGURATION);
    clock_service_init();
    clock_service_mark_unavailable();
    clock_fault_report(CLOCK_FAULT_TASK, CLOCK_FAULT_START);
    clock_service_get_model(&model);
    assert(model.startup_ready == 1U && model.calendar_state == CLOCK_DATA_UNAVAILABLE);
    assert(model.faults.active_categories == 1U);
    assert(model.faults.source[CLOCK_FAULT_TASK].time_valid == 0U);
}

static void s_test_usage_snapshot(void)
{
    static const struct {
        uint32_t used_bytes;
        int32_t percent;
    } cases[] = {
        {0U, 0}, {1U, 1}, {W25Q128_SECTOR_SIZE, 1},
        {40U * W25Q128_SECTOR_SIZE, 1},
        {41U * W25Q128_SECTOR_SIZE, 2},
        {W25Q128_TOTAL_SIZE / 2U, 50},
        {W25Q128_TOTAL_SIZE / 2U + W25Q128_SECTOR_SIZE, 51},
        {W25Q128_TOTAL_SIZE - W25Q128_SECTOR_SIZE, 100},
        {W25Q128_TOTAL_SIZE, 100}
    };
    static const clock_data_state_t states[] = {
        CLOCK_DATA_UNAVAILABLE, CLOCK_DATA_INITIALIZING, CLOCK_DATA_ERROR
    };
    static const uint32_t invalid_totals[] = {
        0U, W25Q128_TOTAL_SIZE - 1U, W25Q128_TOTAL_SIZE + 1U, UINT32_MAX
    };
    clock_view_model_t model;
    uint32_t image_reads = s_image_reads;
    uint32_t rtc_reads = s_rtc_reads;
    uint32_t usage_reads = s_usage_reads;
    size_t i;
    clock_service_init();
    clock_service_get_model(NULL);
    assert(s_usage_reads == usage_reads);
    s_usage.total_bytes = W25Q128_TOTAL_SIZE;
    s_usage.result = W25Q128_OK;
    s_usage.used_bytes = W25Q128_TOTAL_SIZE;
    for (i = 0U; i < sizeof(states) / sizeof(states[0]); i++) {
        s_usage.state = states[i];
        clock_service_get_model(&model);
        assert(model.memory[CLOCK_MEMORY_W25Q].state == states[i]);
        assert(model.memory[CLOCK_MEMORY_W25Q].value == 0);
    }
    s_usage.state = CLOCK_DATA_READY;
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        s_usage.used_bytes = cases[i].used_bytes;
        clock_service_get_model(&model);
        assert(model.memory[CLOCK_MEMORY_W25Q].state == CLOCK_DATA_READY);
        assert(model.memory[CLOCK_MEMORY_W25Q].value == cases[i].percent);
    }
    for (i = 0U; i < sizeof(invalid_totals) / sizeof(invalid_totals[0]); i++) {
        s_usage.total_bytes = invalid_totals[i];
        s_usage.used_bytes = 0U;
        clock_service_get_model(&model);
        assert(model.memory[CLOCK_MEMORY_W25Q].state == CLOCK_DATA_ERROR);
        assert(model.memory[CLOCK_MEMORY_W25Q].value == 0);
    }
    s_usage.total_bytes = W25Q128_TOTAL_SIZE;
    s_usage.used_bytes = W25Q128_TOTAL_SIZE + 1U;
    clock_service_get_model(&model);
    assert(model.memory[CLOCK_MEMORY_W25Q].state == CLOCK_DATA_ERROR);
    s_usage.used_bytes = UINT32_MAX;
    clock_service_get_model(&model);
    assert(model.memory[CLOCK_MEMORY_W25Q].state == CLOCK_DATA_ERROR);
    assert(s_image_reads == image_reads && s_rtc_reads == rtc_reads);
    assert(model.faults.active_categories == 0U);
}

static void s_assert_calendar(const clock_calendar_t *p_actual,
                              const clock_calendar_t *p_expected)
{
    assert(p_actual->year == p_expected->year &&
        p_actual->month == p_expected->month && p_actual->day == p_expected->day &&
        p_actual->hour == p_expected->hour && p_actual->minute == p_expected->minute &&
        p_actual->second == p_expected->second &&
        p_actual->weekday == clock_calendar_weekday(p_expected->year,
            p_expected->month, p_expected->day));
}

static void s_time_reset(TickType_t ticks)
{
    s_ticks = ticks;
    s_rtc_result = 0;
    s_write_result = 0;
    s_write_delay = 0U;
    s_image_result = 0;
    s_bad_verified = 0U;
    s_preempt_notify = 0U;
    s_probe_write = 0U;
    s_inject_on_read = 0U;
    s_inject_on_exit = 0U;
    s_rtc_reads = 0U;
    s_rtc_writes = 0U;
    s_image_reads = 0U;
    s_notifications = 0U;
    s_notify_calls = 0U;
    s_wait_count = 0U;
    s_wait_hook = NULL;
    s_calendar = (clock_calendar_t){2026U, 10U, 27U, 2U, 9U, 0U, 0U};
    s_config.ready = 1U;
    s_config.format = CLOCK_FORMAT_24;
    clock_service_init();
}

static void s_run_until_stop(void)
{
    if (setjmp(s_run_exit) == 0) { clock_service_run(); }
}

static void s_bind_time_owner(void)
{
    s_run_until_stop();
    assert(s_critical == 0U && s_wait_count == 1U);
}

static void s_test_time_requests(void)
{
    static const clock_calendar_t s_jumps[] = {
        {2000U, 1U, 1U, 0U, 0U, 0U, 0U},
        {2028U, 2U, 29U, 0U, 23U, 59U, 59U},
        {2027U, 12U, 31U, 0U, 23U, 59U, 59U},
        {2028U, 1U, 1U, 0U, 0U, 0U, 0U}
    };
    clock_calendar_t requested = {2099U, 12U, 31U, 0U, 23U, 59U, 58U};
    clock_calendar_t expected = requested;
    clock_calendar_t invalid;
    clock_calendar_t historical;
    clock_view_model_t model;
    int32_t result = 123;
    uint32_t writes;
    uint32_t reads;
    uint32_t first_fault_tick;
    size_t i;

    s_time_reset(0U);
    assert(clock_service_request_time(&requested, 101U) == -2);
    assert(clock_service_request_time(NULL, 101U) == -3);
    assert(clock_service_request_time(&requested, 0U) == -3);
    assert(clock_service_take_time_result(0U, &result) == 0U && result == 123);
    invalid = requested; invalid.year = 2100U;
    assert(clock_service_request_time(&invalid, 101U) == -3);
    invalid = requested; invalid.month = 2U; invalid.day = 29U;
    assert(clock_service_request_time(&invalid, 101U) == -3);
    invalid = requested; invalid.second = 60U;
    assert(clock_service_request_time(&invalid, 101U) == -3);
    assert(s_notify_calls == 0U && s_rtc_writes == 0U);
    s_bind_time_owner();
    clock_fault_report(CLOCK_FAULT_IMU, CLOCK_FAULT_IO);
    clock_service_get_model(&model);
    historical = model.faults.source[CLOCK_FAULT_IMU].first_time;
    assert(clock_service_request_time(&requested, 101U) == 0);
    requested.year = 2000U;
    assert(clock_service_request_time(&requested, 102U) == -1);
    assert(clock_service_take_time_result(101U, &result) == 0U && result == 123);
    assert(s_notify_calls == 1U && s_rtc_writes == 0U);
    reads = s_rtc_reads;
    s_probe_write = 1U;
    clock_service_poll();
    assert(s_rtc_reads == reads && s_rtc_writes == 1U);
    s_assert_calendar(&s_requested, &expected);
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_READY);
    s_assert_calendar(&model.calendar, &expected);
    s_assert_calendar(&model.faults.source[CLOCK_FAULT_IMU].first_time, &historical);
    assert(clock_service_request_time(&requested, 102U) == -1);
    assert(clock_service_take_time_result(999U, &result) == 0U && result == 123);
    assert(clock_service_take_time_result(101U, NULL) == 0U);
    assert(clock_service_take_time_result(101U, &result) == 1U && result == 0);
    assert(clock_service_take_time_result(101U, &result) == 0U);
    clock_fault_report(CLOCK_FAULT_IMU, 0U);
    for (i = 0U; i < sizeof(s_jumps) / sizeof(s_jumps[0]); i++) {
        assert(clock_service_request_time(&s_jumps[i], (uint32_t)i + 201U) == 0);
        clock_service_poll();
        clock_service_get_model(&model);
        assert(model.calendar_state == CLOCK_DATA_READY);
        s_assert_calendar(&model.calendar, &s_jumps[i]);
        assert(clock_service_take_time_result((uint32_t)i + 201U, &result) == 1U && result == 0);
    }
    clock_fault_report(CLOCK_FAULT_TEMP, CLOCK_FAULT_IO);
    clock_service_get_model(&model);
    s_assert_calendar(&model.faults.source[CLOCK_FAULT_TEMP].first_time,
                       &s_jumps[3]);
    clock_fault_report(CLOCK_FAULT_TEMP, 0U);

    s_write_result = -7;
    assert(clock_service_request_time(&expected, 301U) == 0);
    clock_service_poll();
    assert(clock_service_take_time_result(301U, &result) == 1U && result == -7);
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_ERROR);
    assert(model.faults.source[CLOCK_FAULT_RTC].active_mask == CLOCK_FAULT_IO);
    first_fault_tick = model.faults.source[CLOCK_FAULT_RTC].first_tick;
    s_assert_calendar(&model.calendar, &s_jumps[3]);
    s_ticks += 1000U;
    clock_service_poll();
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_ERROR);
    assert(model.faults.source[CLOCK_FAULT_RTC].active_mask == CLOCK_FAULT_IO);
    assert(model.faults.source[CLOCK_FAULT_RTC].first_tick == first_fault_tick);
    s_assert_calendar(&model.calendar, &s_jumps[3]);
    writes = s_rtc_writes;
    assert(clock_service_request_time(&invalid, 302U) == -3);
    clock_service_poll();
    assert(s_rtc_writes == writes);
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_ERROR);

    s_write_result = 0;
    assert(clock_service_request_time(&s_jumps[0], 302U) == 0);
    clock_service_poll();
    assert(clock_service_take_time_result(302U, &result) == 1U && result == 0);
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_READY);
    assert(model.faults.source[CLOCK_FAULT_RTC].active_mask == 0U);
    s_assert_calendar(&model.calendar, &s_jumps[0]);
    s_bad_verified = 1U;
    assert(clock_service_request_time(&expected, 303U) == 0);
    clock_service_poll();
    assert(clock_service_take_time_result(303U, &result) == 1U && result == -2);
    s_bad_verified = 0U;
    clock_service_poll();
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_ERROR);
    s_preempt_notify = 1U;
    assert(clock_service_request_time(&expected, 304U) == 0);
    /* Model/fault publication precedes completion even if the owner preempts. */
    assert(clock_service_take_time_result(304U, &result) == 1U && result == 0);
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_READY);
    s_assert_calendar(&model.calendar, &expected);
    s_rtc_result = -1;
    clock_service_poll();
    s_rtc_result = 0;
    clock_service_poll();
    clock_service_get_model(&model);
    assert(model.calendar_state == CLOCK_DATA_READY);
    writes = s_rtc_writes;
    clock_service_mark_unavailable();
    assert(clock_service_request_time(&expected, 305U) == -2);
    assert(s_rtc_writes == writes);
}

static void s_time_loop_wait(TickType_t timeout)
{
    clock_view_model_t model;
    int32_t result = 99;
    switch (s_wait_count) {
    case 1U:
        /* The request arrived after poll publication but before notify-wait. */
        assert(s_ticks == s_loop_origin && timeout == 1000U);
        assert(s_notify_calls == 1U && s_rtc_writes == 0U);
        break;
    case 2U:
        assert(s_ticks == s_loop_origin && timeout == 1000U);
        assert(clock_service_take_time_result(900U, &result) == 1U && result == 0);
        s_ticks += 200U;
        s_injected_calendar.year = 2030U;
        assert(clock_service_request_time(&s_injected_calendar, 901U) == 0);
        break;
    case 3U:
        assert(s_ticks == (TickType_t)(s_loop_origin + 200U) && timeout == 800U);
        assert(clock_service_take_time_result(901U, &result) == 1U && result == 0);
        clock_service_get_model(&model);
        s_assert_calendar(&model.calendar, &s_injected_calendar);
        s_ticks += 450U;
        s_injected_calendar.year = 2001U;
        assert(clock_service_request_time(&s_injected_calendar, 902U) == 0);
        break;
    case 4U:
        assert(s_ticks == (TickType_t)(s_loop_origin + 650U) && timeout == 350U);
        assert(clock_service_take_time_result(902U, &result) == 1U && result == 0);
        break;
    case 5U:
        assert(s_ticks == (TickType_t)(s_loop_origin + 1000U) && timeout == 1000U);
        assert(s_rtc_reads == 2U && s_rtc_writes == 3U && s_notify_calls == 3U);
        longjmp(s_run_exit, 1);
        break;
    default:
        assert(0);
        break;
    }
}

static void s_time_overrun_wait(TickType_t timeout)
{
    int32_t result;
    if (s_wait_count == 1U) {
        assert(timeout == 1000U && s_ticks == 0U);
        s_ticks = 100U;
        s_write_delay = 2500U;
        assert(clock_service_request_time(&s_injected_calendar, 950U) == 0);
    } else if (s_wait_count == 2U) {
        assert(s_ticks == 2600U && timeout == 400U);
        assert(clock_service_take_time_result(950U, &result) == 1U && result == 0);
    } else {
        assert(s_wait_count == 3U && s_ticks == 3000U && timeout == 1000U);
        assert(s_rtc_reads == 2U && s_rtc_writes == 1U);
        longjmp(s_run_exit, 1);
    }
}

static void s_test_time_loop(void)
{
    static const TickType_t s_origins[] = {0U, 0xFFFFFF00UL};
    size_t i;
    for (i = 0U; i < sizeof(s_origins) / sizeof(s_origins[0]); i++) {
        s_time_reset(s_origins[i]);
        s_loop_origin = s_origins[i];
        s_injected_calendar = (clock_calendar_t){2027U, 1U, 1U, 0U, 10U, 0U, 0U};
        s_inject_on_read = 1U;
        s_wait_hook = s_time_loop_wait;
        s_run_until_stop();
        assert(s_wait_count == 5U && s_critical == 0U);
    }
    s_time_reset(0U);
    s_injected_calendar = (clock_calendar_t){2028U, 2U, 29U, 0U, 23U, 59U, 59U};
    s_wait_hook = s_time_overrun_wait;
    s_run_until_stop();
    assert(s_wait_count == 3U && s_critical == 0U);
    s_wait_hook = NULL;
}

int main(void)
{
    s_test_calendar();
    s_test_registry();
    s_test_schedule();
    s_test_service();
    s_test_usage_snapshot();
    s_test_time_requests();
    s_test_time_loop();
    assert(s_critical == 0U);
    (void)puts("clock runtime: dates, faults, schedule, cached service, W25Q snapshots, TIME mailbox/ownership, fault latch/recovery and notify/deadline/wrap passed");
    return 0;
}
