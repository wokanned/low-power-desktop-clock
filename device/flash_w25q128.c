#include "flash_w25q128.h"
#include <stddef.h>
#include <string.h>

#define W25Q128_CMD_READ            0x03U
#define W25Q128_CMD_READ_ID         0x9FU
#define W25Q128_CMD_READ_STATUS1    0x05U
#define W25Q128_CMD_READ_STATUS2    0x35U
#define W25Q128_CMD_READ_STATUS3    0x15U
#define W25Q128_CMD_RELEASE_POWER_DOWN 0xABU
#define W25Q128_CMD_WRITE_ENABLE    0x06U
#define W25Q128_CMD_PAGE_PROGRAM    0x02U
#define W25Q128_CMD_SECTOR_ERASE    0x20U
#define W25Q128_CMD_BLOCK_ERASE32   0x52U
#define W25Q128_CMD_BLOCK_ERASE64   0xD8U
#define W25Q128_CMD_CHIP_ERASE      0xC7U
#define W25Q128_STATUS_WIP          0x01U
#define W25Q128_STATUS_WEL          0x02U
#define W25Q128_STATUS_BP           0x1CU
#define W25Q128_STATUS_CMP          0x40U
#define W25Q128_STATUS_SUS          0x80U
#define W25Q128_STATUS_WPS          0x04U
#define W25Q128_ERASED_BYTE         0xFFU
#define W25Q128_COMMAND_SIZE        4U
#define W25Q128_POWER_ON_DELAY_MS   5U /* Rev E tPUW max, printed p61. */
#define W25Q128_WAKE_DELAY_MS       1U
#define W25Q128_WEL_TIMEOUT_MS      10U
#define W25Q128_PROGRAM_TIMEOUT_MS  100U
#define W25Q128_SECTOR_TIMEOUT_MS   500U
#define W25Q128_BLOCK32_TIMEOUT_MS  2000U
#define W25Q128_BLOCK64_TIMEOUT_MS  3000U
/* Rev E, printed p65: tCE max 200 s; include scheduling margin. */
#define W25Q128_CHIP_TIMEOUT_MS     205000U
#define W25Q128_POLL_DELAY_MS       1U

static w25q128_status_t s_read(const w25q128_ctx_t *p_ctx, uint32_t address,
                               uint8_t *p_data, uint32_t length);

static int s_valid_ctx(const w25q128_ctx_t *p_ctx)
{
    return p_ctx != NULL && p_ctx->transaction != NULL &&
           p_ctx->delay_ms != NULL && p_ctx->tick_ms != NULL &&
           ((p_ctx->lock == NULL) == (p_ctx->unlock == NULL));
}

static w25q128_status_t s_lock(const w25q128_ctx_t *p_ctx)
{
    if (p_ctx->lock != NULL && p_ctx->lock(p_ctx->p_handle) != 0) {
        return W25Q128_ERROR_BUSY;
    }
    return W25Q128_OK;
}

static void s_unlock(const w25q128_ctx_t *p_ctx)
{
    if (p_ctx->unlock != NULL) {
        p_ctx->unlock(p_ctx->p_handle);
    }
}

static w25q128_status_t s_transaction(const w25q128_ctx_t *p_ctx,
                                      const uint8_t *p_tx, uint32_t tx_len,
                                      uint8_t *p_rx, uint32_t rx_len)
{
    return p_ctx->transaction(p_ctx->p_handle, p_tx, tx_len, p_rx, rx_len) == 0 ?
        W25Q128_OK : W25Q128_ERROR_IO;
}

static w25q128_status_t s_read_status(const w25q128_ctx_t *p_ctx, uint8_t *p_status)
{
    uint8_t command = W25Q128_CMD_READ_STATUS1;
    return s_transaction(p_ctx, &command, 1U, p_status, 1U);
}

static w25q128_status_t s_read_registers(const w25q128_ctx_t *p_ctx,
                                         w25q128_registers_t *p_registers)
{
    uint8_t command;
    w25q128_status_t result;
    result = s_read_status(p_ctx, &p_registers->sr1);
    command = W25Q128_CMD_READ_STATUS2;
    if (result == W25Q128_OK) {
        result = s_transaction(p_ctx, &command, 1U, &p_registers->sr2, 1U);
    }
    command = W25Q128_CMD_READ_STATUS3;
    if (result == W25Q128_OK) {
        result = s_transaction(p_ctx, &command, 1U, &p_registers->sr3, 1U);
    }
    return result;
}

