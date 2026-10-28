#include "storage.h"
#include "clock_config.h"
#include "clock_faults.h"
#include "cmsis_os.h"
#include "queue.h"
#include "task.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static uint8_t s_flash[W25Q128_TOTAL_SIZE];
static const char *s_scenario;
static uint32_t s_critical;
static TickType_t s_tick;
static uint32_t s_init_calls;
static uint32_t s_load_calls;
static uint32_t s_save_calls;
static uint32_t s_read_calls;
static uint32_t s_last_read;
static uint32_t s_queue_calls;
static uint32_t s_faults[CLOCK_FAULT_SOURCE_COUNT];
static uint32_t s_stage;
static uint32_t s_boundary_reads;
static uint32_t s_progress_read;
static uint8_t s_failed_read;
static uint8_t s_preempt_tick;
static uint8_t s_tail_queued;
static uint8_t *s_queue_bytes;
static uint32_t s_queue_size;
static uint8_t s_queued;
static clock_format_t s_persisted_format = CLOCK_FORMAT_12;
static osThreadFunc_t s_entry;
static jmp_buf s_finished;

static uint8_t s_is(const char *p_name) { return strcmp(s_scenario, p_name) == 0; }
void storage_test_enter(void) { s_critical++; }
void storage_test_exit(void) { assert(s_critical != 0U); s_critical--; }
TickType_t xTaskGetTickCount(void)
{
    TickType_t tick = s_tick;
    if (s_preempt_tick != 0U) {
        s_tick += 2U;
        s_preempt_tick = 0U;
    }
    return tick;
}

static void s_usage(storage_usage_snapshot_t *p_snapshot)
{
    uint32_t reads = s_read_calls;
    uint32_t inits = s_init_calls;
    uint32_t loads = s_load_calls;
    uint32_t saves = s_save_calls;
    storage_get_usage_snapshot(NULL);
    storage_get_usage_snapshot(p_snapshot);
    assert(s_read_calls == reads && s_init_calls == inits);
    assert(s_load_calls == loads && s_save_calls == saves);
    assert(p_snapshot->total_bytes == W25Q128_TOTAL_SIZE && s_critical == 0U);
}

static void s_finish(uint32_t used)
{
    storage_usage_snapshot_t usage;
    s_usage(&usage);
    assert(usage.state == CLOCK_DATA_READY && usage.result == W25Q128_OK);
    assert(usage.used_bytes == used && usage.used_bytes <= usage.total_bytes);
    longjmp(s_finished, 1);
}

static void s_request(clock_format_t format, uint32_t token)
{
    assert(storage_request_format(format, token) == STORAGE_REQUEST_OK);
}

QueueHandle_t xQueueCreateStatic(uint32_t length, uint32_t item_size,
                                uint8_t *p_bytes, StaticQueue_t *p_control)
{
    assert(length == 1U && p_control != NULL && p_bytes != NULL);
    s_queue_bytes = p_bytes;
    s_queue_size = item_size;
    return p_control;
}

int xQueueSend(QueueHandle_t queue, const void *p_item, uint32_t wait)
{
    assert(queue != NULL && wait == 0U && s_critical == 0U && s_queued == 0U);
    (void)memcpy(s_queue_bytes, p_item, s_queue_size);
    s_queued = 1U;
    return pdPASS;
}

/* The queue models elapsed blocking time and injects requests at task boundaries.
 * Driver reads use a real 16 MiB array, including the final config sectors. */
