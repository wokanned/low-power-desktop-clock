#include "file_store.h"
#include "littlefs/lfs.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#define TEST_PARTITION_END (FILE_STORE_BASE + FILE_STORE_BLOCK_COUNT * FILE_STORE_BLOCK_SIZE)
#define TEST_MAX_STEPS 60000U
#define TEST_DATA_SIZE 8192U

static uint8_t s_flash[W25Q128_TOTAL_SIZE];
static uint8_t s_backup[W25Q128_TOTAL_SIZE];
static uint8_t s_pattern[TEST_DATA_SIZE];
static uint8_t s_old[TEST_DATA_SIZE];
static uint8_t s_readback[TEST_DATA_SIZE];
static file_store_request_t s_request;
static file_store_response_t s_response;
static w25q128_ctx_t s_context;
static jmp_buf s_power_cut;
static uint32_t s_token;
static uint32_t s_writes;
static uint32_t s_erase_count;
static uint32_t s_read_count;
static uint32_t s_cut_at;
static uint32_t s_mutations;
static uint32_t s_mark_address;
static uint32_t s_mark_length;
static uint32_t s_fail_reads;
static uint32_t s_fail_usage_after;
static uint32_t s_usage_read_faults;
static w25q128_status_t s_read_result;
static w25q128_status_t s_program_result;
static w25q128_status_t s_erase_result;
static uint8_t s_program_fail_once;

/* Link-time wrapping keeps fault injection outside production code. Only the
 * selected allocation-statistics traversal loses reads; rename and preceding
 * verification execute against a working NOR. Drop the read cache to ensure
 * the real traversal exercises the NOR read failure rather than a cache hit. */
lfs_ssize_t __real_lfs_fs_size(lfs_t *p_lfs);
lfs_ssize_t __wrap_lfs_fs_size(lfs_t *p_lfs)
{
    lfs_ssize_t result;
    uint32_t reads;
    if (s_fail_usage_after == 0U || --s_fail_usage_after != 0U) {
        return __real_lfs_fs_size(p_lfs);
    }
    reads = s_read_count;
    p_lfs->rcache.block = (lfs_block_t)-1;
    p_lfs->rcache.size = 0U;
    s_fail_reads = 1U;
    result = __real_lfs_fs_size(p_lfs);
    s_fail_reads = 0U;
    assert(s_read_count > reads && result == LFS_ERR_IO);
    s_usage_read_faults++;
    return result;
}

static void s_range(uint32_t address, uint32_t length)
{
    assert(address >= FILE_STORE_BASE);
    assert(address <= TEST_PARTITION_END);
    assert(length <= TEST_PARTITION_END - address);
}

static void s_dirty(uint32_t address, uint32_t length)
{
    s_range(address, length);
    s_mark_address = address;
    s_mark_length = length;
}

w25q128_status_t w25q128_read(const w25q128_ctx_t *p_context, uint32_t address,
                              uint8_t *p_data, uint32_t length)
{
    assert(p_context == &s_context);
    s_range(address, length);
    s_read_count++;
    if (s_read_result != W25Q128_OK) { return s_read_result; }
    if (s_fail_reads != 0U) { return W25Q128_ERROR_IO; }
    memcpy(p_data, s_flash + address, length);
    return W25Q128_OK;
}

w25q128_status_t w25q128_program(const w25q128_ctx_t *p_context, uint32_t address,
                                 const uint8_t *p_data, uint32_t length)
{
    uint32_t i;
    uint32_t programmed;
    w25q128_status_t result;
    assert(p_context == &s_context);
    s_range(address, length);
    assert(address % W25Q128_PAGE_SIZE == 0U);
    assert(length % W25Q128_PAGE_SIZE == 0U);
    assert(s_mark_address == address && s_mark_length == length);
    if (s_program_result != W25Q128_OK) {
        result = s_program_result;
        if (s_program_fail_once != 0U) { s_program_result = W25Q128_OK; }
        return result;
    }
    s_writes++;
    s_mutations++;
    programmed = s_cut_at != 0U && s_mutations == s_cut_at ? length / 2U : length;
    for (i = 0U; i < programmed; i++) {
        assert((s_flash[address + i] & p_data[i]) == p_data[i]);
        s_flash[address + i] &= p_data[i];
    }
    if (programmed != length) { longjmp(s_power_cut, 1); }
    return W25Q128_OK;
}