static w25q128_status_t s_wait_ready(const w25q128_ctx_t *p_ctx, uint32_t timeout_ms)
{
    uint8_t command = W25Q128_CMD_READ_STATUS2;
    uint8_t status2;
    uint32_t start;
    uint32_t elapsed;
    uint8_t status;
    w25q128_status_t result;

    start = p_ctx->tick_ms(p_ctx->p_handle);
    for (;;) {
        result = s_read_status(p_ctx, &status);
        if (result != W25Q128_OK) {
            return result;
        }
        if ((status & W25Q128_STATUS_WIP) == 0U) {
            result = s_transaction(p_ctx, &command, 1U, &status2, 1U);
            if (result != W25Q128_OK) { return result; }
            return (status2 & W25Q128_STATUS_SUS) != 0U ?
                W25Q128_ERROR_BUSY : W25Q128_OK;
        }
        elapsed = p_ctx->tick_ms(p_ctx->p_handle) - start;
        if (elapsed >= timeout_ms) {
            return W25Q128_ERROR_TIMEOUT;
        }
        p_ctx->delay_ms(p_ctx->p_handle, W25Q128_POLL_DELAY_MS);
    }
}

static w25q128_status_t s_write_enable(const w25q128_ctx_t *p_ctx)
{
    w25q128_registers_t registers;
    w25q128_status_t result;
    uint8_t command = W25Q128_CMD_WRITE_ENABLE;
    uint8_t status;
    uint32_t start;

    result = s_wait_ready(p_ctx, W25Q128_CHIP_TIMEOUT_MS);
    if (result != W25Q128_OK) { return result; }
    result = s_read_registers(p_ctx, &registers);
    if (result != W25Q128_OK) { return result; }
    /* Avoid changing protection policy or misreporting an ignored write. */
    if ((registers.sr1 & W25Q128_STATUS_BP) != 0U ||
        (registers.sr2 & W25Q128_STATUS_CMP) != 0U ||
        (registers.sr3 & W25Q128_STATUS_WPS) != 0U) {
        return W25Q128_ERROR_PROTECTED;
    }
    if (s_transaction(p_ctx, &command, 1U, NULL, 0U) != W25Q128_OK) {
        return W25Q128_ERROR_IO;
    }
    start = p_ctx->tick_ms(p_ctx->p_handle);
    for (;;) {
        if (s_read_status(p_ctx, &status) != W25Q128_OK) {
            return W25Q128_ERROR_IO;
        }
        if ((status & W25Q128_STATUS_WEL) != 0U) {
            return W25Q128_OK;
        }
        if ((p_ctx->tick_ms(p_ctx->p_handle) - start) >= W25Q128_WEL_TIMEOUT_MS) {
            return W25Q128_ERROR_TIMEOUT;
        }
        p_ctx->delay_ms(p_ctx->p_handle, W25Q128_POLL_DELAY_MS);
    }
}

static w25q128_status_t s_check_range(uint32_t address, uint32_t length)
{
    if (address > W25Q128_TOTAL_SIZE || length > (W25Q128_TOTAL_SIZE - address)) {
        return W25Q128_ERROR_RANGE;
    }
    return W25Q128_OK;
}

static w25q128_status_t s_read_id(const w25q128_ctx_t *p_ctx, uint32_t *p_id)
{
    uint8_t command = W25Q128_CMD_READ_ID;
    uint8_t id[3];
    if (s_transaction(p_ctx, &command, 1U, id, sizeof(id)) != W25Q128_OK) {
        return W25Q128_ERROR_IO;
    }
    *p_id = ((uint32_t)id[0] << 16U) | ((uint32_t)id[1] << 8U) | id[2];
    return W25Q128_OK;
}

static w25q128_status_t s_verify_erased(const w25q128_ctx_t *p_ctx,
                                        uint32_t address, uint32_t length)
{
    uint8_t bytes[W25Q128_PAGE_SIZE];
    uint32_t chunk;
    uint32_t i;
    w25q128_status_t result;
    while (length != 0U) {
        chunk = length > sizeof(bytes) ? sizeof(bytes) : length;
        result = s_read(p_ctx, address, bytes, chunk);
        if (result != W25Q128_OK) { return result; }
        for (i = 0U; i < chunk; i++) {
            if (bytes[i] != W25Q128_ERASED_BYTE) { return W25Q128_ERROR_VERIFY; }
        }
        address += chunk;
        length -= chunk;
        /* Bound continuous CPU use during large blocking blank checks. */
        if ((address % W25Q128_SECTOR_SIZE) == 0U) {
            p_ctx->delay_ms(p_ctx->p_handle, W25Q128_POLL_DELAY_MS);
        }
    }
    return W25Q128_OK;
}

