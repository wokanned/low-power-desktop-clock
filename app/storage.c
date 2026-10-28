#include "storage.h"
#include "storage_files.h"
#include "clock_config.h"
#include "clock_faults.h"
#include "stm32_w25q128.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "cmsis_os.h"
#include <stddef.h>
#include <string.h>

#define STORAGE_TASK_STACK_BYTES 4096U
#define STORAGE_TEST_PAGE_OFFSET 250U
#define STORAGE_TEST_LENGTH      32U
#define STORAGE_TEST_PATCH_OFFSET 8U
#define STORAGE_TEST_PATCH_LENGTH 8U
#define STORAGE_TEST_PATTERN_BASE 0x20U
#define STORAGE_TEST_PATCH_BASE   0xA0U
#define STORAGE_ERASED_BYTE       0xFFU
#define STORAGE_SCAN_YIELD_TICKS  ((TickType_t)1U)
#define STORAGE_BITMAP_BYTES (W25Q128_TOTAL_SIZE / W25Q128_SECTOR_SIZE / 8U)

typedef enum { STORAGE_FILE_IDLE, STORAGE_FILE_PENDING, STORAGE_FILE_RUNNING,
               STORAGE_FILE_COMPLETE } storage_file_state_t;
static file_store_request_t s_file_request;
static file_store_response_t s_file_response;
static file_store_snapshot_t s_files = {
    FILE_STORE_UNAVAILABLE, FILE_STORE_ERROR_NOT_READY, 0U, FILE_STORE_BLOCK_COUNT, 0U, 0U,
    W25Q128_OK
};
static volatile storage_file_state_t s_file_state;
static uint8_t s_scan_occupied[STORAGE_BITMAP_BYTES];
static uint8_t s_scan_dirty[STORAGE_BITMAP_BYTES];
static volatile uint8_t s_scan_paused;
static uint32_t s_maintenance_fault;
static uint8_t s_maintenance_valid;
static w25q128_status_t s_maintenance_result;

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

static uint8_t s_update_scratch[W25Q128_SECTOR_SIZE];

static void s_mark_dirty(uint32_t address, uint32_t length)
{
    uint32_t first;
    uint32_t last;
    if (length == 0U || address >= W25Q128_TOTAL_SIZE ||
        length > W25Q128_TOTAL_SIZE - address) { return; }
    first = address / W25Q128_SECTOR_SIZE;
    last = (address + length - 1U) / W25Q128_SECTOR_SIZE;
    for (; first <= last; first++) {
        s_scan_dirty[first / 8U] |= (uint8_t)(1U << (first % 8U));
    }
}

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
    s_mark_dirty(STORAGE_TEST_ADDRESS, STORAGE_TEST_SIZE);
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