w25q128_status_t w25q128_erase_sector(const w25q128_ctx_t *p_context,
                                      uint32_t address)
{
    uint32_t erased;
    assert(p_context == &s_context);
    s_range(address, W25Q128_SECTOR_SIZE);
    assert(address % W25Q128_SECTOR_SIZE == 0U);
    assert(s_mark_address == address && s_mark_length == W25Q128_SECTOR_SIZE);
    if (s_erase_result != W25Q128_OK) { return s_erase_result; }
    s_erase_count++;
    s_mutations++;
    erased = s_cut_at != 0U && s_mutations == s_cut_at ?
        W25Q128_SECTOR_SIZE / 2U : W25Q128_SECTOR_SIZE;
    memset(s_flash + address, 0xFF, erased);
    if (erased != W25Q128_SECTOR_SIZE) { longjmp(s_power_cut, 1); }
    return W25Q128_OK;
}

static file_store_snapshot_t s_snapshot(void)
{
    file_store_snapshot_t snapshot;
    file_store_get_snapshot(&snapshot);
    assert(snapshot.total_blocks == FILE_STORE_BLOCK_COUNT);
    return snapshot;
}

static void s_command(file_store_operation_t operation, const char *p_path)
{
    memset(&s_request, 0, sizeof(s_request));
    s_request.operation = operation;
    s_request.token = ++s_token;
    if (p_path != NULL) {
        assert(strlen(p_path) <= FILE_STORE_PATH_MAX);
        strcpy(s_request.path, p_path);
    }
}

static int32_t s_execute(void)
{
    uint32_t steps;
    if (file_store_execute(&s_request, &s_response) != 0U) {
        for (steps = 0U; steps < TEST_MAX_STEPS; steps++) {
            file_store_step();
            if (file_store_poll(&s_response) != 0U) { break; }
        }
        assert(steps < TEST_MAX_STEPS);
        assert(file_store_poll(&s_response) == 0U);
    }
    assert(s_response.token == s_request.token);
    assert(s_response.operation == s_request.operation);
    return s_response.result;
}

static void s_init(void)
{
    uint32_t steps;
    file_store_init(&s_context, s_dirty);
    for (steps = 0U; steps < TEST_MAX_STEPS &&
         s_snapshot().state == FILE_STORE_INITIALIZING; steps++) {
        file_store_step();
    }
    assert(steps < TEST_MAX_STEPS);
}

static void s_fresh(void)
{
    memset(s_flash, 0xA5, sizeof(s_flash));
    memset(s_flash + FILE_STORE_BASE, 0xFF,
            FILE_STORE_BLOCK_COUNT * FILE_STORE_BLOCK_SIZE);
    s_writes = 0U;
    s_erase_count = 0U;
    s_cut_at = 0U;
    s_fail_reads = 0U;
    s_read_result = W25Q128_OK;
    s_program_result = W25Q128_OK;
    s_erase_result = W25Q128_OK;
    s_program_fail_once = 0U;
    s_init();
    assert(s_snapshot().state == FILE_STORE_UNFORMATTED);
    assert(s_writes == 0U && s_erase_count == 0U);
    s_command(FILE_STORE_OP_FORMAT, NULL);
    assert(s_execute() == FILE_STORE_ERROR_CONFIRM);
    s_request.confirm = 1U;
    assert(s_execute() == 0);
    assert(s_snapshot().state == FILE_STORE_READY);
    assert(s_snapshot().flash_result == W25Q128_OK);
    assert(s_snapshot().used_blocks > 0U);
}

static void s_upload_begin(const char *p_path, const uint8_t *p_data, uint32_t length,
                            uint32_t crc_xor)
{
    s_command(FILE_STORE_OP_UPLOAD_BEGIN, p_path);
    s_request.size = length;
    s_request.crc32 = file_store_crc32(0U, p_data, length) ^ crc_xor;
    assert(s_execute() == 0);
    assert(s_snapshot().upload_active != 0U);
}