static w25q128_status_t s_erase(const w25q128_ctx_t *p_ctx, uint8_t command,
                                uint32_t address, uint32_t size, uint32_t timeout_ms)
{
    uint8_t tx[W25Q128_COMMAND_SIZE];
    w25q128_status_t result;

    if ((address % size) != 0U || s_check_range(address, size) != W25Q128_OK) {
        return W25Q128_ERROR_RANGE;
    }
    result = s_write_enable(p_ctx);
    if (result != W25Q128_OK) {
        return result;
    }
    tx[0] = command;
    tx[1] = (uint8_t)(address >> 16U);
    tx[2] = (uint8_t)(address >> 8U);
    tx[3] = (uint8_t)address;
    result = s_transaction(p_ctx, tx, sizeof(tx), NULL, 0U);
    if (result != W25Q128_OK) {
        return result;
    }
    result = s_wait_ready(p_ctx, timeout_ms);
    if (result == W25Q128_OK) { result = s_verify_erased(p_ctx, address, size); }
    return result;
}

w25q128_status_t w25q128_init(const w25q128_ctx_t *p_ctx)
{
    uint32_t id;
    uint8_t command = W25Q128_CMD_RELEASE_POWER_DOWN;
    w25q128_registers_t registers;
    w25q128_status_t result;

    if (!s_valid_ctx(p_ctx)) {
        return W25Q128_ERROR_ARGUMENT;
    }
    result = s_lock(p_ctx);
    if (result != W25Q128_OK) {
        return result;
    }
    p_ctx->delay_ms(p_ctx->p_handle, W25Q128_POWER_ON_DELAY_MS);
    result = s_transaction(p_ctx, &command, 1U, NULL, 0U);
    if (result == W25Q128_OK) {
        p_ctx->delay_ms(p_ctx->p_handle, W25Q128_WAKE_DELAY_MS);
        result = s_wait_ready(p_ctx, W25Q128_CHIP_TIMEOUT_MS);
    }
    if (result == W25Q128_OK) { result = s_read_id(p_ctx, &id); }
    if (result == W25Q128_OK && id != W25Q128_JEDEC_ID) {
        result = W25Q128_ERROR_ID;
    }
    if (result == W25Q128_OK) {
        result = s_read_registers(p_ctx, &registers);
    }
    s_unlock(p_ctx);
    return result;
}

w25q128_status_t w25q128_read_id(const w25q128_ctx_t *p_ctx, uint32_t *p_id)
{
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx) || p_id == NULL) {
        return W25Q128_ERROR_ARGUMENT;
    }
    result = s_lock(p_ctx);
    if (result == W25Q128_OK) {
        result = s_read_id(p_ctx, p_id);
        s_unlock(p_ctx);
    }
    return result;
}

w25q128_status_t w25q128_read_status(const w25q128_ctx_t *p_ctx, uint8_t *p_status)
{
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx) || p_status == NULL) {
        return W25Q128_ERROR_ARGUMENT;
    }
    result = s_lock(p_ctx);
    if (result == W25Q128_OK) {
        result = s_read_status(p_ctx, p_status);
        s_unlock(p_ctx);
    }
    return result;
}

w25q128_status_t w25q128_read_registers(const w25q128_ctx_t *p_ctx,
                                       w25q128_registers_t *p_registers)
{
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx) || p_registers == NULL) { return W25Q128_ERROR_ARGUMENT; }
    result = s_lock(p_ctx);
    if (result == W25Q128_OK) {
        result = s_read_registers(p_ctx, p_registers);
        s_unlock(p_ctx);
    }
    return result;
}

static w25q128_status_t s_read(const w25q128_ctx_t *p_ctx, uint32_t address,
                               uint8_t *p_data, uint32_t length)
{
    uint8_t tx[W25Q128_COMMAND_SIZE];
    uint32_t chunk;
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx) || (length != 0U && p_data == NULL)) {
        return W25Q128_ERROR_ARGUMENT;
    }
    result = s_check_range(address, length);
    if (result != W25Q128_OK || length == 0U) {
        return result;
    }
    while (length != 0U) {
        chunk = length > W25Q128_PAGE_SIZE ? W25Q128_PAGE_SIZE : length;
        tx[0] = W25Q128_CMD_READ;
        tx[1] = (uint8_t)(address >> 16U);
        tx[2] = (uint8_t)(address >> 8U);
        tx[3] = (uint8_t)address;
        result = s_transaction(p_ctx, tx, sizeof(tx), p_data, chunk);
        if (result != W25Q128_OK) {
            break;
        }
        address += chunk;
        p_data += chunk;
        length -= chunk;
    }
    return result;
}

