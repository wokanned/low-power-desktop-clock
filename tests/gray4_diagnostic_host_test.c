/* Real renderer, vendor flow, controller driver and display task. Only the
 * platform, clock data provider and RTOS are replaced; each case is a process. */
#include "../app/display.h"
#include "../app/epd_bringup.h"
#include "../app/epd_clock_view.h"
#include "../app/clock_service.h"
#include "../platform/stm32_gdey029t94.h"
#include "stubs/display/FreeRTOS.h"
#include "stubs/display/cmsis_os.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COMMANDS 64U
#define MAX_ATTEMPTS 3U
#define STAGE_INIT 16U
#define STAGE_REFRESH 17U
#define STAGE_SLEEP 18U
#define STAGE_HELD 19U
#define STOP_SUSPENDED 1
#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d (%s): %s\n", \
                __FILE__, __LINE__, s_scenario, #condition); \
        exit(1); \
    } \
} while (0)

typedef struct {
    uint8_t command;
    uint32_t stage;
    uint32_t attempt;
    uint32_t length;
    uint8_t data[EPD_GDEY029T94_FRAME_BUFFER_SIZE];
} command_t;

static const char *s_scenario;
static unsigned long s_checks;
static jmp_buf s_stop;
static uint8_t s_expected[EPD_GDEY029T94_GRAY4_SIZE];
static command_t s_commands[MAX_COMMANDS];
static uint32_t s_command_count;
static uint32_t s_bind_calls;
static uint32_t s_prepare_calls;
static uint32_t s_reset_calls;
static uint32_t s_failures;
static uint32_t s_retry_delays;
static uint32_t s_model_reads;
static uint32_t s_thread_creates;
static uint32_t s_suspends;
static uint32_t s_tick;
static uint32_t s_attempt_start[MAX_ATTEMPTS];
static uint32_t s_attempt_resets[MAX_ATTEMPTS];
static uint8_t s_dc;
static uint8_t s_cs = 1U;
static osThreadFunc_t s_thread_function;
static void *s_thread_argument;
static command_t *s_current;

static uint8_t scenario(const char *p_name)
{
    return (uint8_t)(strcmp(s_scenario, p_name) == 0);
}

static uint8_t persistent(void)
{
    return (uint8_t)(strstr(s_scenario, "-always") != NULL);
}

static uint8_t inject(const char *p_prefix)
{
    return (uint8_t)((strncmp(s_scenario, p_prefix, strlen(p_prefix)) == 0) &&
                     ((s_bind_calls == 1U) || persistent()));
}

static epd_gdey029t94_status_t expected_error(void)
{
    return strstr(s_scenario, "busy-timeout") != NULL ?
        EPD_GDEY029T94_ERROR_BUSY_TIMEOUT : EPD_GDEY029T94_ERROR_IO;
}

void clock_service_get_model(clock_view_model_t *p_model)
{
    (void)p_model;
    s_model_reads++;
    CHECK(0);
}

void clock_service_note_sync_success(void) { CHECK(0); }

bool weather_publish(const weather_data_t *p_data, uint32_t now_tick)
{
    (void)p_data;
    (void)now_tick;
    CHECK(0);
    return false;
}

void weather_get_snapshot(uint32_t now_tick, weather_snapshot_t *p_snapshot)
{
    (void)now_tick;
    (void)p_snapshot;
    CHECK(0);
}

static int32_t fake_write(void *p_handle, const uint8_t *p_data,
                          uint32_t length, uint32_t timeout_ms)
{
    uint32_t stage;
    (void)p_handle;
    CHECK(s_cs == 0U);
    CHECK(timeout_ms == 1000U);
    CHECK(p_data != NULL && length != 0U);
    stage = epd_vendor_demo_state.stage;
    if ((inject("init-") && stage == STAGE_INIT && s_dc == 0U && p_data[0] == 0x12U) ||
        (inject("refresh-") && stage == STAGE_REFRESH && s_dc != 0U &&
         s_current != NULL && s_current->command == 0x24U) ||
        (inject("sleep-") && stage == STAGE_SLEEP && s_dc != 0U)) {
        s_failures++;
        return -1;
    }
    if (s_dc == 0U) {
        CHECK(length == 1U && s_command_count < MAX_COMMANDS);
        if (stage == STAGE_INIT) {
            CHECK(p_data[0] == 0x12U || p_data[0] == 0x18U ||
                  p_data[0] == 0x22U || p_data[0] == 0x20U || p_data[0] == 0x1AU);
        } else if (stage == STAGE_REFRESH) {
            CHECK(p_data[0] == 0x24U || p_data[0] == 0x26U ||
                  p_data[0] == 0x22U || p_data[0] == 0x20U);
        } else {
            CHECK(stage == STAGE_SLEEP && p_data[0] == 0x10U);
        }
        s_current = &s_commands[s_command_count++];
        s_current->command = p_data[0];
        s_current->stage = stage;
        s_current->attempt = s_bind_calls;
    } else {
        CHECK(s_current != NULL);
        CHECK(s_current->stage == stage && s_current->attempt == s_bind_calls);
        CHECK(s_current->length + length <= sizeof(s_current->data));
        memcpy(s_current->data + s_current->length, p_data, length);
        s_current->length += length;
    }
    return 0;
}