static void s_upload_write(const uint8_t *p_data, uint32_t length)
{
    uint32_t offset;
    uint32_t chunk;
    for (offset = 0U; offset < length; offset += chunk) {
        chunk = length - offset;
        if (chunk > FILE_STORE_DATA_MAX) { chunk = FILE_STORE_DATA_MAX; }
        s_command(FILE_STORE_OP_UPLOAD_WRITE, NULL);
        s_request.offset = offset;
        s_request.length = (uint16_t)chunk;
        memcpy(s_request.data, p_data + offset, chunk);
        assert(s_execute() == 0);
        assert(s_response.offset == offset + chunk);
    }
}

static void s_upload(const char *p_path, const uint8_t *p_data, uint32_t length)
{
    s_upload_begin(p_path, p_data, length, 0U);
    s_upload_write(p_data, length);
    s_command(FILE_STORE_OP_UPLOAD_COMMIT, NULL);
    assert(s_execute() == 0);
    assert(s_response.size == length);
    assert(s_response.crc32 == file_store_crc32(0U, p_data, length));
    assert(s_snapshot().upload_active == 0U && s_snapshot().busy == 0U);
}

static uint32_t s_read_file(const char *p_path)
{
    uint32_t offset;
    uint32_t size = 0U;
    for (offset = 0U; offset < sizeof(s_readback); offset += s_response.length) {
        s_command(FILE_STORE_OP_READ, p_path);
        s_request.offset = offset;
        s_request.length = FILE_STORE_DATA_MAX;
        assert(s_execute() == 0);
        assert(s_response.offset == offset);
        if (offset == 0U) { size = s_response.size; }
        assert(s_response.size == size);
        assert(offset + s_response.length <= sizeof(s_readback));
        memcpy(s_readback + offset, s_response.data, s_response.length);
        if (s_response.length == 0U || offset + s_response.length == size) { break; }
    }
    return size;
}

static void s_test_paths(void)
{
    char path[FILE_STORE_PATH_MAX + 2U];
    const char *invalid[] = {"", "a", "//", "/a/", "/a//b", "/.", "/..",
        "/a/../b", "/a/./b", "/.upload.tmp", "/a/.upload.tmp", "/a\\b", "/a\tb"};
    uint32_t i;
    assert(file_store_path_valid("/") != 0U);
    assert(file_store_path_valid("/images/weather 1.bin") != 0U);
    for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(file_store_path_valid(invalid[i]) == 0U);
    }
    memset(path, 'a', sizeof(path));
    path[0] = '/';
    path[32] = '\0';
    assert(file_store_path_valid(path) != 0U);
    path[32] = 'a'; path[33] = '\0';
    assert(file_store_path_valid(path) == 0U);
    assert(file_store_crc32(0U, (const uint8_t *)"123456789", 9U) == 0xCBF43926UL);
    assert(file_store_crc32(file_store_crc32(0U, (const uint8_t *)"1234", 4U),
                            (const uint8_t *)"56789", 5U) == 0xCBF43926UL);
}

static void s_test_files(void)
{
    uint32_t offset;
    uint32_t entries = 0U;
    s_fresh();
    s_command(FILE_STORE_OP_MKDIR, "/data");
    assert(s_execute() == 0);
    s_upload("/data/old.bin", s_old, sizeof(s_old));
    assert(s_read_file("/data/old.bin") == sizeof(s_old));
    assert(memcmp(s_readback, s_old, sizeof(s_old)) == 0);
    s_command(FILE_STORE_OP_RENAME, "/data/old.bin");
    strcpy(s_request.path2, "/data/current.bin");
    assert(s_execute() == 0);
    s_command(FILE_STORE_OP_STAT, "/data/current.bin");
    assert(s_execute() == 0 && s_response.size == sizeof(s_old));
    assert(s_response.type == LFS_TYPE_REG);
    s_upload("/empty", NULL, 0U);
    assert(s_read_file("/empty") == 0U);
    offset = 0U;
    do {
        s_command(FILE_STORE_OP_LIST, "/");
        s_request.offset = offset;
        assert(s_execute() == 0);
        if (s_response.length != 0U) {
            assert(s_response.offset > offset);
            entries++;
        }
        offset = s_response.offset;
    } while (s_response.length != 0U);
    assert(entries == 2U);
    s_command(FILE_STORE_OP_REMOVE, "/empty");
    assert(s_execute() == 0);
    s_init();
    assert(s_snapshot().state == FILE_STORE_READY);
    assert(s_read_file("/data/current.bin") == sizeof(s_old));
    assert(memcmp(s_readback, s_old, sizeof(s_old)) == 0);
    assert(s_flash[0] == 0xA5U && s_flash[FILE_STORE_BASE - 1U] == 0xA5U);
    assert(s_flash[TEST_PARTITION_END] == 0xA5U &&
           s_flash[W25Q128_TOTAL_SIZE - 1U] == 0xA5U);
}

