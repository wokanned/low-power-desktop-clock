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

TickType_t xTaskGetTickCount(void) { return s_ticks; }
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
void clock_test_exit(void) { assert(s_critical != 0U); s_critical--; }

int32_t stm32_clock_read_calendar(clock_calendar_t *p_calendar)
{
    assert(s_critical == 0U);
    s_rtc_reads++;
    if (s_rtc_result == 0) { *p_calendar = s_calendar; }
    return s_rtc_result;
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
    clock_service_get_model(&model);
    assert(model.startup_ready == 1U && model.format == CLOCK_FORMAT_12 && model.format_revision == 2U);
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

int main(void)
{
    s_test_calendar();
    s_test_registry();
    s_test_schedule();
    s_test_service();
    assert(s_critical == 0U);
    (void)puts("clock runtime: 36525 dates, fault episodes/aggregation/wrap, schedule and cached service passed");
    return 0;
}
