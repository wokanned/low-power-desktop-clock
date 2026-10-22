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

static const char *s_scenario;
static uint32_t s_critical;
static uint32_t s_phase;
static uint32_t s_init_calls;
static uint32_t s_load_calls;
static uint32_t s_save_calls;
static uint32_t s_faults[CLOCK_FAULT_SOURCE_COUNT];
static uint8_t *s_queue_bytes;
static uint32_t s_queue_size;
static uint8_t s_queued;
static osThreadFunc_t s_entry;
static jmp_buf s_finished;

static uint8_t s_is(const char *p_name) { return strcmp(s_scenario, p_name) == 0; }
void storage_test_enter(void) { s_critical++; }
void storage_test_exit(void) { assert(s_critical != 0U); s_critical--; }
TickType_t xTaskGetTickCount(void) { return 0U; }

QueueHandle_t xQueueCreateStatic(uint32_t length, uint32_t item_size,
                                uint8_t *p_bytes, StaticQueue_t *p_control)
{
    assert(length == 1U && p_control != NULL && p_bytes != NULL);
    s_queue_bytes = p_bytes; s_queue_size = item_size;
    return s_is("queue-failure") != 0U ? NULL : p_control;
}

int xQueueSend(QueueHandle_t queue, const void *p_item, uint32_t wait)
{
    assert(queue != NULL && wait == 0U && s_critical == 0U);
    assert(s_queued == 0U);
    (void)memcpy(s_queue_bytes, p_item, s_queue_size);
    s_queued = 1U;
    return pdPASS;
}

int xQueueReceive(QueueHandle_t queue, void *p_item, uint32_t wait)
{
    storage_config_snapshot_t snapshot;
    storage_diagnostics_t diagnostics;
    assert(queue != NULL && wait != portMAX_DELAY && s_critical == 0U);
    storage_get_config_snapshot(&snapshot);
    assert(snapshot.ready == 1U && snapshot.busy == 0U);
    if (s_phase == 0U) {
        assert(snapshot.format == ((s_is("init-failure") || s_is("load-failure") ||
                                    s_is("corrupt")) ? CLOCK_FORMAT_24 : CLOCK_FORMAT_12));
        assert(storage_request_format(CLOCK_FORMAT_12, 0U) == STORAGE_REQUEST_ARGUMENT);
        assert(storage_request_format((clock_format_t)11, 1U) == STORAGE_REQUEST_ARGUMENT);
        assert(storage_request_format(CLOCK_FORMAT_24, 1U) == STORAGE_REQUEST_OK);
        assert(storage_request_format(CLOCK_FORMAT_12, 2U) == STORAGE_REQUEST_BUSY);
    } else if (s_phase == 1U) {
        assert(snapshot.completed_token == 1U);
        if (s_is("save-failure") != 0U || s_is("late-commit") != 0U) {
            assert(snapshot.completed_result == W25Q128_ERROR_IO);
            assert(snapshot.format == CLOCK_FORMAT_12);
            assert(s_faults[CLOCK_FAULT_CONFIG] == CLOCK_FAULT_IO);
        } else {
            assert(snapshot.completed_result == W25Q128_OK);
            assert(snapshot.format == CLOCK_FORMAT_24);
        }
        assert(storage_request_format(s_is("save-failure") ? CLOCK_FORMAT_24 :
                                       CLOCK_FORMAT_12, 2U) == STORAGE_REQUEST_OK);
    } else if (s_phase == 2U && s_is("late-commit") != 0U) {
        /* The previous write reached Flash but its final verification failed.
         * Recovery loads 24h; this new 12h write also fails before committing. */
        assert(snapshot.completed_token == 2U && snapshot.completed_result == W25Q128_ERROR_IO);
        assert(snapshot.format == CLOCK_FORMAT_12 && snapshot.revision == 1U);
        assert(s_faults[CLOCK_FAULT_CONFIG] == CLOCK_FAULT_IO);
        assert(storage_request_format(CLOCK_FORMAT_24, 3U) == STORAGE_REQUEST_OK);
    } else if (s_is("late-commit") != 0U) {
        assert(snapshot.completed_token == 3U && snapshot.completed_result == W25Q128_OK);
        assert(snapshot.format == CLOCK_FORMAT_24 && snapshot.revision == 2U);
        assert(s_faults[CLOCK_FAULT_CONFIG] == 0U && s_faults[CLOCK_FAULT_W25Q] == 0U);
        assert(s_init_calls == 3U && s_load_calls == 3U && s_save_calls == 3U);
        longjmp(s_finished, 1);
    } else {
        assert(snapshot.completed_token == 2U && snapshot.completed_result == W25Q128_OK);
        assert(snapshot.format == (s_is("save-failure") ? CLOCK_FORMAT_24 : CLOCK_FORMAT_12));
        assert(s_faults[CLOCK_FAULT_CONFIG] == 0U);
        assert(s_faults[CLOCK_FAULT_W25Q] == (s_is("init-failure") ? CLOCK_FAULT_IO : 0U));
        assert(s_init_calls == ((s_is("init-failure") || s_is("save-failure") ||
                                  s_is("load-failure")) ? 2U : 1U));
        assert(s_save_calls == 2U);
        storage_get_diagnostics(&diagnostics);
        assert(diagnostics.config_result == W25Q128_OK);
        assert(s_critical == 0U);
        longjmp(s_finished, 1);
    }
    s_phase++;
    assert(s_queued != 0U);
    (void)memcpy(p_item, s_queue_bytes, s_queue_size);
    s_queued = 0U;
    return pdPASS;
}