static void s_test_cancel_crc_and_gate(void)
{
    s_fresh();
    s_upload("/current", s_old, sizeof(s_old));
    s_upload_begin("/current", s_pattern, sizeof(s_pattern), 1U);
    s_command(FILE_STORE_OP_FORMAT, NULL);
    s_request.confirm = 1U;
    assert(s_execute() == FILE_STORE_ERROR_BUSY);
    s_command(FILE_STORE_OP_UPLOAD_WRITE, NULL);
    s_request.offset = 1U;
    s_request.length = 1U;
    assert(s_execute() == FILE_STORE_ERROR_SEQUENCE);
    s_command(FILE_STORE_OP_UPLOAD_COMMIT, NULL);
    assert(s_execute() == FILE_STORE_ERROR_SEQUENCE);
    s_upload_write(s_pattern, sizeof(s_pattern));
    s_command(FILE_STORE_OP_UPLOAD_COMMIT, NULL);
    assert(s_execute() == FILE_STORE_ERROR_CRC);
    assert(s_read_file("/current") == sizeof(s_old));
    assert(memcmp(s_readback, s_old, sizeof(s_old)) == 0);
    s_upload_begin("/current", s_pattern, sizeof(s_pattern), 0U);
    s_upload_write(s_pattern, FILE_STORE_DATA_MAX);
    s_command(FILE_STORE_OP_UPLOAD_CANCEL, NULL);
    assert(s_execute() == 0);
    s_init();
    assert(s_read_file("/current") == sizeof(s_old));
    assert(memcmp(s_readback, s_old, sizeof(s_old)) == 0);
    s_command(FILE_STORE_OP_UPLOAD_BEGIN, "/too_large");
    s_request.size = (FILE_STORE_BLOCK_COUNT - FILE_STORE_RESERVED_BLOCKS) *
        FILE_STORE_BLOCK_SIZE;
    assert(s_execute() == LFS_ERR_NOSPC);
    s_command(FILE_STORE_OP_UPLOAD_BEGIN, "/too_large");
    s_request.size = 0xFFFFFFFFUL;
    assert(s_execute() == LFS_ERR_FBIG);
}

static void s_test_unformatted_and_errors(void)
{
    uint32_t reads;
    memset(s_flash, 0xFF, sizeof(s_flash));
    file_store_init(&s_context, s_dirty);
    assert(s_snapshot().state == FILE_STORE_INITIALIZING);
    reads = s_read_count;
    file_store_step();
    assert(s_read_count == reads + 1U);
    s_command(FILE_STORE_OP_MKDIR, "/data");
    assert(s_execute() == FILE_STORE_ERROR_NOT_READY);
    s_flash[TEST_PARTITION_END - 1U] = 0U;
    s_init();
    assert(s_snapshot().state == FILE_STORE_ERROR);
    s_fail_reads = 1U;
    s_init();
    assert(s_snapshot().state == FILE_STORE_ERROR);
    s_fail_reads = 0U;
    file_store_init(NULL, NULL);
    assert(s_snapshot().state == FILE_STORE_UNAVAILABLE);
}