static int32_t fake_busy(void *p_handle, uint8_t *p_level)
{
    (void)p_handle;
    CHECK(p_level != NULL);
    *p_level = 0U;
    if (epd_vendor_demo_state.stage == STAGE_REFRESH) {
        CHECK(s_current != NULL && s_current->command == 0x20U);
        if (inject("busy-io-")) {
            s_failures++;
            return -1;
        }
        if (inject("busy-timeout-")) { *p_level = 1U; }
    }
    return 0;
}

static void fake_cs(void *p_handle, uint8_t level)
{
    (void)p_handle;
    s_cs = level;
}

static void fake_dc(void *p_handle, uint8_t level)
{
    (void)p_handle;
    s_dc = level;
}

static void fake_reset(void *p_handle, uint8_t level)
{
    (void)p_handle;
    CHECK(epd_vendor_demo_state.stage == STAGE_INIT);
    if (level == 0U) {
        s_reset_calls++;
        s_attempt_resets[s_bind_calls - 1U]++;
    }
}

static void fake_delay(void *p_handle, uint32_t milliseconds)
{
    (void)p_handle;
    CHECK(milliseconds == 1U || milliseconds == 10U || milliseconds == 100U);
    s_tick += milliseconds;
}

static uint32_t fake_tick(void *p_handle)
{
    (void)p_handle;
    return s_tick;
}

