#include "storage.h"
#include "storage_files.h"
#include "clock_config.h"
#include "clock_faults.h"
#include "cmsis_os.h"
#include "queue.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#define TEST_LENGTH 1024U
#define TEST_CONFIG_TOKEN 200U
#define TEST_COMMIT_CONFIG_TOKEN 201U

typedef struct {
    w25q128_status_t read_result;
    w25q128_status_t program_result;
    w25q128_status_t erase_result;
    uint32_t fault_mask;
} flash_fault_case_t;

static const flash_fault_case_t s_flash_fault_cases[] = {
    {W25Q128_ERROR_TIMEOUT, W25Q128_OK, W25Q128_OK, CLOCK_FAULT_TIMEOUT},
    {W25Q128_OK, W25Q128_ERROR_TIMEOUT, W25Q128_OK, CLOCK_FAULT_TIMEOUT},
    {W25Q128_OK, W25Q128_OK, W25Q128_ERROR_TIMEOUT, CLOCK_FAULT_TIMEOUT},
    {W25Q128_OK, W25Q128_ERROR_VERIFY, W25Q128_OK, CLOCK_FAULT_VERIFY},
    {W25Q128_OK, W25Q128_OK, W25Q128_ERROR_PROTECTED, CLOCK_FAULT_IO}
};

static uint8_t s_flash[W25Q128_TOTAL_SIZE];
static uint8_t s_payload[TEST_LENGTH];
static file_store_request_t s_request;
static file_store_response_t s_response;
static uint8_t *s_queue_bytes;
static uint8_t s_queued_bytes[32];
static uint32_t s_queue_size;
static uint32_t s_queue_calls;
static uint32_t s_token;
static uint32_t s_pending;
static uint32_t s_critical;
static uint32_t s_stage;
static uint32_t s_offset;
static uint32_t s_io;
static uint32_t s_faults[CLOCK_FAULT_SOURCE_COUNT];
static TickType_t s_tick;
static osThreadFunc_t s_entry;
static uint8_t s_queued;
static uint8_t s_formatted_start;
static uint8_t s_commit_config;
static uint8_t s_fail_fs_reads;
static uint8_t s_diagnostic_mode;
static uint32_t s_fault_case;
static w25q128_status_t s_read_result;
static w25q128_status_t s_program_result;
static w25q128_status_t s_erase_result;
static jmp_buf s_finished;

void storage_test_enter(void) { s_critical++; }
void storage_test_exit(void) { assert(s_critical != 0U); s_critical--; }
TickType_t xTaskGetTickCount(void) { return s_tick; }

static void s_range(uint32_t address, uint32_t length)
{
    assert(s_critical == 0U);
    assert(address < W25Q128_TOTAL_SIZE);
    assert(length <= W25Q128_TOTAL_SIZE - address);
    s_io++;
}

int32_t stm32_w25q128_bind(w25q128_ctx_t *p_context)
{
    assert(p_context != NULL);
    memset(p_context, 0, sizeof(*p_context));
    return 0;
}