static void s_upload_repeated(const char *p_path, uint32_t length)
{
    uint32_t offset;
    uint32_t crc = 0U;
    assert(length % FILE_STORE_DATA_MAX == 0U);
    for (offset = 0U; offset < length; offset += FILE_STORE_DATA_MAX) {
        crc = file_store_crc32(crc, s_pattern, FILE_STORE_DATA_MAX);
    }
    s_command(FILE_STORE_OP_UPLOAD_BEGIN, p_path);
    s_request.size = length;
    s_request.crc32 = crc;
    assert(s_execute() == 0);
    for (offset = 0U; offset < length; offset += FILE_STORE_DATA_MAX) {
        s_command(FILE_STORE_OP_UPLOAD_WRITE, NULL);
        s_request.offset = offset;
        s_request.length = FILE_STORE_DATA_MAX;
        memcpy(s_request.data, s_pattern, FILE_STORE_DATA_MAX);
        assert(s_execute() == 0);
    }
    s_command(FILE_STORE_OP_UPLOAD_COMMIT, NULL);
    assert(s_execute() == 0);
    assert(s_response.size == length && s_response.crc32 == crc);
}

static void s_test_near_full(void)
{
    uint32_t i;
    char path[] = "/f0";
    s_fresh();
    for (i = 0U; i < 5U; i++) {
        path[2] = (char)('0' + i);
        s_upload_repeated(path, 2U * 1024U * 1024U);
    }
    assert(s_snapshot().used_blocks > 2500U);
    assert(s_snapshot().used_blocks <=
           FILE_STORE_BLOCK_COUNT - FILE_STORE_RESERVED_BLOCKS);
    s_command(FILE_STORE_OP_UPLOAD_BEGIN, "/overflow");
    s_request.size = 256U * 1024U;
    assert(s_execute() == LFS_ERR_NOSPC);
    s_command(FILE_STORE_OP_STAT, "/f0");
    assert(s_execute() == 0 && s_response.size == 2U * 1024U * 1024U);
    s_command(FILE_STORE_OP_REMOVE, "/f0");
    assert(s_execute() == 0);
    s_upload_repeated("/recovered", 256U * 1024U);
    s_init();
    assert(s_snapshot().state == FILE_STORE_READY);
    s_command(FILE_STORE_OP_STAT, "/recovered");
    assert(s_execute() == 0 && s_response.size == 256U * 1024U);
    puts("PASS: near-full 10 MiB retained, reserve rejects growth, remove restores writes");
}

static void s_test_interrupted_format(void)
{
    volatile uint32_t cut;
    uint32_t mutation_count;
    memset(s_flash, 0xA5, sizeof(s_flash));
    memset(s_flash + FILE_STORE_BASE, 0xFF,
            FILE_STORE_BLOCK_COUNT * FILE_STORE_BLOCK_SIZE);
    memcpy(s_backup, s_flash, sizeof(s_backup));
    s_cut_at = 0U;
    s_init();
    s_command(FILE_STORE_OP_FORMAT, NULL);
    s_request.confirm = 1U;
    s_mutations = 0U;
    assert(s_execute() == 0);
    mutation_count = s_mutations;
    for (cut = 1U; cut <= mutation_count; cut++) {
        memcpy(s_flash, s_backup, sizeof(s_flash));
        s_init();
        s_cut_at = cut;
        s_mutations = 0U;
        if (setjmp(s_power_cut) == 0) {
            s_command(FILE_STORE_OP_FORMAT, NULL);
            s_request.confirm = 1U;
            (void)s_execute();
        }
        s_cut_at = 0U;
        s_init();
        assert(s_snapshot().state == FILE_STORE_ERROR ||
               s_snapshot().state == FILE_STORE_UNFORMATTED ||
               s_snapshot().state == FILE_STORE_READY);
        s_command(FILE_STORE_OP_FORMAT, NULL);
        assert(s_execute() == FILE_STORE_ERROR_CONFIRM);
        s_request.confirm = 1U;
        assert(s_execute() == 0);
        assert(s_snapshot().state == FILE_STORE_READY);
        assert(s_flash[0] == 0xA5U && s_flash[TEST_PARTITION_END] == 0xA5U);
    }
    printf("PASS: %lu interrupted format windows recover by explicit confirmed format\n",
            (unsigned long)mutation_count);
}

