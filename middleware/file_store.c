#include "file_store.h"
#include "littlefs/lfs.h"
#include <stddef.h>
#include <string.h>

#define FILE_STORE_READ_SIZE 16U
#define FILE_STORE_CACHE_SIZE 256U
#define FILE_STORE_LOOKAHEAD_SIZE 128U
#define FILE_STORE_BLOCK_CYCLES 500
#define FILE_STORE_ERASED_BYTE 0xFFU
#define FILE_STORE_TEMP_PATH "/.upload.tmp"
#define FILE_STORE_TEMP_NAME ".upload.tmp"
#define FILE_STORE_CRC_POLYNOMIAL 0xEDB88320UL
#define FILE_STORE_CRC_FINAL 0xFFFFFFFFUL
#define FILE_STORE_USAGE_INTERVAL FILE_STORE_BLOCK_SIZE
#define FILE_STORE_SPACE_LIMIT (FILE_STORE_BLOCK_COUNT - FILE_STORE_RESERVED_BLOCKS)
#define FILE_STORE_SIZE_BYTES (FILE_STORE_BLOCK_COUNT * FILE_STORE_BLOCK_SIZE)

static const w25q128_ctx_t *s_context;
static void (*s_mark_dirty)(uint32_t address, uint32_t length);
static lfs_t s_lfs;
static struct lfs_config s_lfs_config;
static uint8_t s_read_cache[FILE_STORE_CACHE_SIZE];
static uint8_t s_program_cache[FILE_STORE_CACHE_SIZE];
static uint8_t s_lookahead[FILE_STORE_LOOKAHEAD_SIZE];
static uint8_t s_write_cache[FILE_STORE_CACHE_SIZE];
static uint8_t s_file_cache[FILE_STORE_CACHE_SIZE];
static uint8_t s_data[FILE_STORE_DATA_MAX];
static lfs_file_t s_writer;
static lfs_file_t s_reader;
static const struct lfs_file_config s_writer_config = {s_write_cache, NULL, 0U};
static const struct lfs_file_config s_reader_config = {s_file_cache, NULL, 0U};
static file_store_snapshot_t s_snapshot;
static file_store_response_t s_completion;
static char s_upload_path[FILE_STORE_PATH_MAX + 1U];
static uint32_t s_blank_offset;
static uint32_t s_upload_size;
static uint32_t s_upload_crc;
static uint32_t s_written;
static uint32_t s_verified;
static uint32_t s_verify_crc;
static uint32_t s_usage_offset;
static int32_t s_mount_result;
static w25q128_status_t s_io_result;
static uint8_t s_mounted;
static uint8_t s_writer_open;
static uint8_t s_reader_open;
static uint8_t s_committing;
static uint8_t s_completion_ready;

uint32_t file_store_crc32(uint32_t crc, const uint8_t *p_data, uint32_t length)
{
    uint32_t i;
    uint8_t bit;
    if (p_data == NULL && length != 0U) { return crc; }
    crc ^= FILE_STORE_CRC_FINAL;
    for (i = 0U; i < length; i++) {
        crc ^= p_data[i];
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? FILE_STORE_CRC_POLYNOMIAL : 0U);
        }
    }
    return crc ^ FILE_STORE_CRC_FINAL;
}

uint8_t file_store_path_valid(const char *p_path)
{
    uint32_t length;
    uint32_t component;
    uint32_t start;
    unsigned char byte;
    if (p_path == NULL || p_path[0] != '/') { return 0U; }
    start = 1U;
    for (length = 1U; length <= FILE_STORE_PATH_MAX; length++) {
        byte = (unsigned char)p_path[length];
        if (byte == 0U || byte == '/') {
            component = length - start;
            if (component == 0U) {
                return (uint8_t)(length == 1U && byte == 0U);
            }
            if (component > FILE_STORE_NAME_MAX ||
                (component == 1U && p_path[start] == '.') ||
                (component == 2U && p_path[start] == '.' && p_path[start + 1U] == '.') ||
                (component == sizeof(FILE_STORE_TEMP_NAME) - 1U &&
                 memcmp(p_path + start, FILE_STORE_TEMP_NAME, component) == 0)) {
                return 0U;
            }
            if (byte == 0U) { return 1U; }
            start = length + 1U;
        } else if (byte < 0x20U || byte > 0x7EU || byte == '\\') {
            return 0U;
        }
    }
    return 0U;
}