int xQueueReceive(QueueHandle_t queue, void *p_item, uint32_t wait)
{
    storage_usage_snapshot_t usage;
    storage_config_snapshot_t config;
    assert(queue != NULL && s_critical == 0U && wait != portMAX_DELAY);
    assert(++s_queue_calls < 20000U);
    s_usage(&usage);
    storage_get_config_snapshot(&config);
    assert(config.ready != 0U);

    if (s_is("periodic") || s_is("tick-wrap") || s_is("timer-preempt")) {
        if (usage.state == CLOCK_DATA_READY && s_stage == 0U) {
            assert(usage.used_bytes == 0U && s_init_calls == 1U);
            assert(wait == STORAGE_USAGE_INTERVAL_MS);
            s_boundary_reads = s_read_calls;
            s_flash[W25Q128_TOTAL_SIZE - 1U] = 0U;
            s_tick += STORAGE_USAGE_INTERVAL_MS - 1U;
            s_preempt_tick = s_is("timer-preempt");
            s_stage = 1U;
            return 0;
        }
        if (usage.state == CLOCK_DATA_READY && s_stage == 1U) {
            assert(s_read_calls == s_boundary_reads && s_init_calls == 1U);
            assert(wait == (s_is("timer-preempt") ? 0U : 1U));
            s_stage = 2U;
        } else if (usage.state == CLOCK_DATA_INITIALIZING && s_stage == 2U) {
            assert(s_read_calls == s_boundary_reads && s_init_calls == 2U);
            s_stage = 3U;
        } else if (usage.state == CLOCK_DATA_READY && s_stage == 3U) {
            s_finish(W25Q128_SECTOR_SIZE);
        }
    } else if (s_is("retry-init-failure")) {
        if (usage.state == CLOCK_DATA_READY && s_stage == 0U) {
            assert(usage.used_bytes == 0U && s_init_calls == 1U);
            s_flash[0U] = 0U;
            s_stage = 1U;
        } else if (usage.state == CLOCK_DATA_ERROR) {
            assert(s_stage == 1U && s_init_calls == 2U);
            assert(usage.used_bytes == 0U && usage.result == W25Q128_ERROR_TIMEOUT);
            assert(wait == STORAGE_USAGE_INTERVAL_MS);
            assert(s_faults[CLOCK_FAULT_W25Q] == CLOCK_FAULT_TIMEOUT);
            s_stage = 2U;
        } else if (usage.state == CLOCK_DATA_READY && s_stage == 2U) {
            assert(s_init_calls == 3U && s_faults[CLOCK_FAULT_W25Q] == 0U);
            s_finish(W25Q128_SECTOR_SIZE);
        }
    } else if (s_is("tail-read-failure")) {
        if (usage.state == CLOCK_DATA_ERROR) {
            assert(usage.used_bytes == 0U && usage.result == W25Q128_ERROR_TIMEOUT);
            assert(s_last_read == CLOCK_CONFIG_SECTOR_B);
            assert(s_faults[CLOCK_FAULT_W25Q] == CLOCK_FAULT_TIMEOUT);
            longjmp(s_finished, 1);
        }
        assert(usage.state != CLOCK_DATA_READY);
    } else if (s_is("read-failure") || s_is("init-failure")) {
        if (usage.state == CLOCK_DATA_ERROR) {
            assert(s_stage == 0U && usage.used_bytes == 0U);
            assert(usage.result == (s_is("init-failure") ? W25Q128_ERROR_IO :
                                    W25Q128_ERROR_TIMEOUT));
            assert(wait == STORAGE_USAGE_INTERVAL_MS);
            assert(s_faults[CLOCK_FAULT_W25Q] != 0U);
            if (s_is("init-failure")) { assert(s_read_calls == 0U && s_load_calls == 0U); }
            s_stage = 1U;
        } else if (usage.state == CLOCK_DATA_READY) {
            assert(s_stage == 1U && s_init_calls == 2U);
            assert(s_faults[CLOCK_FAULT_W25Q] ==
                   (s_is("init-failure") ? CLOCK_FAULT_IO : 0U));
            assert(s_load_calls == (s_is("init-failure") ? 0U : 1U));
            s_finish(W25Q128_SECTOR_SIZE);
        }
    } else if (s_is("fault-aggregation")) {
        if (usage.state == CLOCK_DATA_INITIALIZING && s_stage == 0U && s_save_calls == 0U) {
            s_request(CLOCK_FORMAT_24, 1U);
        } else if (usage.state == CLOCK_DATA_ERROR && s_stage == 0U) {
            assert(usage.used_bytes == 0U && config.completed_result == W25Q128_ERROR_IO);
            assert(s_faults[CLOCK_FAULT_CONFIG] == CLOCK_FAULT_IO);
            assert(s_faults[CLOCK_FAULT_W25Q] == (CLOCK_FAULT_IO | CLOCK_FAULT_TIMEOUT));
            s_request(CLOCK_FORMAT_24, 2U);
            s_stage = 1U;
        } else if (usage.state == CLOCK_DATA_ERROR && s_stage == 1U) {
            assert(config.completed_token == 2U && config.completed_result == W25Q128_OK);
            assert(s_faults[CLOCK_FAULT_CONFIG] == 0U);
            assert(s_faults[CLOCK_FAULT_W25Q] == CLOCK_FAULT_TIMEOUT);
            s_stage = 2U;
        } else if (usage.state == CLOCK_DATA_READY) {
            assert(s_stage == 2U && s_faults[CLOCK_FAULT_W25Q] == 0U);
            s_finish(W25Q128_SECTOR_SIZE);
        }
    } else if (s_is("late-commit-scan")) {
        if (usage.state == CLOCK_DATA_INITIALIZING && s_save_calls == 0U) {
            s_request(CLOCK_FORMAT_24, 1U);
        } else if (usage.state == CLOCK_DATA_READY && s_stage == 0U) {
            assert(config.format == CLOCK_FORMAT_12 && config.revision == 1U);
            assert(s_load_calls == 1U && s_persisted_format == CLOCK_FORMAT_24);
            s_request(CLOCK_FORMAT_12, 2U);
            s_stage = 1U;
        } else if (usage.state == CLOCK_DATA_READY && s_stage == 1U) {
            assert(config.completed_token == 2U && config.completed_result == W25Q128_ERROR_IO);
            assert(config.format == CLOCK_FORMAT_12 && config.revision == 1U);
            assert(s_load_calls == 2U && s_persisted_format == CLOCK_FORMAT_24);
            s_request(CLOCK_FORMAT_24, 3U);
            s_stage = 2U;
        } else if (usage.state == CLOCK_DATA_READY && s_stage == 2U) {
            assert(config.completed_token == 3U && config.completed_result == W25Q128_OK);
            assert(config.format == CLOCK_FORMAT_24 && config.revision == 2U);
            assert(s_load_calls == 3U && s_faults[CLOCK_FAULT_W25Q] == 0U);
            s_finish(W25Q128_SECTOR_SIZE);
        }
    } else if (s_is("requests")) {
        if (usage.state == CLOCK_DATA_INITIALIZING && s_stage < 100U) {
            assert(config.completed_token == s_stage);
            if (s_stage != 0U) { assert(s_last_read > s_progress_read); }
            s_progress_read = s_last_read;
            s_request((s_stage & 1U) == 0U ? CLOCK_FORMAT_24 : CLOCK_FORMAT_12,
                      s_stage + 1U);
            s_stage++;
        } else if (usage.state == CLOCK_DATA_READY) {
            assert(s_save_calls == 100U && config.completed_token == 100U);
            s_finish(W25Q128_SECTOR_SIZE);
        }
    } else if (s_is("config-tail")) {
        if (usage.state == CLOCK_DATA_READY && s_stage == 0U) {
            assert(s_tail_queued != 0U && s_save_calls == 0U && s_queued != 0U);
            assert(usage.used_bytes == W25Q128_SECTOR_SIZE);
            s_stage = 1U;
        } else if (usage.state == CLOCK_DATA_READY && s_stage == 1U) {
            assert(config.completed_token == 1U && s_save_calls == 1U);
            s_finish(W25Q128_SECTOR_SIZE);
        }
    } else if (s_is("partial-write")) {
        if (usage.state == CLOCK_DATA_INITIALIZING && s_save_calls == 0U) {
            s_request(CLOCK_FORMAT_24, 1U);
        } else if (usage.state == CLOCK_DATA_READY) {
            assert(config.completed_result == W25Q128_ERROR_IO);
            assert(config.format == CLOCK_FORMAT_12);
            assert(s_faults[CLOCK_FAULT_CONFIG] == CLOCK_FAULT_IO);
            assert(s_faults[CLOCK_FAULT_W25Q] == CLOCK_FAULT_IO);
            s_finish(W25Q128_SECTOR_SIZE);
        }
    } else if (usage.state == CLOCK_DATA_READY) {
        if (s_is("full")) { s_finish(W25Q128_TOTAL_SIZE); }
        if (s_is("boundaries")) { s_finish(5U * W25Q128_SECTOR_SIZE); }
        if (s_is("boot-failure")) {
            assert(storage_get_test_status() == STORAGE_TEST_FAILED);
            assert(s_faults[CLOCK_FAULT_W25Q] == CLOCK_FAULT_VERIFY);
        }
        s_finish(0U);
    }

    if (s_queued != 0U) {
        (void)memcpy(p_item, s_queue_bytes, s_queue_size);
        s_queued = 0U;
        return pdPASS;
    }
    s_tick += wait;
    return 0;
}