static void s_publish(const storage_diagnostics_t *p_diagnostics)
{
    taskENTER_CRITICAL();
    s_diagnostics = *p_diagnostics;
    if (s_maintenance_valid != 0U) {
        s_diagnostics.test_status = s_maintenance_result == W25Q128_OK ?
            STORAGE_TEST_PASSED : STORAGE_TEST_FAILED;
        if (s_maintenance_result != W25Q128_OK) {
            s_diagnostics.result = s_maintenance_result;
        }
    }
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
    uint8_t old_used;
    uint8_t mask;
    uint32_t sector;
    uint32_t address;
    uint32_t i;
    w25q128_status_t result;

    do {
        address = p_scan->address;
        if (address == W25Q128_TOTAL_SIZE) {
            for (i = 0U; i < STORAGE_BITMAP_BYTES && s_scan_dirty[i] == 0U; i++) { }
            if (i == STORAGE_BITMAP_BYTES) { return W25Q128_OK; }
            sector = i * 8U;
            while ((s_scan_dirty[sector / 8U] & (1U << (sector % 8U))) == 0U) {
                sector++;
            }
            address = sector * W25Q128_SECTOR_SIZE;
        }
        sector = address / W25Q128_SECTOR_SIZE;
        mask = (uint8_t)(1U << (sector % 8U));
        result = s_scan_sector(address, &used);
        if (result != W25Q128_OK) { return result; }
        old_used = (uint8_t)((s_scan_occupied[sector / 8U] & mask) != 0U);
        if (used != old_used) {
            if (used != 0U) { p_scan->used_bytes += W25Q128_SECTOR_SIZE; }
            else { p_scan->used_bytes -= W25Q128_SECTOR_SIZE; }
        }
        if (used != 0U) { s_scan_occupied[sector / 8U] |= mask; }
        else { s_scan_occupied[sector / 8U] &= (uint8_t)~mask; }
        s_scan_dirty[sector / 8U] &= (uint8_t)~mask;
        if (p_scan->address < W25Q128_TOTAL_SIZE) {
            p_scan->address += W25Q128_SECTOR_SIZE;
        }
        /* Preserve an uninterrupted tail snapshot, and track mutations of ALL
         * other sectors (including failed attempts) for subsequent rechecks. */
    } while (p_scan->address > CLOCK_CONFIG_SECTOR_A &&
             p_scan->address < W25Q128_TOTAL_SIZE);
    return W25Q128_OK;
}

static uint8_t s_scan_clean(void)
{
    uint32_t i;
    for (i = 0U; i < STORAGE_BITMAP_BYTES; i++) {
        if (s_scan_dirty[i] != 0U) { return 0U; }
    }
    return 1U;
}

static void s_files_publish(void)
{
    file_store_snapshot_t snapshot;
    file_store_get_snapshot(&snapshot);
    taskENTER_CRITICAL();
    s_files = snapshot;
    s_files.busy = (uint8_t)(snapshot.busy != 0U ||
        s_file_state == STORAGE_FILE_PENDING || s_file_state == STORAGE_FILE_RUNNING);
    taskEXIT_CRITICAL();
}