static uint8_t s_path_mutable(const char *p_path)
{
    return (uint8_t)(file_store_path_valid(p_path) != 0U && p_path[1] != '\0');
}

static int s_check_range(lfs_block_t block, lfs_off_t offset, lfs_size_t size,
                         uint32_t alignment)
{
    if (block >= FILE_STORE_BLOCK_COUNT || offset > FILE_STORE_BLOCK_SIZE ||
        size > FILE_STORE_BLOCK_SIZE - offset || size == 0U ||
        offset % alignment != 0U || size % alignment != 0U) {
        return LFS_ERR_INVAL;
    }
    return 0;
}

static int s_bd_result(w25q128_status_t result)
{
    if (result != W25Q128_OK && s_io_result == W25Q128_OK) {
        s_io_result = result;
    }
    return result == W25Q128_OK ? 0 : LFS_ERR_IO;
}

static int s_bd_read(const struct lfs_config *p_config, lfs_block_t block,
                     lfs_off_t offset, void *p_buffer, lfs_size_t size)
{
    int result;
    (void)p_config;
    result = s_check_range(block, offset, size, FILE_STORE_READ_SIZE);
    if (result != 0 || p_buffer == NULL) { return LFS_ERR_INVAL; }
    return s_bd_result(w25q128_read(s_context,
        FILE_STORE_BASE + block * FILE_STORE_BLOCK_SIZE + offset,
        (uint8_t *)p_buffer, size));
}

static int s_bd_program(const struct lfs_config *p_config, lfs_block_t block,
                        lfs_off_t offset, const void *p_buffer, lfs_size_t size)
{
    int result;
    uint32_t address;
    (void)p_config;
    result = s_check_range(block, offset, size, FILE_STORE_CACHE_SIZE);
    if (result != 0 || p_buffer == NULL) { return LFS_ERR_INVAL; }
    address = FILE_STORE_BASE + block * FILE_STORE_BLOCK_SIZE + offset;
    if (s_mark_dirty != NULL) { s_mark_dirty(address, size); }
    return s_bd_result(w25q128_program(s_context, address,
                                       (const uint8_t *)p_buffer, size));
}

static int s_bd_erase(const struct lfs_config *p_config, lfs_block_t block)
{
    uint32_t address;
    (void)p_config;
    if (block >= FILE_STORE_BLOCK_COUNT) { return LFS_ERR_INVAL; }
    address = FILE_STORE_BASE + block * FILE_STORE_BLOCK_SIZE;
    if (s_mark_dirty != NULL) { s_mark_dirty(address, FILE_STORE_BLOCK_SIZE); }
    return s_bd_result(w25q128_erase_sector(s_context, address));
}

static int s_bd_sync(const struct lfs_config *p_config)
{
    (void)p_config;
    /* The driver already waits for and verifies every program/erase. There is
     * no pending DMA or external write cache to flush at this boundary. */
    return 0;
}

static void s_record_fault(int32_t result)
{
    if (result == LFS_ERR_IO || result == LFS_ERR_CORRUPT) {
        s_snapshot.state = FILE_STORE_ERROR;
        s_snapshot.result = result;
        s_snapshot.flash_result = s_io_result;
    }
}

static int32_t s_refresh_usage(void)
{
    lfs_ssize_t blocks;
    blocks = lfs_fs_size(&s_lfs);
    if (blocks < 0) {
        s_record_fault(blocks);
        return blocks;
    }
    if ((uint32_t)blocks > FILE_STORE_BLOCK_COUNT) {
        s_record_fault(LFS_ERR_CORRUPT);
        return LFS_ERR_CORRUPT;
    }
    s_snapshot.used_blocks = (uint32_t)blocks;
    return 0;
}