w25q128_status_t w25q128_read(const w25q128_ctx_t *p_ctx, uint32_t address,
                              uint8_t *p_data, uint32_t length)
{
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx) || (length != 0U && p_data == NULL)) {
        return W25Q128_ERROR_ARGUMENT;
    }
    result = s_check_range(address, length);
    if (result != W25Q128_OK || length == 0U) {
        return result;
    }
    result = s_lock(p_ctx);
    if (result == W25Q128_OK) {
        result = s_wait_ready(p_ctx, W25Q128_CHIP_TIMEOUT_MS);
        if (result == W25Q128_OK) { result = s_read(p_ctx, address, p_data, length); }
        s_unlock(p_ctx);
    }
    return result;
}

static w25q128_status_t s_program(const w25q128_ctx_t *p_ctx, uint32_t address,
                                  const uint8_t *p_data, uint32_t length)
{
    uint8_t tx[W25Q128_PAGE_SIZE + W25Q128_COMMAND_SIZE];
    uint8_t verified[W25Q128_PAGE_SIZE];
    uint32_t page_offset;
    uint32_t chunk;
    uint32_t i;
    w25q128_status_t result;
    result = s_wait_ready(p_ctx, W25Q128_CHIP_TIMEOUT_MS);
    if (result != W25Q128_OK) { return result; }
    while (length != 0U) {
        page_offset = address % W25Q128_PAGE_SIZE;
        chunk = W25Q128_PAGE_SIZE - page_offset;
        if (chunk > length) {
            chunk = length;
        }
        result = s_read(p_ctx, address, verified, chunk);
        if (result != W25Q128_OK) { return result; }
        for (i = 0U; i < chunk; i++) {
            if ((verified[i] & p_data[i]) != p_data[i]) {
                return W25Q128_ERROR_NEEDS_ERASE;
            }
        }
        result = s_write_enable(p_ctx);
        if (result != W25Q128_OK) {
            return result;
        }
        tx[0] = W25Q128_CMD_PAGE_PROGRAM;
        tx[1] = (uint8_t)(address >> 16U);
        tx[2] = (uint8_t)(address >> 8U);
        tx[3] = (uint8_t)address;
        (void)memcpy(&tx[W25Q128_COMMAND_SIZE], p_data, chunk);
        result = s_transaction(p_ctx, tx, chunk + W25Q128_COMMAND_SIZE, NULL, 0U);
        if (result != W25Q128_OK) {
            return result;
        }
        result = s_wait_ready(p_ctx, W25Q128_PROGRAM_TIMEOUT_MS);
        if (result != W25Q128_OK) {
            return result;
        }
        result = s_read(p_ctx, address, verified, chunk);
        if (result != W25Q128_OK) { return result; }
        if (memcmp(verified, p_data, chunk) != 0) { return W25Q128_ERROR_VERIFY; }
        address += chunk;
        p_data += chunk;
        length -= chunk;
    }
    return W25Q128_OK;
}

w25q128_status_t w25q128_program(const w25q128_ctx_t *p_ctx, uint32_t address,
                                 const uint8_t *p_data, uint32_t length)
{
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx) || (length != 0U && p_data == NULL)) {
        return W25Q128_ERROR_ARGUMENT;
    }
    result = s_check_range(address, length);
    if (result != W25Q128_OK || length == 0U) {
        return result;
    }
    result = s_lock(p_ctx);
    if (result == W25Q128_OK) {
        result = s_program(p_ctx, address, p_data, length);
        s_unlock(p_ctx);
    }
    return result;
}