osThreadId_t osThreadNew(osThreadFunc_t function, void *p_argument,
                         const osThreadAttr_t *p_attributes)
{
    (void)p_argument;
    assert(p_attributes->cb_mem != NULL && p_attributes->stack_mem != NULL);
    assert(p_attributes->stack_size == 4096U);
    s_entry = function;
    return &s_entry;
}

int32_t stm32_w25q128_bind(w25q128_ctx_t *p_context) { (void)p_context; return 0; }
w25q128_status_t w25q128_init(const w25q128_ctx_t *p_context)
{
    (void)p_context;
    assert(s_critical == 0U);
    s_init_calls++;
    if (s_is("retry-init-failure") && s_init_calls == 2U) { return W25Q128_ERROR_TIMEOUT; }
    return s_is("init-failure") && s_init_calls == 1U ? W25Q128_ERROR_IO : W25Q128_OK;
}
w25q128_status_t w25q128_read_id(const w25q128_ctx_t *p_context, uint32_t *p_id)
{
    (void)p_context;
    assert(s_critical == 0U);
    *p_id = W25Q128_JEDEC_ID;
    return W25Q128_OK;
}
w25q128_status_t w25q128_read_registers(const w25q128_ctx_t *p_context,
                                       w25q128_registers_t *p_registers)
{
    (void)p_context;
    assert(s_critical == 0U);
    (void)memset(p_registers, 0, sizeof(*p_registers));
    return W25Q128_OK;
}
w25q128_status_t w25q128_read(const w25q128_ctx_t *p_context, uint32_t address,
                              uint8_t *p_bytes, uint32_t length)
{
    storage_usage_snapshot_t usage;
    (void)p_context;
    assert(s_critical == 0U && p_bytes != NULL && length != 0U);
    assert(length <= W25Q128_PAGE_SIZE && address <= W25Q128_TOTAL_SIZE - length);
    s_read_calls++;
    s_last_read = address;
    if (s_is("tail-read-failure") && address == CLOCK_CONFIG_SECTOR_B) {
        return W25Q128_ERROR_TIMEOUT;
    }
    if ((s_is("read-failure") || s_is("fault-aggregation")) &&
        s_failed_read == 0U && address >= 2U * W25Q128_SECTOR_SIZE) {
        s_failed_read = 1U;
        return W25Q128_ERROR_TIMEOUT;
    }
    if (s_is("config-tail") && address == CLOCK_CONFIG_SECTOR_A) {
        assert(s_tail_queued == 0U);
        s_usage(&usage);
        assert(usage.state == CLOCK_DATA_INITIALIZING);
        s_request(CLOCK_FORMAT_24, 1U);
        s_tail_queued = 1U;
    }
    if (s_is("config-tail") && address >= CLOCK_CONFIG_SECTOR_B) {
        assert(s_tail_queued != 0U && s_save_calls == 0U);
    }
    (void)memcpy(p_bytes, &s_flash[address], length);
    return W25Q128_OK;
}