static int32_t s_close_reader(void)
{
    int32_t result = 0;
    if (s_io_result != W25Q128_OK) {
        s_record_fault(LFS_ERR_IO);
        result = LFS_ERR_IO;
    }
    if (s_reader_open != 0U) {
        if (s_snapshot.state != FILE_STORE_ERROR && s_io_result == W25Q128_OK) {
            result = lfs_file_close(&s_lfs, &s_reader);
        }
        s_reader_open = 0U;
    }
    return result;
}

static int32_t s_abort_upload(void)
{
    int32_t result = 0;
    int32_t cleanup;
    if (s_io_result != W25Q128_OK) { s_record_fault(LFS_ERR_IO); }
    if (s_snapshot.state == FILE_STORE_ERROR) {
        /* Failed IO can leave littlefs program caches dirty. Do not sync or
         * remove anything through that mount; reinit/format rebuilds it using
         * static buffers and mount cleanup removes any retained temporary. */
        result = s_snapshot.result;
    } else {
        if (s_writer_open != 0U) {
            result = lfs_file_close(&s_lfs, &s_writer);
        }
        if (result == 0) { result = s_close_reader(); }
        if (result == 0 && s_snapshot.upload_active != 0U) {
            cleanup = lfs_remove(&s_lfs, FILE_STORE_TEMP_PATH);
            if (cleanup == LFS_ERR_NOENT) { cleanup = 0; }
            result = cleanup;
        }
        s_record_fault(result);
    }
    /* A failed cleanup can leave only the reserved temporary file. The old
     * destination was never removed; mount retries cleanup before publishing. */
    s_snapshot.upload_active = 0U;
    s_snapshot.busy = 0U;
    s_writer_open = 0U;
    s_reader_open = 0U;
    s_committing = 0U;
    if (s_snapshot.state == FILE_STORE_READY) {
        /* Allocation statistics are diagnostic. They cannot undo a completed
         * cancellation or turn a successfully removed temporary file into a
         * failed cancellation response. Refresh records any IO fault itself. */
        (void)s_refresh_usage();
    }
    return result;
}

static int32_t s_ready_mount(void)
{
    int32_t result;
    result = lfs_remove(&s_lfs, FILE_STORE_TEMP_PATH);
    if (result == LFS_ERR_NOENT) { result = 0; }
    if (result == 0) { result = s_refresh_usage(); }
    if (result == 0) {
        s_snapshot.state = FILE_STORE_READY;
        s_snapshot.result = 0;
        s_snapshot.flash_result = W25Q128_OK;
    } else {
        s_snapshot.state = FILE_STORE_ERROR;
        s_snapshot.result = result;
        s_snapshot.flash_result = s_io_result;
    }
    return result;
}

void file_store_init(const w25q128_ctx_t *p_context,
                     void (*mark_dirty)(uint32_t address, uint32_t length))
{
    memset(&s_lfs, 0, sizeof(s_lfs));
    memset(&s_lfs_config, 0, sizeof(s_lfs_config));
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(&s_writer, 0, sizeof(s_writer));
    memset(&s_reader, 0, sizeof(s_reader));
    s_context = p_context;
    s_mark_dirty = mark_dirty;
    s_blank_offset = 0U;
    s_mounted = 0U;
    s_writer_open = 0U;
    s_reader_open = 0U;
    s_committing = 0U;
    s_completion_ready = 0U;
    s_io_result = W25Q128_OK;
    s_snapshot.total_blocks = FILE_STORE_BLOCK_COUNT;
    if (p_context == NULL) {
        s_snapshot.state = FILE_STORE_UNAVAILABLE;
        s_snapshot.result = FILE_STORE_ERROR_NOT_READY;
        return;
    }
    s_lfs_config.read = s_bd_read;
    s_lfs_config.prog = s_bd_program;
    s_lfs_config.erase = s_bd_erase;
    s_lfs_config.sync = s_bd_sync;
    s_lfs_config.read_size = FILE_STORE_READ_SIZE;
    s_lfs_config.prog_size = FILE_STORE_CACHE_SIZE;
    s_lfs_config.block_size = FILE_STORE_BLOCK_SIZE;
    s_lfs_config.block_count = FILE_STORE_BLOCK_COUNT;
    s_lfs_config.block_cycles = FILE_STORE_BLOCK_CYCLES;
    s_lfs_config.cache_size = FILE_STORE_CACHE_SIZE;
    s_lfs_config.lookahead_size = FILE_STORE_LOOKAHEAD_SIZE;
    s_lfs_config.read_buffer = s_read_cache;
    s_lfs_config.prog_buffer = s_program_cache;
    s_lfs_config.lookahead_buffer = s_lookahead;
    s_lfs_config.name_max = FILE_STORE_NAME_MAX;
    s_lfs_config.file_max = FILE_STORE_SIZE_BYTES;
    s_snapshot.state = FILE_STORE_INITIALIZING;
    s_mount_result = lfs_mount(&s_lfs, &s_lfs_config);
    if (s_mount_result == 0) {
        s_mounted = 1U;
        (void)s_ready_mount();
    } else if (s_mount_result != LFS_ERR_CORRUPT) {
        s_snapshot.state = FILE_STORE_ERROR;
        s_snapshot.result = s_mount_result;
        s_snapshot.flash_result = s_io_result;
    }
}