w25q128_status_t w25q128_update(const w25q128_ctx_t *p_ctx, uint32_t address,
                                const uint8_t *p_data, uint32_t length,
                                uint8_t *p_scratch, uint32_t scratch_len)
{
    uint32_t sector_base;
    uint32_t sector_offset;
    uint32_t chunk;
    uintptr_t data_start;
    uintptr_t scratch_start;
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx) || (length != 0U && p_data == NULL) ||
        (length != 0U && (p_scratch == NULL || scratch_len < W25Q128_SECTOR_SIZE))) {
        return W25Q128_ERROR_ARGUMENT;
    }
    result = s_check_range(address, length);
    if (result != W25Q128_OK || length == 0U) {
        return result;
    }
    data_start = (uintptr_t)p_data;
    scratch_start = (uintptr_t)p_scratch;
    if ((data_start >= scratch_start && data_start - scratch_start < W25Q128_SECTOR_SIZE) ||
        (data_start < scratch_start && scratch_start - data_start < length)) {
        return W25Q128_ERROR_ARGUMENT;
    }
    result = s_lock(p_ctx);
    if (result != W25Q128_OK) {
        return result;
    }
    result = s_wait_ready(p_ctx, W25Q128_CHIP_TIMEOUT_MS);
    while (result == W25Q128_OK && length != 0U) {
        sector_base = address - (address % W25Q128_SECTOR_SIZE);
        sector_offset = address - sector_base;
        chunk = W25Q128_SECTOR_SIZE - sector_offset;
        if (chunk > length) {
            chunk = length;
        }
        result = s_read(p_ctx, sector_base, p_scratch, W25Q128_SECTOR_SIZE);
        if (result != W25Q128_OK) {
            break;
        }
        if (memcmp(&p_scratch[sector_offset], p_data, chunk) != 0) {
            (void)memcpy(&p_scratch[sector_offset], p_data, chunk);
            result = s_erase(p_ctx, W25Q128_CMD_SECTOR_ERASE, sector_base,
                         W25Q128_SECTOR_SIZE, W25Q128_SECTOR_TIMEOUT_MS);
            if (result == W25Q128_OK) {
                result = s_program(p_ctx, sector_base, p_scratch, W25Q128_SECTOR_SIZE);
            }
        }
        if (result != W25Q128_OK) {
            break;
        }
        address += chunk;
        p_data += chunk;
        length -= chunk;
    }
    s_unlock(p_ctx);
    return result;
}

w25q128_status_t w25q128_erase_sector(const w25q128_ctx_t *p_ctx, uint32_t address)
{
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx)) { return W25Q128_ERROR_ARGUMENT; }
    result = s_lock(p_ctx);
    if (result == W25Q128_OK) {
        result = s_erase(p_ctx, W25Q128_CMD_SECTOR_ERASE, address,
                         W25Q128_SECTOR_SIZE, W25Q128_SECTOR_TIMEOUT_MS);
        s_unlock(p_ctx);
    }
    return result;
}

w25q128_status_t w25q128_erase_block32(const w25q128_ctx_t *p_ctx, uint32_t address)
{
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx)) { return W25Q128_ERROR_ARGUMENT; }
    result = s_lock(p_ctx);
    if (result == W25Q128_OK) {
        result = s_erase(p_ctx, W25Q128_CMD_BLOCK_ERASE32, address,
                         W25Q128_BLOCK32_SIZE, W25Q128_BLOCK32_TIMEOUT_MS);
        s_unlock(p_ctx);
    }
    return result;
}

w25q128_status_t w25q128_erase_block64(const w25q128_ctx_t *p_ctx, uint32_t address)
{
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx)) { return W25Q128_ERROR_ARGUMENT; }
    result = s_lock(p_ctx);
    if (result == W25Q128_OK) {
        result = s_erase(p_ctx, W25Q128_CMD_BLOCK_ERASE64, address,
                         W25Q128_BLOCK64_SIZE, W25Q128_BLOCK64_TIMEOUT_MS);
        s_unlock(p_ctx);
    }
    return result;
}

w25q128_status_t w25q128_erase_chip(const w25q128_ctx_t *p_ctx)
{
    uint8_t command = W25Q128_CMD_CHIP_ERASE;
    w25q128_status_t result;
    if (!s_valid_ctx(p_ctx)) { return W25Q128_ERROR_ARGUMENT; }
    result = s_lock(p_ctx);
    if (result == W25Q128_OK) {
        result = s_write_enable(p_ctx);
        if (result == W25Q128_OK) {
            result = s_transaction(p_ctx, &command, 1U, NULL, 0U);
        }
        if (result == W25Q128_OK) {
            result = s_wait_ready(p_ctx, W25Q128_CHIP_TIMEOUT_MS);
        }
        if (result == W25Q128_OK) {
            result = s_verify_erased(p_ctx, 0U, W25Q128_TOTAL_SIZE);
        }
        s_unlock(p_ctx);
    }
    return result;
}