w25q128_status_t w25q128_init(const w25q128_ctx_t *p_context)
{
    assert(p_context != NULL && s_critical == 0U);
    return W25Q128_OK;
}
w25q128_status_t w25q128_read_id(const w25q128_ctx_t *p_context, uint32_t *p_id)
{
    assert(p_context != NULL && p_id != NULL && s_critical == 0U);
    *p_id = W25Q128_JEDEC_ID;
    return W25Q128_OK;
}
w25q128_status_t w25q128_read_registers(const w25q128_ctx_t *p_context,
                                       w25q128_registers_t *p_registers)
{
    assert(p_context != NULL && p_registers != NULL && s_critical == 0U);
    memset(p_registers, 0, sizeof(*p_registers));
    return W25Q128_OK;
}
w25q128_status_t w25q128_read(const w25q128_ctx_t *p_context, uint32_t address,
                              uint8_t *p_data, uint32_t length)
{
    assert(p_context != NULL && p_data != NULL);
    s_range(address, length);
    if (s_read_result != W25Q128_OK && address >= FILE_STORE_BASE &&
        address < CLOCK_CONFIG_SECTOR_A) { return s_read_result; }
    if (s_fail_fs_reads != 0U && address >= FILE_STORE_BASE &&
        address < CLOCK_CONFIG_SECTOR_A) { return W25Q128_ERROR_IO; }
    memcpy(p_data, s_flash + address, length);
    return W25Q128_OK;
}
w25q128_status_t w25q128_program(const w25q128_ctx_t *p_context, uint32_t address,
                                 const uint8_t *p_data, uint32_t length)
{
    uint32_t i;
    assert(p_context != NULL && p_data != NULL);
    s_range(address, length);
    if (s_program_result != W25Q128_OK && address >= FILE_STORE_BASE &&
        address < CLOCK_CONFIG_SECTOR_A) { return s_program_result; }
    for (i = 0U; i < length; i++) {
        assert((s_flash[address + i] & p_data[i]) == p_data[i]);
        s_flash[address + i] &= p_data[i];
    }
    return W25Q128_OK;
}
w25q128_status_t w25q128_erase_sector(const w25q128_ctx_t *p_context,
                                      uint32_t address)
{
    assert(p_context != NULL && address % W25Q128_SECTOR_SIZE == 0U);
    s_range(address, W25Q128_SECTOR_SIZE);
    if (s_erase_result != W25Q128_OK && address >= FILE_STORE_BASE &&
        address < CLOCK_CONFIG_SECTOR_A) { return s_erase_result; }
    memset(s_flash + address, 0xFF, W25Q128_SECTOR_SIZE);
    return W25Q128_OK;
}
w25q128_status_t w25q128_update(const w25q128_ctx_t *p_context, uint32_t address,
                                const uint8_t *p_data, uint32_t length,
                                uint8_t *p_scratch, uint32_t scratch_length)
{
    (void)p_context; (void)address; (void)p_data; (void)length;
    (void)p_scratch; (void)scratch_length;
    assert(!"normal startup/files must not run destructive selftest");
    return W25Q128_ERROR_ARGUMENT;
}

void clock_fault_report(clock_fault_source_t source, uint32_t mask)
{
    assert(source < CLOCK_FAULT_SOURCE_COUNT);
    s_faults[source] = mask;
}

QueueHandle_t xQueueCreateStatic(uint32_t length, uint32_t item_size,
                                 uint8_t *p_bytes, StaticQueue_t *p_control)
{
    assert(length == 1U && item_size <= sizeof(s_queued_bytes));
    s_queue_bytes = p_bytes;
    s_queue_size = item_size;
    return p_control;
}
int xQueueSend(QueueHandle_t queue, const void *p_item, uint32_t wait)
{
    assert(queue != NULL && p_item != NULL && wait == 0U);
    if (s_queued != 0U) { return 0; }
    memcpy(s_queued_bytes, p_item, s_queue_size);
    s_queued = 1U;
    return pdPASS;
}
osThreadId_t osThreadNew(osThreadFunc_t function, void *p_argument,
                         const osThreadAttr_t *p_attributes)
{
    assert(p_argument == NULL && p_attributes->stack_mem != NULL);
    assert(p_attributes->stack_size == 4096U);
    s_entry = function;
    return p_attributes->cb_mem;
}

static void s_snapshot(file_store_snapshot_t *p_files,
                        storage_config_snapshot_t *p_config)
{
    uint32_t io = s_io;
    storage_get_files_snapshot(p_files);
    storage_get_config_snapshot(p_config);
    assert(s_io == io && s_critical == 0U);
}

static void s_submit(file_store_operation_t operation, const char *p_path)
{
    memset(&s_request, 0, sizeof(s_request));
    s_request.operation = operation;
    s_request.token = ++s_token;
    if (p_path != NULL) { strcpy(s_request.path, p_path); }
}

static void s_send(void)
{
    file_store_snapshot_t files;
    storage_config_snapshot_t config;
    s_pending = s_request.token;
    assert(storage_file_request(&s_request) == 0);
    assert(storage_file_request(&s_request) == FILE_STORE_ERROR_BUSY);
    s_snapshot(&files, &config);
    assert(files.busy != 0U);
    memset(&s_request, 0xA5, sizeof(s_request));
    assert(storage_file_take_result(s_pending + 999U, &s_response) == 0U);
}