int32_t clock_config_load(const w25q128_ctx_t *p_context, clock_config_state_t *p_state)
{
    (void)p_context;
    assert(s_critical == 0U);
    s_load_calls++;
    (void)memset(p_state, 0, sizeof(*p_state));
    p_state->format = s_persisted_format;
    p_state->active_slot = 0U;
    p_state->valid = 1U;
    return W25Q128_OK;
}
int32_t clock_config_save(const w25q128_ctx_t *p_context, clock_config_state_t *p_state,
                          clock_format_t format)
{
    storage_usage_snapshot_t usage;
    (void)p_context;
    assert(s_critical == 0U);
    s_save_calls++;
    if (s_is("config-tail")) {
        s_usage(&usage);
        assert(usage.state == CLOCK_DATA_READY && s_stage == 1U);
    }
    if (s_is("fault-aggregation") && s_save_calls == 1U) { return W25Q128_ERROR_IO; }
    if (s_is("partial-write")) {
        s_flash[CLOCK_CONFIG_SECTOR_A + 5U] = 0U;
        return W25Q128_ERROR_IO;
    }
    if (s_is("late-commit-scan") && s_save_calls <= 2U) {
        if (s_save_calls == 1U) {
            s_persisted_format = format;
            s_flash[CLOCK_CONFIG_SECTOR_A] = 0U;
        }
        return W25Q128_ERROR_IO;
    }
    (void)memset(&s_flash[CLOCK_CONFIG_SECTOR_B], 0xFF, W25Q128_SECTOR_SIZE);
    s_flash[CLOCK_CONFIG_SECTOR_A] = 0U;
    s_persisted_format = format;
    p_state->format = format;
    p_state->valid = 1U;
    return W25Q128_OK;
}