static void s_blank_step(void)
{
    uint32_t i;
    w25q128_status_t result;
    result = w25q128_read(s_context, FILE_STORE_BASE + s_blank_offset,
                          s_data, sizeof(s_data));
    if (result != W25Q128_OK) {
        s_snapshot.state = FILE_STORE_ERROR;
        s_snapshot.result = LFS_ERR_IO;
        s_snapshot.flash_result = result;
        return;
    }
    for (i = 0U; i < sizeof(s_data); i++) {
        if (s_data[i] != FILE_STORE_ERASED_BYTE) {
            s_snapshot.state = FILE_STORE_ERROR;
            s_snapshot.result = s_mount_result;
            s_snapshot.flash_result = W25Q128_OK;
            return;
        }
    }
    s_blank_offset += sizeof(s_data);
    if (s_blank_offset == FILE_STORE_SIZE_BYTES) {
        s_snapshot.state = FILE_STORE_UNFORMATTED;
        s_snapshot.result = 0;
        s_snapshot.flash_result = W25Q128_OK;
    }
}

static void s_complete_commit(int32_t result)
{
    int32_t cleanup;
    s_record_fault(result);
    cleanup = s_close_reader();
    if (result == 0) { result = cleanup; }
    s_record_fault(result);
    if (result == 0) {
        result = lfs_rename(&s_lfs, FILE_STORE_TEMP_PATH, s_upload_path);
        if (result == 0) {
            s_snapshot.upload_active = 0U;
            s_snapshot.busy = 0U;
            s_committing = 0U;
            /* The rename has durably published the file. Report its known
             * success even if the following diagnostic traversal fails. */
            (void)s_refresh_usage();
        }
    }
    s_record_fault(result);
    if (s_snapshot.upload_active != 0U) {
        cleanup = s_abort_upload();
        if (result == 0) { result = cleanup; }
    }
    s_record_fault(result);
    s_completion.result = result;
    s_completion.size = s_verified;
    s_completion.crc32 = s_verify_crc;
    s_completion_ready = 1U;
}

void file_store_step(void)
{
    lfs_ssize_t count;
    uint32_t wanted;
    if (s_snapshot.state == FILE_STORE_INITIALIZING && s_mounted == 0U) {
        s_blank_step();
        return;
    }
    if (s_committing == 0U) { return; }
    wanted = s_upload_size - s_verified;
    if (wanted > sizeof(s_data)) { wanted = sizeof(s_data); }
    if (wanted != 0U) {
        count = lfs_file_read(&s_lfs, &s_reader, s_data, wanted);
        if (count < 0 || (uint32_t)count != wanted) {
            s_complete_commit(count < 0 ? count : LFS_ERR_CORRUPT);
            return;
        }
        s_verify_crc = file_store_crc32(s_verify_crc, s_data, wanted);
        s_verified += wanted;
    }
    if (s_verified == s_upload_size) {
        s_complete_commit(s_verify_crc == s_upload_crc ? 0 : FILE_STORE_ERROR_CRC);
    }
}

uint8_t file_store_poll(file_store_response_t *p_response)
{
    if (p_response == NULL || s_completion_ready == 0U) { return 0U; }
    *p_response = s_completion;
    s_completion_ready = 0U;
    return 1U;
}