static void s_test_postcommit_statistics_fault(void)
{
    uint32_t faults;
    s_fresh();
    s_upload("/current", s_old, sizeof(s_old));
    s_upload_begin("/current", s_pattern, sizeof(s_pattern), 0U);
    s_upload_write(s_pattern, sizeof(s_pattern));
    faults = s_usage_read_faults;
    s_fail_usage_after = 1U;
    s_command(FILE_STORE_OP_UPLOAD_COMMIT, NULL);
    assert(s_execute() == 0);
    assert(s_usage_read_faults == faults + 1U);
    assert(s_snapshot().state == FILE_STORE_ERROR && s_snapshot().result == LFS_ERR_IO);
    assert(s_snapshot().flash_result == W25Q128_ERROR_IO);
    assert(s_response.size == sizeof(s_pattern));
    assert(s_response.crc32 == file_store_crc32(0U, s_pattern, sizeof(s_pattern)));
    assert(s_snapshot().upload_active == 0U && s_snapshot().busy == 0U);
    s_init();
    assert(s_snapshot().state == FILE_STORE_READY);
    assert(s_snapshot().flash_result == W25Q128_OK);
    assert(s_read_file("/current") == sizeof(s_pattern));
    assert(memcmp(s_readback, s_pattern, sizeof(s_pattern)) == 0);

    /* mkdir has a preflight statistics check; only the second traversal is
     * after its durable metadata commit. */
    s_fail_usage_after = 2U;
    s_command(FILE_STORE_OP_MKDIR, "/directory");
    assert(s_execute() == 0 && s_snapshot().state == FILE_STORE_ERROR);
    s_init();
    s_command(FILE_STORE_OP_STAT, "/directory");
    assert(s_execute() == 0 && s_response.type == LFS_TYPE_DIR);

    s_fail_usage_after = 1U;
    s_command(FILE_STORE_OP_RENAME, "/current");
    strcpy(s_request.path2, "/renamed");
    assert(s_execute() == 0 && s_snapshot().state == FILE_STORE_ERROR);
    s_init();
    assert(s_read_file("/renamed") == sizeof(s_pattern));
    assert(memcmp(s_readback, s_pattern, sizeof(s_pattern)) == 0);
    s_command(FILE_STORE_OP_STAT, "/current");
    assert(s_execute() == LFS_ERR_NOENT);

    s_fail_usage_after = 1U;
    s_command(FILE_STORE_OP_REMOVE, "/renamed");
    assert(s_execute() == 0 && s_snapshot().state == FILE_STORE_ERROR);
    s_init();
    s_command(FILE_STORE_OP_STAT, "/renamed");
    assert(s_execute() == LFS_ERR_NOENT);

    s_upload_begin("/abandoned", s_pattern, sizeof(s_pattern), 0U);
    s_upload_write(s_pattern, FILE_STORE_DATA_MAX);
    s_fail_usage_after = 1U;
    s_command(FILE_STORE_OP_UPLOAD_CANCEL, NULL);
    assert(s_execute() == 0 && s_snapshot().state == FILE_STORE_ERROR);
    s_init();
    s_command(FILE_STORE_OP_STAT, "/abandoned");
    assert(s_execute() == LFS_ERR_NOENT);
    assert(s_usage_read_faults == faults + 5U);
    puts("PASS: post-commit statistics read faults retain successful mutation ACKs and expose snapshot fault");
}