static uint8_t s_take(int32_t expected)
{
    if (storage_file_take_result(s_pending, &s_response) == 0U) { return 0U; }
    assert(s_response.token == s_pending && s_response.result == expected);
    assert(storage_file_take_result(s_pending, &s_response) == 0U);
    return 1U;
}

static uint32_t s_physical_used(void)
{
    uint32_t address;
    uint32_t offset;
    uint32_t used = 0U;
    for (address = 0U; address < W25Q128_TOTAL_SIZE; address += W25Q128_SECTOR_SIZE) {
        for (offset = 0U; offset < W25Q128_SECTOR_SIZE; offset++) {
            if (s_flash[address + offset] != 0xFFU) {
                used += W25Q128_SECTOR_SIZE;
                break;
            }
        }
    }
    return used;
}

static void s_drive(void)
{
    file_store_snapshot_t files;
    storage_config_snapshot_t config;
    storage_usage_snapshot_t usage;
    s_snapshot(&files, &config);
    assert(config.ready != 0U);
    switch (s_stage) {
    case 0U:
        if (s_formatted_start != 0U && s_queue_calls < 3500U) { break; }
        assert(storage_request_format(CLOCK_FORMAT_12, TEST_CONFIG_TOKEN) == 0);
        if (s_formatted_start == 0U) {
            assert(files.state == FILE_STORE_INITIALIZING);
            s_submit(FILE_STORE_OP_FORMAT, NULL);
            s_request.confirm = 1U;
            s_send();
            s_stage = 1U;
        } else { s_stage = 2U; }
        break;
    case 1U:
        if (s_take(FILE_STORE_ERROR_NOT_READY) != 0U) { s_stage = 2U; }
        break;
    case 2U:
        if (files.state != FILE_STORE_UNFORMATTED &&
            !(s_formatted_start != 0U && files.state == FILE_STORE_READY)) { break; }
        assert(config.completed_token == TEST_CONFIG_TOKEN &&
               config.completed_result == 0 && config.format == CLOCK_FORMAT_12);
        assert(s_faults[CLOCK_FAULT_W25Q] == 0U);
        s_submit(FILE_STORE_OP_FORMAT, NULL);
        s_request.confirm = 1U;
        s_send();
        s_stage = 3U;
        break;
    case 3U:
        if (s_take(0) == 0U) { break; }
        s_submit(FILE_STORE_OP_UPLOAD_BEGIN, "/current");
        s_request.size = TEST_LENGTH;
        s_request.crc32 = file_store_crc32(0U, s_payload, sizeof(s_payload));
        s_send();
        s_stage = 4U;
        break;
    case 4U:
        if (s_take(0) == 0U) { break; }
        assert(files.upload_active != 0U);
        s_offset = 0U;
        s_submit(FILE_STORE_OP_UPLOAD_WRITE, NULL);
        s_request.length = FILE_STORE_DATA_MAX;
        memcpy(s_request.data, s_payload, FILE_STORE_DATA_MAX);
        s_send();
        s_stage = 5U;
        break;
    case 5U:
        if (s_take(0) == 0U) { break; }
        s_offset += FILE_STORE_DATA_MAX;
        assert(s_response.offset == s_offset);
        if (s_offset < TEST_LENGTH) {
            s_submit(FILE_STORE_OP_UPLOAD_WRITE, NULL);
            s_request.offset = s_offset;
            s_request.length = FILE_STORE_DATA_MAX;
            memcpy(s_request.data, s_payload + s_offset, FILE_STORE_DATA_MAX);
            s_send();
        } else {
            s_submit(FILE_STORE_OP_UPLOAD_COMMIT, NULL);
            s_send();
            s_stage = 6U;
        }
        break;
    case 6U:
        if (s_commit_config == 0U && files.busy != 0U) {
            assert(storage_request_format(CLOCK_FORMAT_24, TEST_COMMIT_CONFIG_TOKEN) == 0);
            s_commit_config = 1U;
        }
        if (s_take(0) == 0U) { break; }
        assert(s_response.size == TEST_LENGTH && s_response.crc32 ==
               file_store_crc32(0U, s_payload, sizeof(s_payload)));
        assert(config.completed_token == TEST_COMMIT_CONFIG_TOKEN && config.completed_result == 0);
        assert(s_commit_config != 0U);
        s_submit(FILE_STORE_OP_READ, "/current");
        s_request.length = FILE_STORE_DATA_MAX;
        s_send();
        s_stage = 7U;
        break;
    case 7U:
        if (s_take(0) == 0U) { break; }
        assert(s_response.length == FILE_STORE_DATA_MAX && s_response.size == TEST_LENGTH);
        assert(memcmp(s_response.data, s_payload, FILE_STORE_DATA_MAX) == 0);
        if (s_formatted_start == 0U) { s_tick += STORAGE_USAGE_INTERVAL_MS; }
        s_stage = 8U;
        break;
    case 8U:
        storage_get_usage_snapshot(&usage);
        if (usage.state != CLOCK_DATA_READY) { break; }
        assert(usage.used_bytes == s_physical_used());
        assert(files.state == FILE_STORE_READY && files.used_blocks > 0U);
        s_fail_fs_reads = 1U;
        s_submit(FILE_STORE_OP_READ, "/current");
        s_request.length = FILE_STORE_DATA_MAX;
        s_send();
        s_stage = 9U;
        break;
    case 9U:
        if (s_take(-5) == 0U) { break; }
        assert(files.state == FILE_STORE_ERROR);
        assert(s_faults[CLOCK_FAULT_W25Q] != 0U);
        s_fail_fs_reads = 0U;
        s_tick += STORAGE_USAGE_INTERVAL_MS;
        s_stage = 10U;
        break;
    case 10U:
        if (files.state != FILE_STORE_READY) { break; }
        assert(s_faults[CLOCK_FAULT_W25Q] == 0U);
        assert(config.ready != 0U && config.format == CLOCK_FORMAT_24);
        assert(files.flash_result == W25Q128_OK);
        if (s_diagnostic_mode != 0U) { s_stage = 20U; break; }
        longjmp(s_finished, 1);
        break;
    case 20U:
        storage_set_scan_paused(1U);
        if (s_fault_case < sizeof(s_flash_fault_cases) / sizeof(s_flash_fault_cases[0])) {
            s_read_result = s_flash_fault_cases[s_fault_case].read_result;
            s_program_result = s_flash_fault_cases[s_fault_case].program_result;
            s_erase_result = s_flash_fault_cases[s_fault_case].erase_result;
            if (s_read_result != W25Q128_OK) {
                s_submit(FILE_STORE_OP_READ, "/current");
                s_request.length = FILE_STORE_DATA_MAX;
            } else {
                s_submit(FILE_STORE_OP_FORMAT, NULL);
                s_request.confirm = 1U;
            }
            s_send();
            s_stage = 21U;
        } else {
            /* Leave TIMEOUT latched, then let recovery discover metadata
             * corruption through working reads. Its cause must be IO, not
             * the old timeout from a different error domain/transaction. */
            s_erase_result = W25Q128_ERROR_TIMEOUT;
            s_submit(FILE_STORE_OP_FORMAT, NULL);
            s_request.confirm = 1U;
            s_send();
            s_stage = 25U;
        }
        break;
    case 21U:
        if (s_take(-5) == 0U) { break; }
        assert(files.state == FILE_STORE_ERROR && files.result == -5);
        assert(files.flash_result == (s_read_result != W25Q128_OK ? s_read_result :
            (s_program_result != W25Q128_OK ? s_program_result : s_erase_result)));
        assert(s_faults[CLOCK_FAULT_W25Q] == s_flash_fault_cases[s_fault_case].fault_mask);
        s_read_result = W25Q128_OK;
        s_program_result = W25Q128_OK;
        s_erase_result = W25Q128_OK;
        s_submit(FILE_STORE_OP_STATUS, NULL);
        s_send();
        s_stage = 22U;
        break;
    case 22U:
        if (s_take(-5) == 0U) { break; }
        assert(s_faults[CLOCK_FAULT_W25Q] == s_flash_fault_cases[s_fault_case].fault_mask);
        if (s_fault_case == 0U) {
            storage_set_scan_paused(0U);
            s_tick += STORAGE_USAGE_INTERVAL_MS;
            s_stage = 23U;
        } else {
            s_submit(FILE_STORE_OP_FORMAT, NULL);
            s_request.confirm = 1U;
            s_send();
            s_stage = 24U;
        }
        break;
    case 23U:
        if (files.state != FILE_STORE_READY) { break; }
        assert(files.flash_result == W25Q128_OK && s_faults[CLOCK_FAULT_W25Q] == 0U);
        s_fault_case++;
        s_stage = 20U;
        break;
    case 24U:
        if (s_take(0) == 0U) { break; }
        assert(files.state == FILE_STORE_READY && files.flash_result == W25Q128_OK);
        assert(s_faults[CLOCK_FAULT_W25Q] == 0U);
        s_fault_case++;
        s_stage = 20U;
        break;
    case 25U:
        if (s_take(-5) == 0U) { break; }
        assert(files.flash_result == W25Q128_ERROR_TIMEOUT);
        assert(s_faults[CLOCK_FAULT_W25Q] == CLOCK_FAULT_TIMEOUT);
        s_erase_result = W25Q128_OK;
        memset(s_flash + FILE_STORE_BASE, 0xA5,
               FILE_STORE_BLOCK_COUNT * FILE_STORE_BLOCK_SIZE);
        storage_set_scan_paused(0U);
        s_tick += STORAGE_USAGE_INTERVAL_MS;
        s_stage = 26U;
        break;
    case 26U:
        if (files.state != FILE_STORE_ERROR || files.result != -84) { break; }
        assert(files.flash_result == W25Q128_OK);
        assert(s_faults[CLOCK_FAULT_W25Q] == CLOCK_FAULT_IO);
        s_submit(FILE_STORE_OP_FORMAT, NULL);
        s_request.confirm = 1U;
        s_send();
        s_stage = 27U;
        break;
    case 27U:
        if (s_take(0) == 0U) { break; }
        assert(files.state == FILE_STORE_READY && files.flash_result == W25Q128_OK);
        assert(s_faults[CLOCK_FAULT_W25Q] == 0U);
        longjmp(s_finished, 1);
        break;
    default: assert(!"invalid integration stage"); break;
    }
}