void file_store_get_snapshot(file_store_snapshot_t *p_snapshot)
{
    if (p_snapshot != NULL) { *p_snapshot = s_snapshot; }
}

static int32_t s_list(const file_store_request_t *p_request,
                       file_store_response_t *p_response)
{
    lfs_dir_t directory;
    struct lfs_info info;
    lfs_soff_t offset;
    int32_t result;
    int32_t closed;
    result = lfs_dir_open(&s_lfs, &directory, p_request->path);
    if (result != 0) { return result; }
    result = lfs_dir_seek(&s_lfs, &directory, p_request->offset);
    while (result == 0) {
        result = lfs_dir_read(&s_lfs, &directory, &info);
        if (result <= 0) { break; }
        result = 0;
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0 ||
            strcmp(info.name, FILE_STORE_TEMP_NAME) == 0) { continue; }
        offset = lfs_dir_tell(&s_lfs, &directory);
        if (offset < 0) { result = offset; break; }
        p_response->offset = (uint32_t)offset;
        p_response->type = info.type;
        p_response->size = info.size;
        p_response->length = (uint16_t)strlen(info.name);
        memcpy(p_response->data, info.name, p_response->length);
        break;
    }
    closed = lfs_dir_close(&s_lfs, &directory);
    return result != 0 ? result : closed;
}

static int32_t s_read(const file_store_request_t *p_request,
                       file_store_response_t *p_response)
{
    lfs_soff_t position;
    lfs_soff_t size;
    lfs_ssize_t count;
    int32_t result;
    int32_t closed;
    result = lfs_file_opencfg(&s_lfs, &s_reader, p_request->path, LFS_O_RDONLY,
                              &s_reader_config);
    if (result != 0) { return result; }
    s_reader_open = 1U;
    size = lfs_file_size(&s_lfs, &s_reader);
    position = size < 0 ? size :
        lfs_file_seek(&s_lfs, &s_reader, p_request->offset, LFS_SEEK_SET);
    if (position < 0) {
        result = position;
    } else {
        count = lfs_file_read(&s_lfs, &s_reader, p_response->data,
                              p_request->length);
        if (count < 0) { result = count; }
        else {
            p_response->length = (uint16_t)count;
            p_response->offset = p_request->offset;
            p_response->size = (uint32_t)size;
        }
    }
    closed = s_close_reader();
    return result != 0 ? result : closed;
}

static int32_t s_begin_upload(const file_store_request_t *p_request)
{
    struct lfs_info info;
    uint32_t required;
    int32_t result;
    result = lfs_stat(&s_lfs, p_request->path, &info);
    if (result == 0 && info.type != LFS_TYPE_REG) { return LFS_ERR_ISDIR; }
    if (result != 0 && result != LFS_ERR_NOENT) { return result; }
    result = s_refresh_usage();
    if (result != 0) { return result; }
    if (p_request->size > FILE_STORE_SIZE_BYTES) { return LFS_ERR_FBIG; }
    required = (p_request->size + FILE_STORE_BLOCK_SIZE - 1U) / FILE_STORE_BLOCK_SIZE;
    /* CTZ links and metadata also need space. Two blocks are a conservative
     * minimum; the running allocation check enforces the operating watermark. */
    required += 2U;
    if (s_snapshot.used_blocks >= FILE_STORE_SPACE_LIMIT ||
        required > FILE_STORE_SPACE_LIMIT - s_snapshot.used_blocks) {
        return LFS_ERR_NOSPC;
    }
    result = lfs_file_opencfg(&s_lfs, &s_writer, FILE_STORE_TEMP_PATH,
                              LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                              &s_writer_config);
    if (result != 0) { return result; }
    s_writer_open = 1U;
    s_snapshot.upload_active = 1U;
    memcpy(s_upload_path, p_request->path, strlen(p_request->path) + 1U);
    s_upload_size = p_request->size;
    s_upload_crc = p_request->crc32;
    s_written = 0U;
    s_usage_offset = 0U;
    return 0;
}