osThreadId_t osThreadNew(osThreadFunc_t function, void *p_argument,
                         const osThreadAttr_t *p_attributes)
{
    (void)p_argument;
    assert(p_attributes->cb_mem != NULL && p_attributes->stack_mem != NULL);
    assert(p_attributes->stack_size == 2048U);
    s_entry = function;
    return s_is("task-failure") != 0U ? NULL : &s_entry;
}

int32_t stm32_w25q128_bind(w25q128_ctx_t *p_context)
{
    (void)p_context;
    return s_is("bind-failure") != 0U ? -1 : 0;
}
w25q128_status_t w25q128_init(const w25q128_ctx_t *p_context)
{
    (void)p_context;
    assert(s_critical == 0U);
    s_init_calls++;
    return s_is("init-failure") != 0U && s_init_calls == 1U ? W25Q128_ERROR_IO : W25Q128_OK;
}
w25q128_status_t w25q128_read_id(const w25q128_ctx_t *p_context, uint32_t *p_id)
{
    (void)p_context; assert(s_critical == 0U); *p_id = W25Q128_JEDEC_ID; return W25Q128_OK;
}
w25q128_status_t w25q128_read_registers(const w25q128_ctx_t *p_context,
                                       w25q128_registers_t *p_registers)
{
    (void)p_context; assert(s_critical == 0U);
    (void)memset(p_registers, 0, sizeof(*p_registers)); return W25Q128_OK;
}
w25q128_status_t w25q128_read(const w25q128_ctx_t *p_context, uint32_t address,
                              uint8_t *p_bytes, uint32_t length)
{
    (void)p_context;
    assert(s_critical == 0U && address <= W25Q128_TOTAL_SIZE - length);
    (void)memset(p_bytes, 0xFF, length);
    return W25Q128_OK;
}
int32_t clock_config_load(const w25q128_ctx_t *p_context, clock_config_state_t *p_state)
{
    (void)p_context;
    assert(s_critical == 0U);
    s_load_calls++;
    (void)memset(p_state, 0, sizeof(*p_state));
    p_state->format = CLOCK_FORMAT_24;
    p_state->active_slot = CLOCK_CONFIG_NO_SLOT;
    if (s_is("corrupt") != 0U && s_load_calls == 1U) { return CLOCK_CONFIG_ERROR_CORRUPT; }
    if (s_is("load-failure") != 0U && s_load_calls == 1U) { return W25Q128_ERROR_IO; }
    p_state->format = s_is("late-commit") != 0U && s_load_calls > 1U ?
        CLOCK_FORMAT_24 : CLOCK_FORMAT_12;
    p_state->valid = 1U;
    p_state->active_slot = 0U;
    return W25Q128_OK;
}
int32_t clock_config_save(const w25q128_ctx_t *p_context, clock_config_state_t *p_state,
                          clock_format_t format)
{
    (void)p_context;
    assert(s_critical == 0U);
    s_save_calls++;
    if (s_is("save-failure") != 0U && s_save_calls == 1U) { return W25Q128_ERROR_IO; }
    if (s_is("late-commit") != 0U && s_save_calls <= 2U) { return W25Q128_ERROR_IO; }
    p_state->format = format;
    p_state->valid = 1U;
    return W25Q128_OK;
}
void clock_fault_report(clock_fault_source_t source, uint32_t mask)
{
    assert(source < CLOCK_FAULT_SOURCE_COUNT);
    if (s_phase != 0U) { assert(s_critical != 0U); }
    s_faults[source] = mask;
}

int main(int argc, char **argv)
{
    storage_config_snapshot_t snapshot;
    storage_usage_snapshot_t usage;
    assert(argc == 2);
    s_scenario = argv[1];
    if (s_is("bind-failure") || s_is("task-failure") || s_is("queue-failure")) {
        assert(storage_start() == -1);
        storage_get_config_snapshot(&snapshot);
        assert(snapshot.ready == 1U && snapshot.format == CLOCK_FORMAT_24);
        assert(storage_request_format(CLOCK_FORMAT_12, 1U) == STORAGE_REQUEST_UNAVAILABLE);
        assert(s_faults[CLOCK_FAULT_W25Q] == CLOCK_FAULT_START);
        storage_get_usage_snapshot(&usage);
        assert(usage.state == CLOCK_DATA_ERROR && usage.used_bytes == 0U);
        assert(usage.total_bytes == W25Q128_TOTAL_SIZE && usage.result == W25Q128_ERROR_IO);
    } else {
        assert(storage_start() == 0);
        assert(storage_request_format(CLOCK_FORMAT_12, 1U) == STORAGE_REQUEST_BUSY);
        assert(s_entry != NULL);
        if (setjmp(s_finished) == 0) { s_entry(NULL); }
    }
    (void)printf("storage service: %s passed\n", s_scenario);
    return 0;
}