int xQueueReceive(QueueHandle_t queue, void *p_item, uint32_t wait)
{
    assert(queue != NULL && p_item != NULL && s_critical == 0U);
    assert(++s_queue_calls < 100000U);
    s_drive();
    if (s_queued != 0U) {
        memcpy(p_item, s_queued_bytes, s_queue_size);
        s_queued = 0U;
        return pdPASS;
    }
    if (wait != portMAX_DELAY) { s_tick += wait; }
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t i;
    uint32_t steps;
    file_store_snapshot_t files;
    w25q128_ctx_t fixture;
    assert(argc == 2);
    s_diagnostic_mode = (uint8_t)(strcmp(argv[1], "diagnostics") == 0);
    s_formatted_start = (uint8_t)(strcmp(argv[1], "formatted") == 0 || s_diagnostic_mode != 0U);
    memset(s_flash, 0xFF, sizeof(s_flash));
    for (i = 0U; i < sizeof(s_payload); i++) { s_payload[i] = (uint8_t)(i * 37U); }
    if (s_formatted_start != 0U) {
        memset(&fixture, 0, sizeof(fixture));
        file_store_init(&fixture, NULL);
        for (steps = 0U; steps < 60000U; steps++) {
            file_store_step();
            file_store_get_snapshot(&files);
            if (files.state == FILE_STORE_UNFORMATTED) { break; }
        }
        assert(steps < 60000U);
        s_submit(FILE_STORE_OP_FORMAT, NULL);
        s_request.confirm = 1U;
        assert(file_store_execute(&s_request, &s_response) == 0U);
        assert(s_response.result == 0);
    }
    assert(storage_start() == 0);
    assert(s_entry != NULL && s_queue_bytes != NULL);
    if (setjmp(s_finished) == 0) { s_entry(NULL); assert(!"storage task returned"); }
    assert(s_critical == 0U);
    printf("PASS: real storage+littlefs %s, copied mailbox/config during init+commit/dirty scan/IO recovery (%lu loops)\n",
            argv[1], (unsigned long)s_queue_calls);
    if (s_diagnostic_mode != 0U) {
        puts("PASS: real storageTask read/program/erase timeout, verify/protection classification and recovery");
    }
    return 0;
}