static int32_t s_write_upload(const file_store_request_t *p_request)
{
    lfs_ssize_t count;
    int32_t result = 0;
    if (s_snapshot.upload_active == 0U || s_writer_open == 0U) {
        return FILE_STORE_ERROR_NOT_READY;
    }
    if (p_request->offset != s_written) { return FILE_STORE_ERROR_SEQUENCE; }
    if (p_request->length == 0U || p_request->length > s_upload_size - s_written) {
        return FILE_STORE_ERROR_ARGUMENT;
    }
    count = lfs_file_write(&s_lfs, &s_writer, p_request->data, p_request->length);
    if (count < 0) { result = count; }
    else if ((uint32_t)count != p_request->length) { result = LFS_ERR_NOSPC; }
    else {
        s_written += (uint32_t)count;
        if (s_written - s_usage_offset >= FILE_STORE_USAGE_INTERVAL ||
            s_written == s_upload_size) {
            result = s_refresh_usage();
            s_usage_offset = s_written;
            if (result == 0 && s_snapshot.used_blocks > FILE_STORE_SPACE_LIMIT) {
                result = LFS_ERR_NOSPC;
            }
        }
    }
    if (result != 0) {
        s_record_fault(result);
        (void)s_abort_upload();
    }
    return result;
}

static int32_t s_start_commit(const file_store_request_t *p_request)
{
    int32_t result;
    if (s_snapshot.upload_active == 0U || s_writer_open == 0U) {
        return FILE_STORE_ERROR_NOT_READY;
    }
    if (s_written != s_upload_size) { return FILE_STORE_ERROR_SEQUENCE; }
    result = lfs_file_sync(&s_lfs, &s_writer);
    if (result == 0 && s_io_result != W25Q128_OK) { result = LFS_ERR_IO; }
    s_record_fault(result);
    if (s_snapshot.state != FILE_STORE_ERROR && s_io_result == W25Q128_OK) {
        if (result == 0) { result = lfs_file_close(&s_lfs, &s_writer); }
        else { (void)lfs_file_close(&s_lfs, &s_writer); }
    }
    s_writer_open = 0U;
    if (result == 0 && s_io_result != W25Q128_OK) { result = LFS_ERR_IO; }
    s_record_fault(result);
    if (result == 0) {
        result = lfs_file_opencfg(&s_lfs, &s_reader, FILE_STORE_TEMP_PATH,
                                  LFS_O_RDONLY, &s_reader_config);
        if (result == 0) { s_reader_open = 1U; }
    }
    if (result == 0 && s_io_result != W25Q128_OK) { result = LFS_ERR_IO; }
    if (result != 0) {
        s_record_fault(result);
        (void)s_abort_upload();
        return result;
    }
    memset(&s_completion, 0, sizeof(s_completion));
    s_completion.token = p_request->token;
    s_completion.operation = p_request->operation;
    s_verified = 0U;
    s_verify_crc = 0U;
    s_committing = 1U;
    s_snapshot.busy = 1U;
    return 0;
}

static int32_t s_format(void)
{
    int32_t result;
    if (s_mounted != 0U) { (void)lfs_unmount(&s_lfs); s_mounted = 0U; }
    result = lfs_format(&s_lfs, &s_lfs_config);
    if (result == 0) { result = lfs_mount(&s_lfs, &s_lfs_config); }
    if (result == 0) {
        s_mounted = 1U;
        result = s_ready_mount();
    } else {
        s_snapshot.state = FILE_STORE_ERROR;
        s_snapshot.result = result;
        s_snapshot.flash_result = s_io_result;
    }
    return result;
}