epd_gdey029t94_status_t stm32_gdey029t94_make_io(
    epd_gdey029t94_io_t *p_io, stm32_gdey029t94_context_t *p_context)
{
    CHECK(p_io != NULL && p_context != NULL);
    CHECK(s_bind_calls < MAX_ATTEMPTS);
    s_attempt_start[s_bind_calls] = s_command_count;
    s_bind_calls++;
    s_current = NULL;
    if (inject("bind-")) {
        s_failures++;
        return EPD_GDEY029T94_ERROR_IO;
    }
    p_io->spi_write = fake_write;
    p_io->read_busy = fake_busy;
    p_io->set_cs = fake_cs;
    p_io->set_dc = fake_dc;
    p_io->set_reset = fake_reset;
    p_io->delay_ms = fake_delay;
    p_io->get_tick_ms = fake_tick;
    p_io->handle = p_context;
    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t stm32_gdey029t94_prepare(
    stm32_gdey029t94_context_t *p_context)
{
    CHECK(p_context != NULL);
    s_prepare_calls++;
    if (inject("prepare-")) {
        s_failures++;
        return EPD_GDEY029T94_ERROR_IO;
    }
    return EPD_GDEY029T94_OK;
}

void Debug_Log(const char *p_message)
{
    CHECK(p_message != NULL);
}

TickType_t xTaskGetTickCount(void) { return s_tick; }

osThreadId_t osThreadNew(osThreadFunc_t function, void *p_argument,
                         const osThreadAttr_t *p_attributes)
{
    CHECK(function != NULL && p_attributes != NULL);
    CHECK(p_attributes->cb_mem != NULL && p_attributes->cb_size != 0U);
    CHECK(p_attributes->stack_mem != NULL && p_attributes->stack_size == 2048U);
    CHECK(p_attributes->priority == osPriorityNormal);
    CHECK(strcmp(p_attributes->name, "displayTask") == 0);
    s_thread_creates++;
    s_thread_function = function;
    s_thread_argument = p_argument;
    return (osThreadId_t)&s_thread_function;
}

osThreadId_t osThreadGetId(void) { return (osThreadId_t)&s_thread_function; }

int32_t osThreadSuspend(osThreadId_t thread)
{
    CHECK(thread == osThreadGetId());
    s_suspends++;
    longjmp(s_stop, STOP_SUSPENDED);
    return 0;
}

int32_t osDelay(uint32_t ticks)
{
    CHECK(ticks == 1000U && s_retry_delays < 2U);
    CHECK(display_get_last_status() == expected_error());
    CHECK(epd_bringup_get_last_status() == expected_error());
    s_retry_delays++;
    s_tick += ticks;
    return 0;
}

static uint8_t encoded_pair(uint8_t first, uint8_t second, uint8_t plane)
{
    uint16_t pair;
    uint8_t result;
    uint8_t pixel;
    pair = (uint16_t)(((uint16_t)first << 8U) | second);
    result = 0U;
    for (pixel = 0U; pixel < 8U; pixel++) {
        result = (uint8_t)(result | (((pair >> (14U - 2U * pixel)) >> plane) & 1U)
                                    << (7U - pixel));
    }
    return (uint8_t)~result;
}

static void check_payloads(void)
{
    uint32_t record;
    uint32_t offset;
    uint8_t plane;
    command_t *p_command;
    for (record = 0U; record < s_command_count; record++) {
        p_command = &s_commands[record];
        if (p_command->command == 0x24U || p_command->command == 0x26U) {
            CHECK(p_command->stage == STAGE_REFRESH);
            CHECK(p_command->length == 0U || p_command->length == sizeof(p_command->data));
            if (p_command->length == 0U) { continue; }
            plane = (uint8_t)(p_command->command == 0x26U);
            for (offset = 0U; offset < p_command->length; offset++) {
                CHECK(p_command->data[offset] == encoded_pair(
                    s_expected[offset * 2U], s_expected[offset * 2U + 1U], plane));
            }
        } else if (p_command->command == 0x22U) {
            CHECK(p_command->length == 1U);
            CHECK(p_command->data[0] == (p_command->stage == STAGE_REFRESH ? 0xC7U :
                  (s_commands[record - 1U].command == 0x1AU ? 0x91U : 0xB1U)));
        } else if (p_command->command == 0x10U) {
            CHECK(p_command->stage == STAGE_SLEEP);
            CHECK(p_command->length == 0U ||
                  (p_command->length == 1U && p_command->data[0] == 0x01U));
        }
    }
}

static void check_success_trace(void)
{
    static const uint8_t commands[] = {
        0x12U, 0x18U, 0x22U, 0x20U, 0x1AU, 0x22U, 0x20U,
        0x24U, 0x26U, 0x22U, 0x20U, 0x10U
    };
    uint32_t start;
    uint32_t index;
    start = s_attempt_start[s_bind_calls - 1U];
    CHECK(s_command_count - start == sizeof(commands));
    for (index = 0U; index < sizeof(commands); index++) {
        CHECK(s_commands[start + index].command == commands[index]);
    }
    CHECK(s_commands[s_command_count - 1U].length == 1U);
}

static void verify(void)
{
    uint32_t expected_attempts;
    uint32_t attempt;
    uint8_t success;
    success = (uint8_t)!persistent();
    expected_attempts = persistent() ? 3U :
        ((scenario("success") || scenario("run-once")) ? 1U : 2U);
    CHECK(s_bind_calls == expected_attempts);
    CHECK(s_retry_delays == (scenario("run-once") ? 0U : expected_attempts - 1U));
    CHECK(s_model_reads == 0U && epd_bringup_get_completed_frames() == 0U);
    CHECK(epd_vendor_demo_state.recovered_to_clock == 0U);
    CHECK(epd_vendor_demo_state.completed_batches == 0U);
    CHECK(epd_vendor_demo_state.cleanup_refreshes == 0U);
    CHECK(s_cs == 1U);
    if (strncmp(s_scenario, "bind-", 5U) == 0) {
        CHECK(s_prepare_calls == (success ? 1U : 0U));
    } else {
        CHECK(s_prepare_calls == expected_attempts);
    }
    if (strncmp(s_scenario, "bind-", 5U) == 0 ||
        strncmp(s_scenario, "prepare-", 8U) == 0) {
        CHECK(s_reset_calls == (success ? 1U : 0U));
    } else {
        CHECK(s_reset_calls == expected_attempts);
        for (attempt = 0U; attempt < expected_attempts; attempt++) {
            CHECK(s_attempt_resets[attempt] == 1U);
        }
    }
    if (success) {
        CHECK(epd_bringup_get_last_status() == EPD_GDEY029T94_OK);
        CHECK(display_get_last_status() == EPD_GDEY029T94_OK);
        CHECK(epd_vendor_demo_state.status == EPD_GDEY029T94_OK);
        CHECK(epd_vendor_demo_state.stage == STAGE_HELD);
        CHECK(epd_vendor_demo_state.completed == 1U);
        CHECK(epd_vendor_demo_state.completed_pages == 1U);
        check_success_trace();
    } else {
        CHECK(epd_bringup_get_last_status() == expected_error());
        CHECK(display_get_last_status() == expected_error());
        CHECK(epd_vendor_demo_state.completed == 0U);
        if (strncmp(s_scenario, "bind-", 5U) != 0 &&
            strncmp(s_scenario, "prepare-", 8U) != 0) {
            CHECK(epd_vendor_demo_state.status == expected_error());
        }
    }
    CHECK(s_failures == 0U || s_failures == expected_attempts - success);
    check_payloads();
}

int main(int argc, char **argv)
{
    int stopped;
    if (argc != 2) { fprintf(stderr, "A scenario argument is required\n"); return 2; }
    s_scenario = argv[1];
    CHECK(EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_GRAY4_HOLD);
    CHECK(epd_clock_view_draw_gray4_diagnostic(s_expected, sizeof(s_expected)) == 1U);
    if (scenario("run-once")) {
        CHECK(epd_bringup_run_once() == EPD_GDEY029T94_OK);
        CHECK(s_thread_creates == 0U && s_suspends == 0U);
    } else {
        CHECK(display_start());
        CHECK(display_start());
        CHECK(s_thread_creates == 1U && s_thread_function != NULL);
        stopped = setjmp(s_stop);
        if (stopped == 0) {
            s_thread_function(s_thread_argument);
            CHECK(0);
        }
        CHECK(stopped == STOP_SUSPENDED && s_suspends == 1U);
    }
    verify();
    printf("PASS: gray4 diagnostic %s (%lu checks), %lu attempts\n", s_scenario,
           s_checks, (unsigned long)s_bind_calls);
    return 0;
}
