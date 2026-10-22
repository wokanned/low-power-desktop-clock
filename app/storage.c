#include "storage.h"
#include "clock_config.h"
#include "clock_faults.h"
#include "stm32_w25q128.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "cmsis_os.h"
#include <stddef.h>

#define STORAGE_TASK_STACK_BYTES 2048U
#define STORAGE_TEST_PAGE_OFFSET 250U
#define STORAGE_TEST_LENGTH      32U
#define STORAGE_TEST_PATCH_OFFSET 8U
#define STORAGE_TEST_PATCH_LENGTH 8U
#define STORAGE_TEST_PATTERN_BASE 0x20U
#define STORAGE_TEST_PATCH_BASE   0xA0U
#define STORAGE_ERASED_BYTE       0xFFU
#define STORAGE_SCAN_YIELD_TICKS  ((TickType_t)1U)

typedef struct {
    uint32_t address;
    uint32_t used_bytes;
    TickType_t completed_tick;
    uint8_t running;
} storage_scan_t;

static w25q128_ctx_t s_context;
static StackType_t s_storage_task_stack[STORAGE_TASK_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_storage_task_control;
static osThreadId_t s_storage_task_handle;
static volatile storage_diagnostics_t s_diagnostics;
static storage_config_snapshot_t s_config = { CLOCK_FORMAT_24, 0U, 0U, 0U, 0U, 0 };
static storage_usage_snapshot_t s_usage = {
    CLOCK_DATA_UNAVAILABLE, 0U, W25Q128_TOTAL_SIZE, W25Q128_OK
};
typedef struct { clock_format_t format; uint32_t token; } storage_request_t;
static StaticQueue_t s_request_queue_control;
static uint8_t s_request_queue_bytes[sizeof(storage_request_t)];
static QueueHandle_t s_request_queue;

#if STORAGE_BOOT_TEST_ENABLE
static uint8_t s_update_scratch[W25Q128_SECTOR_SIZE];

static w25q128_status_t s_check_sector(uint8_t patched)
{
    uint8_t bytes[W25Q128_PAGE_SIZE];
    uint8_t expected;
    uint32_t offset;
    uint32_t index;
    uint32_t i;
    w25q128_status_t result;

    for (offset = 0U; offset < STORAGE_TEST_SIZE; offset += sizeof(bytes)) {
        result = w25q128_read(&s_context, STORAGE_TEST_ADDRESS + offset,
                              bytes, sizeof(bytes));
        if (result != W25Q128_OK) { return result; }
        for (i = 0U; i < sizeof(bytes); i++) {
            index = offset + i;
            expected = STORAGE_ERASED_BYTE;
            if (index >= STORAGE_TEST_PAGE_OFFSET &&
                index < STORAGE_TEST_PAGE_OFFSET + STORAGE_TEST_LENGTH) {
                index -= STORAGE_TEST_PAGE_OFFSET;
                expected = (uint8_t)(STORAGE_TEST_PATTERN_BASE + index);
                if (patched != 0U && index >= STORAGE_TEST_PATCH_OFFSET &&
                    index < STORAGE_TEST_PATCH_OFFSET + STORAGE_TEST_PATCH_LENGTH) {
                    expected = (uint8_t)(STORAGE_TEST_PATCH_BASE + index);
                }
            }
            if (bytes[i] != expected) { return W25Q128_ERROR_VERIFY; }
        }
    }
    return W25Q128_OK;
}

static w25q128_status_t s_run_test(void)
{
    uint8_t pattern[STORAGE_TEST_LENGTH];
    uint8_t patch[STORAGE_TEST_PATCH_LENGTH];
    uint32_t i;
    w25q128_status_t result;

    for (i = 0U; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(STORAGE_TEST_PATTERN_BASE + i);
    }
    for (i = 0U; i < sizeof(patch); i++) {
        patch[i] = (uint8_t)(STORAGE_TEST_PATCH_BASE + STORAGE_TEST_PATCH_OFFSET + i);
    }
    result = w25q128_erase_sector(&s_context, STORAGE_TEST_ADDRESS);
    if (result == W25Q128_OK) {
        result = w25q128_program(&s_context, STORAGE_TEST_ADDRESS + STORAGE_TEST_PAGE_OFFSET,
                                 pattern, sizeof(pattern));
    }
    if (result == W25Q128_OK) { result = s_check_sector(0U); }
    if (result == W25Q128_OK) {
        result = w25q128_update(&s_context,
                                STORAGE_TEST_ADDRESS + STORAGE_TEST_PAGE_OFFSET +
                                STORAGE_TEST_PATCH_OFFSET,
                                patch, sizeof(patch), s_update_scratch,
                                sizeof(s_update_scratch));
    }
    if (result == W25Q128_OK) { result = s_check_sector(1U); }
    return result;
}
#endif

static void s_publish(const storage_diagnostics_t *p_diagnostics)
{
    taskENTER_CRITICAL();
    s_diagnostics = *p_diagnostics;
    taskEXIT_CRITICAL();
}

static uint32_t s_fault_mask(int32_t result)
{
    switch (result) {
    case W25Q128_OK: return 0U;
    case W25Q128_ERROR_ID: return CLOCK_FAULT_ID;
    case W25Q128_ERROR_TIMEOUT: return CLOCK_FAULT_TIMEOUT;
    case W25Q128_ERROR_VERIFY: return CLOCK_FAULT_VERIFY;
    case CLOCK_CONFIG_ERROR_CORRUPT: return CLOCK_FAULT_CONFIGURATION;
    default: return CLOCK_FAULT_IO;
    }
}

static void s_publish_usage(clock_data_state_t state, uint32_t used_bytes,
                            w25q128_status_t result)
{
    taskENTER_CRITICAL();
    s_usage.state = state;
    s_usage.used_bytes = used_bytes;
    s_usage.result = result;
    taskEXIT_CRITICAL();
}

static w25q128_status_t s_scan_sector(uint32_t address, uint8_t *p_used)
{
    uint8_t bytes[W25Q128_PAGE_SIZE];
    uint32_t offset;
    uint32_t i;
    w25q128_status_t result;

    *p_used = 0U;
    for (offset = 0U; offset < W25Q128_SECTOR_SIZE; offset += sizeof(bytes)) {
        result = w25q128_read(&s_context, address + offset, bytes, sizeof(bytes));
        if (result != W25Q128_OK) { return result; }
        for (i = 0U; i < sizeof(bytes); i++) {
            if (bytes[i] != STORAGE_ERASED_BYTE) {
                *p_used = 1U;
                return W25Q128_OK;
            }
        }
    }
    return W25Q128_OK;
}

static w25q128_status_t s_scan_step(storage_scan_t *p_scan)
{
    uint8_t used;
    w25q128_status_t result;

    do {
        result = s_scan_sector(p_scan->address, &used);
        if (result != W25Q128_OK) { return result; }
        if (used != 0U) { p_scan->used_bytes += W25Q128_SECTOR_SIZE; }
        p_scan->address += W25Q128_SECTOR_SIZE;
        /* Only the final two sectors can change at runtime. Check both and
         * publish before dequeuing another configuration transaction. */
    } while (p_scan->address > CLOCK_CONFIG_SECTOR_A &&
             p_scan->address < W25Q128_TOTAL_SIZE);
    return W25Q128_OK;
}

static int32_t s_initialize(storage_diagnostics_t *p_diagnostics)
{
    int32_t result;
    w25q128_status_t id_result;
    w25q128_status_t registers_result;
    result = w25q128_init(&s_context);
    /* Retain raw ID/status even if init rejects the device. */
    id_result = w25q128_read_id(&s_context, &p_diagnostics->jedec_id);
    registers_result = w25q128_read_registers(&s_context, &p_diagnostics->registers);
    p_diagnostics->id_valid = id_result == W25Q128_OK ? 1U : 0U;
    p_diagnostics->registers_valid = registers_result == W25Q128_OK ? 1U : 0U;
    if (result == W25Q128_OK) { result = id_result; }
    if (result == W25Q128_OK) { result = registers_result; }
    return result;
}

static void s_publish_config(const clock_config_state_t *p_state, uint8_t ready,
                             uint8_t completed, uint32_t token, int32_t result)
{
    taskENTER_CRITICAL();
    if ((completed == 0U || result == W25Q128_OK) &&
        s_config.format != p_state->format) {
        s_config.format = p_state->format;
        s_config.revision++;
    }
    s_config.ready = ready;
    if (completed != 0U) {
        s_config.completed_token = token;
        s_config.completed_result = result;
        s_config.busy = 0U;
    }
    taskEXIT_CRITICAL();
}

static void s_storage_task(void *p_argument)
{
    storage_diagnostics_t diagnostics = {
        STORAGE_TEST_NOT_RUN, W25Q128_OK, 0U, {0U, 0U, 0U}, 0U, 0U, 0, 0U
    };
    clock_config_state_t state = { CLOCK_FORMAT_24, 0U, CLOCK_CONFIG_NO_SLOT, 0U, 0U };
    clock_config_state_t recovered;
    storage_request_t request;
    storage_scan_t scan = { 0U, 0U, 0U, 0U };
    TickType_t now;
    TickType_t wait;
    TickType_t elapsed;
    uint32_t boot_fault;
    uint32_t config_fault;
    uint32_t scan_fault;
    w25q128_status_t scan_result;
    uint8_t online;
    uint8_t test_ran = 0U;
    int32_t result;

    (void)p_argument;
    diagnostics.test_status = STORAGE_TEST_RUNNING;
    s_publish(&diagnostics);
    diagnostics.result = (w25q128_status_t)s_initialize(&diagnostics);
    scan_result = diagnostics.result;
    online = diagnostics.result == W25Q128_OK ? 1U : 0U;
    diagnostics.config_result = online != 0U ? clock_config_load(&s_context, &state) :
        diagnostics.result;
    if (diagnostics.config_result != W25Q128_OK &&
        diagnostics.config_result != CLOCK_CONFIG_ERROR_CORRUPT) { online = 0U; }
    diagnostics.config_degraded = state.degraded;
    clock_fault_report(CLOCK_FAULT_CONFIG, s_fault_mask(diagnostics.config_result));
    s_publish(&diagnostics);

#if STORAGE_BOOT_TEST_ENABLE
    if (diagnostics.result == W25Q128_OK) {
        test_ran = 1U;
        diagnostics.result = s_run_test();
    }
    diagnostics.test_status = diagnostics.result == W25Q128_OK ?
        STORAGE_TEST_PASSED : STORAGE_TEST_FAILED;
#else
    diagnostics.test_status = diagnostics.result == W25Q128_OK ?
        STORAGE_TEST_SKIPPED : STORAGE_TEST_FAILED;
#endif
    /* A failed destructive test remains visible until the next boot's test.
     * Successful config traffic cannot prove that test range recovered. */
    boot_fault = test_ran != 0U ? s_fault_mask(diagnostics.result) : 0U;
    config_fault = diagnostics.config_result == CLOCK_CONFIG_ERROR_CORRUPT ?
        0U : s_fault_mask(diagnostics.config_result);
    scan_fault = s_fault_mask(scan_result);
    scan.running = scan_result == W25Q128_OK ? 1U : 0U;
    scan.completed_tick = xTaskGetTickCount();
    taskENTER_CRITICAL();
    clock_fault_report(CLOCK_FAULT_W25Q, boot_fault | config_fault | scan_fault);
    s_publish_usage(scan.running != 0U ? CLOCK_DATA_INITIALIZING : CLOCK_DATA_ERROR,
                    0U, scan_result);
    s_publish(&diagnostics);
    s_publish_config(&state, 1U, 0U, 0U, 0);
    taskEXIT_CRITICAL();
    /* Configuration recovery and scan recovery are independent: a successful
     * scan must not bypass reload after an ambiguous configuration commit. */
    for (;;) {
        now = xTaskGetTickCount();
        if (scan.running == 0U && (TickType_t)(now - scan.completed_tick) >=
            pdMS_TO_TICKS(STORAGE_USAGE_INTERVAL_MS)) {
            s_publish_usage(CLOCK_DATA_INITIALIZING, 0U, W25Q128_OK);
            scan_result = w25q128_init(&s_context);
            if (scan_result == W25Q128_OK) {
                scan.address = 0U;
                scan.used_bytes = 0U;
                scan.running = 1U;
            } else {
                online = 0U;
                scan.completed_tick = xTaskGetTickCount();
                scan_fault = s_fault_mask(scan_result);
                taskENTER_CRITICAL();
                s_publish_usage(CLOCK_DATA_ERROR, 0U, scan_result);
                clock_fault_report(CLOCK_FAULT_W25Q, boot_fault | config_fault | scan_fault);
                taskEXIT_CRITICAL();
            }
        }
        now = xTaskGetTickCount();
        elapsed = (TickType_t)(now - scan.completed_tick);
        wait = scan.running != 0U ? STORAGE_SCAN_YIELD_TICKS :
            (elapsed >= pdMS_TO_TICKS(STORAGE_USAGE_INTERVAL_MS) ? 0U :
             pdMS_TO_TICKS(STORAGE_USAGE_INTERVAL_MS) - elapsed);
        if (xQueueReceive(s_request_queue, &request, wait) == pdPASS) {
            result = W25Q128_OK;
            if (online == 0U) {
                result = s_initialize(&diagnostics);
                if (result == W25Q128_OK) {
                    diagnostics.config_result = clock_config_load(&s_context, &recovered);
                    /* Corrupt configuration may be repaired by this explicit write. */
                    if (diagnostics.config_result != W25Q128_OK &&
                        diagnostics.config_result != CLOCK_CONFIG_ERROR_CORRUPT) {
                        result = diagnostics.config_result;
                    } else {
                        state = recovered;
                    }
                }
            }
            if (result == W25Q128_OK) {
                result = clock_config_save(&s_context, &state, request.format);
            }
            online = result == W25Q128_OK ? 1U : 0U;
            config_fault = s_fault_mask(result);
            diagnostics.config_result = result;
            diagnostics.config_degraded = state.degraded;
            diagnostics.result = (w25q128_status_t)result;
            taskENTER_CRITICAL();
            clock_fault_report(CLOCK_FAULT_CONFIG, config_fault);
            clock_fault_report(CLOCK_FAULT_W25Q, boot_fault | config_fault | scan_fault);
            s_publish(&diagnostics);
            s_publish_config(&state, 1U, 1U, request.token, result);
            taskEXIT_CRITICAL();
        }
        if (scan.running != 0U) {
            scan_result = s_scan_step(&scan);
            if (scan_result != W25Q128_OK || scan.address == W25Q128_TOTAL_SIZE) {
                scan.running = 0U;
                scan.completed_tick = xTaskGetTickCount();
                scan_fault = s_fault_mask(scan_result);
                if (scan_result != W25Q128_OK) { online = 0U; }
                taskENTER_CRITICAL();
                s_publish_usage(scan_result == W25Q128_OK ? CLOCK_DATA_READY : CLOCK_DATA_ERROR,
                                scan_result == W25Q128_OK ? scan.used_bytes : 0U, scan_result);
                clock_fault_report(CLOCK_FAULT_W25Q, boot_fault | config_fault | scan_fault);
                taskEXIT_CRITICAL();
            }
        }
    }
}

int32_t storage_start(void)
{
    const osThreadAttr_t attributes = {
        .name = "storageTask",
        .cb_mem = &s_storage_task_control,
        .cb_size = sizeof(s_storage_task_control),
        .stack_mem = s_storage_task_stack,
        .stack_size = sizeof(s_storage_task_stack),
        .priority = osPriorityBelowNormal
    };

    if (s_storage_task_handle != NULL) { return 0; }
    if (s_request_queue == NULL) {
        s_request_queue = xQueueCreateStatic(1U, sizeof(storage_request_t),
            s_request_queue_bytes, &s_request_queue_control);
    }
    if (s_request_queue != NULL && stm32_w25q128_bind(&s_context) == 0) {
        s_storage_task_handle = osThreadNew(s_storage_task, NULL, &attributes);
        if (s_storage_task_handle != NULL) { return 0; }
    }
    s_diagnostics.result = W25Q128_ERROR_IO;
    s_diagnostics.test_status = STORAGE_TEST_FAILED;
    s_diagnostics.config_result = W25Q128_ERROR_IO;
    s_config.ready = 1U;
    s_publish_usage(CLOCK_DATA_ERROR, 0U, W25Q128_ERROR_IO);
    clock_fault_report(CLOCK_FAULT_W25Q, CLOCK_FAULT_START);
    return -1;
}

storage_test_status_t storage_get_test_status(void)
{
    return s_diagnostics.test_status;
}

void storage_get_diagnostics(storage_diagnostics_t *p_diagnostics)
{
    if (p_diagnostics != NULL) {
        taskENTER_CRITICAL();
        *p_diagnostics = s_diagnostics;
        taskEXIT_CRITICAL();
    }
}

void storage_get_config_snapshot(storage_config_snapshot_t *p_snapshot)
{
    if (p_snapshot != NULL) {
        taskENTER_CRITICAL();
        *p_snapshot = s_config;
        taskEXIT_CRITICAL();
    }
}

void storage_get_usage_snapshot(storage_usage_snapshot_t *p_snapshot)
{
    if (p_snapshot != NULL) {
        taskENTER_CRITICAL();
        *p_snapshot = s_usage;
        taskEXIT_CRITICAL();
    }
}

int32_t storage_request_format(clock_format_t format, uint32_t token)
{
    storage_request_t request;
    if ((format != CLOCK_FORMAT_12 && format != CLOCK_FORMAT_24) || token == 0U) {
        return STORAGE_REQUEST_ARGUMENT;
    }
    taskENTER_CRITICAL();
    if (s_storage_task_handle == NULL || s_request_queue == NULL) {
        taskEXIT_CRITICAL();
        return STORAGE_REQUEST_UNAVAILABLE;
    }
    if (s_config.ready == 0U || s_config.busy != 0U) {
        taskEXIT_CRITICAL();
        return STORAGE_REQUEST_BUSY;
    }
    s_config.busy = 1U;
    taskEXIT_CRITICAL();
    request.format = format;
    request.token = token;
    if (xQueueSend(s_request_queue, &request, 0U) != pdPASS) {
        taskENTER_CRITICAL();
        s_config.busy = 0U;
        taskEXIT_CRITICAL();
        return STORAGE_REQUEST_BUSY;
    }
    return STORAGE_REQUEST_OK;
}