static void s_test_flash_diagnostics(void)
{
    uint32_t offset;
    int32_t result = 0;
    s_fresh();
    s_upload("/current", s_old, sizeof(s_old));
    s_read_result = W25Q128_ERROR_TIMEOUT;
    s_command(FILE_STORE_OP_READ, "/current");
    s_request.length = FILE_STORE_DATA_MAX;
    assert(s_execute() == LFS_ERR_IO);
    assert(s_snapshot().flash_result == W25Q128_ERROR_TIMEOUT);
    s_read_result = W25Q128_OK;
    s_command(FILE_STORE_OP_STATUS, NULL);
    assert(s_execute() == LFS_ERR_IO);
    assert(s_snapshot().flash_result == W25Q128_ERROR_TIMEOUT);
    s_command(FILE_STORE_OP_READ, "/current");
    assert(s_execute() == FILE_STORE_ERROR_NOT_READY);
    assert(s_snapshot().flash_result == W25Q128_ERROR_TIMEOUT);
    s_init();
    assert(s_snapshot().state == FILE_STORE_READY);
    assert(s_snapshot().flash_result == W25Q128_OK);

    /* A single failed program is followed by working IO during upload cleanup.
     * Keep the cause even though those later operations succeed. */
    s_upload_begin("/current", s_pattern, sizeof(s_pattern), 0U);
    s_program_result = W25Q128_ERROR_TIMEOUT;
    s_program_fail_once = 1U;
    for (offset = 0U; offset < sizeof(s_pattern); offset += FILE_STORE_DATA_MAX) {
        s_command(FILE_STORE_OP_UPLOAD_WRITE, NULL);
        s_request.offset = offset;
        s_request.length = FILE_STORE_DATA_MAX;
        memcpy(s_request.data, s_pattern + offset, FILE_STORE_DATA_MAX);
        result = s_execute();
        if (result != 0) { break; }
    }
    assert(result == LFS_ERR_IO && s_program_result == W25Q128_OK);
    assert(s_snapshot().flash_result == W25Q128_ERROR_TIMEOUT);
    assert(s_snapshot().upload_active == 0U);
    s_init();
    assert(s_snapshot().flash_result == W25Q128_OK);
    assert(s_read_file("/current") == sizeof(s_old));
    assert(memcmp(s_readback, s_old, sizeof(s_old)) == 0);

    /* A sync failure must not retry metadata cleanup through dirty caches. */
    s_upload_begin("/current", s_pattern, FILE_STORE_DATA_MAX, 0U);
    s_upload_write(s_pattern, FILE_STORE_DATA_MAX);
    s_program_result = W25Q128_ERROR_TIMEOUT;
    s_program_fail_once = 1U;
    s_command(FILE_STORE_OP_UPLOAD_COMMIT, NULL);
    assert(s_execute() == LFS_ERR_IO);
    assert(s_snapshot().state == FILE_STORE_ERROR);
    assert(s_snapshot().flash_result == W25Q128_ERROR_TIMEOUT);
    assert(s_snapshot().upload_active == 0U);
    s_init();
    assert(s_snapshot().state == FILE_STORE_READY);
    assert(s_read_file("/current") == sizeof(s_old));
    assert(memcmp(s_readback, s_old, sizeof(s_old)) == 0);

    /* Readback failure follows a successful sync/close, before rename. */
    s_upload_begin("/current", s_pattern, sizeof(s_pattern), 0U);
    s_upload_write(s_pattern, sizeof(s_pattern));
    s_command(FILE_STORE_OP_UPLOAD_COMMIT, NULL);
    assert(file_store_execute(&s_request, &s_response) == 1U);
    s_read_result = W25Q128_ERROR_TIMEOUT;
    file_store_step();
    assert(file_store_poll(&s_response) == 1U && s_response.result == LFS_ERR_IO);
    assert(s_snapshot().state == FILE_STORE_ERROR && s_snapshot().upload_active == 0U);
    assert(s_snapshot().flash_result == W25Q128_ERROR_TIMEOUT);
    s_read_result = W25Q128_OK;
    s_command(FILE_STORE_OP_UPLOAD_CANCEL, NULL);
    assert(s_execute() == LFS_ERR_IO);
    assert(s_snapshot().flash_result == W25Q128_ERROR_TIMEOUT);
    s_init();
    assert(s_snapshot().flash_result == W25Q128_OK);
    assert(s_read_file("/current") == sizeof(s_old));
    assert(memcmp(s_readback, s_old, sizeof(s_old)) == 0);

    s_erase_result = W25Q128_ERROR_TIMEOUT;
    s_command(FILE_STORE_OP_FORMAT, NULL);
    s_request.confirm = 1U;
    assert(s_execute() == LFS_ERR_IO);
    assert(s_snapshot().flash_result == W25Q128_ERROR_TIMEOUT);
    s_erase_result = W25Q128_OK;
    s_program_result = W25Q128_ERROR_VERIFY;
    assert(s_execute() == LFS_ERR_IO);
    assert(s_snapshot().flash_result == W25Q128_ERROR_VERIFY);
    s_program_result = W25Q128_ERROR_PROTECTED;
    assert(s_execute() == LFS_ERR_IO);
    assert(s_snapshot().flash_result == W25Q128_ERROR_PROTECTED);
    s_program_result = W25Q128_OK;
    assert(s_execute() == 0);
    assert(s_snapshot().state == FILE_STORE_READY);
    assert(s_snapshot().flash_result == W25Q128_OK);

    /* Neither a later pure corruption nor a fresh blank check inherits an old
     * timeout. A failure during the blank check still keeps its raw cause. */
    memset(s_flash + FILE_STORE_BASE, 0xA5, FILE_STORE_BLOCK_COUNT * FILE_STORE_BLOCK_SIZE);
    s_init();
    assert(s_snapshot().state == FILE_STORE_ERROR);
    assert(s_snapshot().result == LFS_ERR_CORRUPT);
    assert(s_snapshot().flash_result == W25Q128_OK);
    memset(s_flash + FILE_STORE_BASE, 0xFF, FILE_STORE_BLOCK_COUNT * FILE_STORE_BLOCK_SIZE);
    file_store_init(&s_context, s_dirty);
    assert(s_snapshot().state == FILE_STORE_INITIALIZING);
    s_read_result = W25Q128_ERROR_TIMEOUT;
    file_store_step();
    assert(s_snapshot().state == FILE_STORE_ERROR && s_snapshot().result == LFS_ERR_IO);
    assert(s_snapshot().flash_result == W25Q128_ERROR_TIMEOUT);
    s_read_result = W25Q128_OK;
    s_init();
    assert(s_snapshot().state == FILE_STORE_UNFORMATTED);
    assert(s_snapshot().flash_result == W25Q128_OK);
    puts("PASS: raw Flash errors survive cleanup, stay distinct from littlefs, and clear on recovery");
}