static void s_files_process(void)
{
    uint8_t execute;
    uint8_t deferred;
    taskENTER_CRITICAL();
    execute = (uint8_t)(s_file_state == STORAGE_FILE_PENDING);
    if (execute != 0U) { s_file_state = STORAGE_FILE_RUNNING; }
    taskEXIT_CRITICAL();
    if (execute != 0U) {
        if (s_file_request.operation == FILE_STORE_OP_SELFTEST) {
            memset(&s_file_response, 0, sizeof(s_file_response));
            s_file_response.token = s_file_request.token;
            s_file_response.operation = FILE_STORE_OP_SELFTEST;
            s_file_response.result = s_file_request.confirm == 0U ? FILE_STORE_ERROR_CONFIRM :
                (s_files.upload_active != 0U ? FILE_STORE_ERROR_BUSY : (int32_t)w25q128_init(&s_context));
            if (s_file_response.result == W25Q128_OK) {
                s_file_response.result = (int32_t)s_run_test();
            }
            if (s_file_request.confirm != 0U && s_files.upload_active == 0U) {
                s_maintenance_fault = s_fault_mask(s_file_response.result);
                s_maintenance_valid = 1U;
                s_maintenance_result = (w25q128_status_t)s_file_response.result;
                taskENTER_CRITICAL();
                s_diagnostics.test_status = s_file_response.result == 0 ?
                    STORAGE_TEST_PASSED : STORAGE_TEST_FAILED;
                s_diagnostics.result = (w25q128_status_t)s_file_response.result;
                taskEXIT_CRITICAL();
            }
            deferred = 0U;
        } else {
            deferred = file_store_execute(&s_file_request, &s_file_response);
        }
        if (deferred == 0U) {
            taskENTER_CRITICAL();
            s_file_state = STORAGE_FILE_COMPLETE;
            taskEXIT_CRITICAL();
        }
    }
    file_store_step();
    if (s_file_state == STORAGE_FILE_RUNNING && file_store_poll(&s_file_response) != 0U) {
        taskENTER_CRITICAL();
        s_file_state = STORAGE_FILE_COMPLETE;
        taskEXIT_CRITICAL();
    }
    s_files_publish();
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
    uint32_t file_fault = 0U;
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
    file_store_init(online != 0U ? &s_context : NULL, s_mark_dirty);
    s_files_publish();
    /* Configuration recovery and scan recovery are independent: a successful
     * scan must not bypass reload after an ambiguous configuration commit. */
    for (;;) {
        now = xTaskGetTickCount();
        if (scan.running == 0U && s_scan_paused == 0U && s_files.upload_active == 0U &&
            s_file_state != STORAGE_FILE_RUNNING && (TickType_t)(now - scan.completed_tick) >=
            pdMS_TO_TICKS(STORAGE_USAGE_INTERVAL_MS)) {
            s_publish_usage(CLOCK_DATA_INITIALIZING, 0U, W25Q128_OK);
            scan_result = w25q128_init(&s_context);
            if (scan_result == W25Q128_OK) {
                scan.address = 0U;
                scan.used_bytes = 0U;
                memset(s_scan_occupied, 0, sizeof(s_scan_occupied));
                memset(s_scan_dirty, 0, sizeof(s_scan_dirty));
                scan.running = 1U;
                if ((s_files.state == FILE_STORE_UNAVAILABLE || s_files.state == FILE_STORE_ERROR) &&
                    s_files.upload_active == 0U && s_file_state == STORAGE_FILE_IDLE) {
                    file_store_init(&s_context, s_mark_dirty);
                }
            } else {
                online = 0U;
                scan.completed_tick = xTaskGetTickCount();
                scan_fault = s_fault_mask(scan_result);
                taskENTER_CRITICAL();
                s_publish_usage(CLOCK_DATA_ERROR, 0U, scan_result);
                clock_fault_report(CLOCK_FAULT_W25Q, boot_fault | config_fault | scan_fault |
                                   file_fault | s_maintenance_fault);
                taskEXIT_CRITICAL();
            }
        }
        now = xTaskGetTickCount();
        elapsed = (TickType_t)(now - scan.completed_tick);
        wait = ((scan.running != 0U && s_scan_paused == 0U && s_files.upload_active == 0U) ||
                s_files.state == FILE_STORE_INITIALIZING || s_file_state == STORAGE_FILE_RUNNING) ?
            STORAGE_SCAN_YIELD_TICKS :
            ((scan.running != 0U || s_scan_paused != 0U || s_files.upload_active != 0U) ?
             pdMS_TO_TICKS(STORAGE_USAGE_INTERVAL_MS) :
             (elapsed >= pdMS_TO_TICKS(STORAGE_USAGE_INTERVAL_MS) ? 0U :
              pdMS_TO_TICKS(STORAGE_USAGE_INTERVAL_MS) - elapsed));
        /* token zero is a wake-only message for the separate file mailbox. */
        if (xQueueReceive(s_request_queue, &request, wait) == pdPASS && request.token != 0U) {
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
                s_mark_dirty(CLOCK_CONFIG_SECTOR_A, 2U * W25Q128_SECTOR_SIZE);
                result = clock_config_save(&s_context, &state, request.format);
            }
            online = result == W25Q128_OK ? 1U : 0U;
            config_fault = s_fault_mask(result);
            diagnostics.config_result = result;
            diagnostics.config_degraded = state.degraded;
            diagnostics.result = (w25q128_status_t)result;
            taskENTER_CRITICAL();
            clock_fault_report(CLOCK_FAULT_CONFIG, config_fault);
            clock_fault_report(CLOCK_FAULT_W25Q, boot_fault | config_fault | scan_fault |
                               file_fault | s_maintenance_fault);
            s_publish(&diagnostics);
            s_publish_config(&state, 1U, 1U, request.token, result);
            taskEXIT_CRITICAL();
        }
        s_files_process();
        file_fault = s_files.state == FILE_STORE_ERROR ?
            (s_files.flash_result == W25Q128_OK ? CLOCK_FAULT_IO :
             s_fault_mask(s_files.flash_result)) : 0U;
        taskENTER_CRITICAL();
        clock_fault_report(CLOCK_FAULT_W25Q, boot_fault | config_fault | scan_fault |
                           file_fault | s_maintenance_fault);
        taskEXIT_CRITICAL();
        if (scan.running != 0U && s_scan_paused == 0U &&
            s_files.upload_active == 0U && s_file_state != STORAGE_FILE_RUNNING) {
            scan_result = s_scan_step(&scan);
            if (scan_result != W25Q128_OK ||
                (scan.address == W25Q128_TOTAL_SIZE && s_scan_clean() != 0U)) {
                scan.running = 0U;
                scan.completed_tick = xTaskGetTickCount();
                scan_fault = s_fault_mask(scan_result);
                if (scan_result != W25Q128_OK) { online = 0U; }
                taskENTER_CRITICAL();
                s_publish_usage(scan_result == W25Q128_OK ? CLOCK_DATA_READY : CLOCK_DATA_ERROR,
                                scan_result == W25Q128_OK ? scan.used_bytes : 0U, scan_result);
                clock_fault_report(CLOCK_FAULT_W25Q, boot_fault | config_fault | scan_fault |
                                   file_fault | s_maintenance_fault);
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

int32_t storage_file_request(const file_store_request_t *p_request)
{
    storage_request_t wake = {CLOCK_FORMAT_24, 0U};
    if (p_request == NULL || p_request->token == 0U ||
        p_request->length > FILE_STORE_DATA_MAX ||
        p_request->operation < FILE_STORE_OP_STATUS ||
        p_request->operation > FILE_STORE_OP_SELFTEST) {
        return FILE_STORE_ERROR_ARGUMENT;
    }
    taskENTER_CRITICAL();
    if (s_storage_task_handle == NULL || s_request_queue == NULL) {
        taskEXIT_CRITICAL();
        return FILE_STORE_ERROR_NOT_READY;
    }
    if (s_file_state != STORAGE_FILE_IDLE) {
        taskEXIT_CRITICAL();
        return FILE_STORE_ERROR_BUSY;
    }
    s_file_request = *p_request;
    s_file_state = STORAGE_FILE_PENDING;
    s_files.busy = 1U;
    taskEXIT_CRITICAL();
    /* A full queue already contains configuration work that wakes the owner.
     * The file mailbox is retained independently and processed in either case. */
    (void)xQueueSend(s_request_queue, &wake, 0U);
    return FILE_STORE_OK;
}

uint8_t storage_file_take_result(uint32_t token, file_store_response_t *p_response)
{
    uint8_t complete = 0U;
    if (token == 0U || p_response == NULL) { return 0U; }
    taskENTER_CRITICAL();
    if (s_file_state == STORAGE_FILE_COMPLETE && s_file_response.token == token) {
        *p_response = s_file_response;
        s_file_state = STORAGE_FILE_IDLE;
        complete = 1U;
    }
    taskEXIT_CRITICAL();
    return complete;
}

void storage_get_files_snapshot(file_store_snapshot_t *p_snapshot)
{
    if (p_snapshot != NULL) {
        taskENTER_CRITICAL();
        *p_snapshot = s_files;
        taskEXIT_CRITICAL();
    }
}

void storage_set_scan_paused(uint8_t paused)
{
    storage_request_t wake = {CLOCK_FORMAT_24, 0U};
    QueueHandle_t queue;
    taskENTER_CRITICAL();
    s_scan_paused = (uint8_t)(paused != 0U);
    queue = s_request_queue;
    taskEXIT_CRITICAL();
    if (queue != NULL) { (void)xQueueSend(queue, &wake, 0U); }
}