uint8_t file_store_execute(const file_store_request_t *p_request,
                           file_store_response_t *p_response)
{
    struct lfs_info info;
    int32_t result = FILE_STORE_ERROR_ARGUMENT;
    uint8_t mutating;
    if (p_response == NULL) { return 0U; }
    memset(p_response, 0, sizeof(*p_response));
    if (p_request == NULL) { p_response->result = result; return 0U; }
    p_response->token = p_request->token;
    p_response->operation = p_request->operation;
    if (p_request->token == 0U || p_request->length > FILE_STORE_DATA_MAX) {
        p_response->result = result; return 0U;
    }
    if (s_committing != 0U || s_completion_ready != 0U) {
        p_response->result = FILE_STORE_ERROR_BUSY; return 0U;
    }
    if (p_request->operation == FILE_STORE_OP_STATUS) {
        p_response->result = s_snapshot.result;
        p_response->type = (uint8_t)s_snapshot.state;
        p_response->size = s_snapshot.used_blocks;
        p_response->offset = s_snapshot.total_blocks;
        return 0U;
    }
    /* Keep one candidate across deferred commit steps and their cleanup, but
     * never reuse a previous transaction's device error for a new fault. */
    s_io_result = W25Q128_OK;
    if (p_request->operation == FILE_STORE_OP_FORMAT) {
        if (p_request->confirm == 0U) { result = FILE_STORE_ERROR_CONFIRM; }
        else if (s_snapshot.upload_active != 0U) { result = FILE_STORE_ERROR_BUSY; }
        else if (s_context == NULL || s_snapshot.state == FILE_STORE_INITIALIZING) {
            result = FILE_STORE_ERROR_NOT_READY;
        } else { result = s_format(); }
        p_response->result = result;
        return 0U;
    }
    /* Explicit cancellation also works after an unrelated IO fault, so an
     * abandoned upload cannot permanently prevent confirmed maintenance. */
    if (p_request->operation == FILE_STORE_OP_UPLOAD_CANCEL && s_mounted != 0U) {
        p_response->result = s_abort_upload();
        return 0U;
    }
    if (s_snapshot.state != FILE_STORE_READY) {
        p_response->result = FILE_STORE_ERROR_NOT_READY; return 0U;
    }
    if (p_request->operation == FILE_STORE_OP_UPLOAD_WRITE) {
        result = s_write_upload(p_request);
        p_response->offset = s_written;
    } else if (p_request->operation == FILE_STORE_OP_UPLOAD_COMMIT) {
        result = s_start_commit(p_request);
        if (result == 0) { return 1U; }
    } else {
        if (file_store_path_valid(p_request->path) == 0U) {
            p_response->result = FILE_STORE_ERROR_ARGUMENT; return 0U;
        }
        mutating = (uint8_t)(p_request->operation == FILE_STORE_OP_MKDIR ||
            p_request->operation == FILE_STORE_OP_REMOVE ||
            p_request->operation == FILE_STORE_OP_RENAME ||
            p_request->operation == FILE_STORE_OP_UPLOAD_BEGIN);
        if (mutating != 0U && s_path_mutable(p_request->path) == 0U) {
            p_response->result = FILE_STORE_ERROR_ARGUMENT; return 0U;
        }
        if (mutating != 0U && s_snapshot.upload_active != 0U) {
            p_response->result = FILE_STORE_ERROR_BUSY; return 0U;
        }
        switch (p_request->operation) {
        case FILE_STORE_OP_LIST: result = s_list(p_request, p_response); break;
        case FILE_STORE_OP_READ: result = s_read(p_request, p_response); break;
        case FILE_STORE_OP_STAT:
            result = lfs_stat(&s_lfs, p_request->path, &info);
            if (result == 0) { p_response->size = info.size; p_response->type = info.type; }
            break;
        case FILE_STORE_OP_MKDIR:
            result = s_refresh_usage();
            if (result == 0) {
                result = s_snapshot.used_blocks + 2U > FILE_STORE_SPACE_LIMIT ?
                    LFS_ERR_NOSPC : lfs_mkdir(&s_lfs, p_request->path);
            }
            break;
        case FILE_STORE_OP_REMOVE: result = lfs_remove(&s_lfs, p_request->path); break;
        case FILE_STORE_OP_RENAME:
            if (s_path_mutable(p_request->path2) != 0U) {
                result = lfs_rename(&s_lfs, p_request->path, p_request->path2);
            }
            break;
        case FILE_STORE_OP_UPLOAD_BEGIN: result = s_begin_upload(p_request); break;
        default: break;
        }
        if (result == 0 && mutating != 0U) {
            /* Preserve the operation result after its commit point. A failed
             * statistics refresh is exposed independently in the snapshot. */
            (void)s_refresh_usage();
        }
    }
    s_record_fault(result);
    p_response->result = result;
    return 0U;
}