static void s_test_interrupted_replace(void)
{
    volatile uint32_t cut;
    volatile uint32_t tested = 0U;
    uint32_t mutation_count;
    s_fresh();
    s_upload("/current", s_old, sizeof(s_old));
    memcpy(s_backup, s_flash, sizeof(s_backup));
    s_mutations = 0U;
    s_upload("/current", s_pattern, sizeof(s_pattern));
    mutation_count = s_mutations;
    assert(mutation_count > 10U);
    for (cut = 1U; cut <= mutation_count; cut++) {
        memcpy(s_flash, s_backup, sizeof(s_flash));
        s_init();
        s_mutations = 0U;
        s_cut_at = cut;
        if (setjmp(s_power_cut) == 0) {
            s_upload("/current", s_pattern, sizeof(s_pattern));
        } else { tested++; }
        s_cut_at = 0U;
        s_init();
        assert(s_snapshot().state == FILE_STORE_READY);
        assert(s_snapshot().upload_active == 0U);
        assert(s_read_file("/current") == sizeof(s_old));
        assert(memcmp(s_readback, s_old, sizeof(s_old)) == 0 ||
               memcmp(s_readback, s_pattern, sizeof(s_pattern)) == 0);
    }
    assert(tested == mutation_count);
    printf("PASS: %lu partial program/erase power cuts preserve old-or-new file\n",
            (unsigned long)tested);
}

int main(void)
{
    uint32_t i;
    (void)setvbuf(stdout, NULL, _IONBF, 0U);
    for (i = 0U; i < sizeof(s_pattern); i++) {
        s_pattern[i] = (uint8_t)(i * 37U + i / 17U);
        s_old[i] = (uint8_t)(i * 11U + 3U);
    }
    s_test_paths();
    s_test_files();
    s_test_cancel_crc_and_gate();
    s_test_unformatted_and_errors();
    s_test_interrupted_replace();
    s_test_near_full();
    s_test_interrupted_format();
    s_test_postcommit_statistics_fault();
    s_test_flash_diagnostics();
    puts("PASS: file store NOR boundaries, blank scan, CRUD, CRC, cancellation, reserve and recovery");
    return 0;
}
