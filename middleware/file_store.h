#ifndef FILE_STORE_H
#define FILE_STORE_H

#include "flash_w25q128.h"
#include <stdint.h>

#define FILE_STORE_BASE 0x00400000UL
#define FILE_STORE_BLOCK_SIZE 4096U
#define FILE_STORE_BLOCK_COUNT 3070U
#define FILE_STORE_RESERVED_BLOCKS 461U
#define FILE_STORE_PATH_MAX 95U
#define FILE_STORE_NAME_MAX 31U
#define FILE_STORE_DATA_MAX 256U

typedef enum {
    FILE_STORE_UNAVAILABLE = 0, FILE_STORE_INITIALIZING, FILE_STORE_UNFORMATTED,
    FILE_STORE_READY, FILE_STORE_ERROR
} file_store_state_t;

typedef enum {
    FILE_STORE_OP_STATUS = 1, FILE_STORE_OP_LIST, FILE_STORE_OP_MKDIR,
    FILE_STORE_OP_READ, FILE_STORE_OP_REMOVE, FILE_STORE_OP_RENAME,
    FILE_STORE_OP_UPLOAD_BEGIN, FILE_STORE_OP_UPLOAD_WRITE,
    FILE_STORE_OP_UPLOAD_COMMIT, FILE_STORE_OP_UPLOAD_CANCEL,
    FILE_STORE_OP_FORMAT, FILE_STORE_OP_STAT, FILE_STORE_OP_SELFTEST
} file_store_operation_t;

#define FILE_STORE_OK 0
#define FILE_STORE_ERROR_ARGUMENT (-1001)
#define FILE_STORE_ERROR_BUSY (-1002)
#define FILE_STORE_ERROR_NOT_READY (-1003)
#define FILE_STORE_ERROR_CRC (-1004)
#define FILE_STORE_ERROR_SEQUENCE (-1005)
#define FILE_STORE_ERROR_CONFIRM (-1006)

typedef struct {
    uint32_t token;
    file_store_operation_t operation;
    char path[FILE_STORE_PATH_MAX + 1U];
    char path2[FILE_STORE_PATH_MAX + 1U];
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
    uint16_t length;
    uint8_t confirm;
    uint8_t data[FILE_STORE_DATA_MAX];
} file_store_request_t;

typedef struct {
    uint32_t token;
    file_store_operation_t operation;
    int32_t result;
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
    uint16_t length;
    uint8_t type;
    uint8_t data[FILE_STORE_DATA_MAX];
} file_store_response_t;

typedef struct {
    file_store_state_t state;
    int32_t result;
    uint32_t used_blocks;
    uint32_t total_blocks;
    uint8_t upload_active;
    uint8_t busy;
    /* Cause of the current ERROR, in the W25Q error domain; OK means no
     * device failure was observed (for example, littlefs corruption).
     * The first failed IO in a transaction survives successful cleanup.
     * A successful mount/confirmed format clears the retained cause. */
    w25q128_status_t flash_result;
} file_store_snapshot_t;

/* All engine functions belong to one owner (storageTask). No RTOS or HAL.
 * mark_dirty is called BEFORE every attempted program/erase, including failures.
 * No automatic format. Successful mount may recover/remove the reserved temp.
 * Unformatted media requires explicit confirmed format.
 * execute returns 1 for a deferred operation, 0 for a completed response.
 * step advances startup checks and long commits by at most one data block;
 * individual littlefs metadata/erase calls can still take longer.
 * poll returns 1 exactly once for a completed deferred response. */
void file_store_init(const w25q128_ctx_t *p_context,
                     void (*mark_dirty)(uint32_t address, uint32_t length));
void file_store_step(void);
uint8_t file_store_execute(const file_store_request_t *p_request,
                           file_store_response_t *p_response);
uint8_t file_store_poll(file_store_response_t *p_response);
void file_store_get_snapshot(file_store_snapshot_t *p_snapshot);
uint8_t file_store_path_valid(const char *p_path);
uint32_t file_store_crc32(uint32_t crc, const uint8_t *p_data, uint32_t length);

#endif