w25q128_status_t w25q128_erase_sector(const w25q128_ctx_t *p_context, uint32_t address)
{
    (void)p_context;
    assert(address == STORAGE_TEST_ADDRESS && s_is("boot-failure"));
    return W25Q128_ERROR_VERIFY;
}
w25q128_status_t w25q128_program(const w25q128_ctx_t *p_context, uint32_t address,
                                 const uint8_t *p_bytes, uint32_t length)
{
    (void)p_context; (void)address; (void)p_bytes; (void)length;
    assert(0);
    return W25Q128_ERROR_IO;
}
w25q128_status_t w25q128_update(const w25q128_ctx_t *p_context, uint32_t address,
                                const uint8_t *p_bytes, uint32_t length,
                                uint8_t *p_scratch, uint32_t scratch_length)
{
    (void)p_context; (void)address; (void)p_bytes; (void)length;
    (void)p_scratch; (void)scratch_length;
    assert(0);
    return W25Q128_ERROR_IO;
}

void clock_fault_report(clock_fault_source_t source, uint32_t mask)
{
    assert(source < CLOCK_FAULT_SOURCE_COUNT);
    s_faults[source] = mask;
}

int main(int argc, char **argv)
{
    storage_usage_snapshot_t usage;
    assert(argc == 2);
    s_scenario = argv[1];
    (void)memset(s_flash, s_is("full") ? 0 : 0xFF, sizeof(s_flash));
    if (s_is("boundaries")) {
        s_flash[0U] = 0U;
        s_flash[2U * W25Q128_SECTOR_SIZE - 1U] = 0x7FU;
        s_flash[2U * W25Q128_SECTOR_SIZE + W25Q128_PAGE_SIZE] = 0xFEU;
        s_flash[CLOCK_CONFIG_SECTOR_A] = 0U;
        s_flash[W25Q128_TOTAL_SIZE - 1U] = 0U;
    }
    if (s_is("read-failure") || s_is("init-failure")) { s_flash[0U] = 0U; }
    if (s_is("config-tail")) { s_flash[CLOCK_CONFIG_SECTOR_B] = 0U; }
    if (s_is("tail-read-failure")) { s_flash[CLOCK_CONFIG_SECTOR_A] = 0U; }
    if (s_is("tick-wrap")) { s_tick = UINT32_MAX - 5000U; }
    s_usage(&usage);
    assert(usage.state == CLOCK_DATA_UNAVAILABLE && usage.used_bytes == 0U);
    assert(storage_start() == 0 && s_entry != NULL);
    if (setjmp(s_finished) == 0) { s_entry(NULL); }
    assert(s_critical == 0U);
    (void)printf("storage usage: %s passed\n", s_scenario);
    return 0;
}
