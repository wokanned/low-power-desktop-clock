#ifndef W25Q128_H
#define W25Q128_H

#include <stdint.h>

#define W25Q128_TOTAL_SIZE       0x01000000UL
#define W25Q128_PAGE_SIZE        256U
#define W25Q128_SECTOR_SIZE      4096U
#define W25Q128_BLOCK32_SIZE     32768U
#define W25Q128_BLOCK64_SIZE     65536U
#define W25Q128_JEDEC_ID         0xEF4018UL

typedef enum {
    W25Q128_OK = 0,
    W25Q128_ERROR_ARGUMENT = -1,
    W25Q128_ERROR_IO = -2,
    W25Q128_ERROR_ID = -3,
    W25Q128_ERROR_TIMEOUT = -4,
    W25Q128_ERROR_RANGE = -5,
    W25Q128_ERROR_BUSY = -6,
    W25Q128_ERROR_VERIFY = -7,
    W25Q128_ERROR_PROTECTED = -8,
    W25Q128_ERROR_NEEDS_ERASE = -9
} w25q128_status_t;

typedef struct {
    /* One CS-low command/data transaction; return with CS high even on error. */
    int32_t (*transaction)(void *p_handle, const uint8_t *p_tx,
                           uint32_t tx_len, uint8_t *p_rx, uint32_t rx_len);
    void (*delay_ms)(void *p_handle, uint32_t delay_ms);
    uint32_t (*tick_ms)(void *p_handle);
    int32_t (*lock)(void *p_handle);
    void (*unlock)(void *p_handle);
    void *p_handle;
} w25q128_ctx_t;

typedef struct {
    uint8_t sr1;
    uint8_t sr2;
    uint8_t sr3;
} w25q128_registers_t;

/* Task context only for the STM32 adapter. Lock covers each whole public call.
 * Both lock callbacks may be NULL only for an exclusively owned device.
 * init wakes from power-down, waits for any previous operation, checks ID/SR.
 * No reset, automatic unlock, or status/OTP register writes are performed. */
w25q128_status_t w25q128_init(const w25q128_ctx_t *p_ctx);
w25q128_status_t w25q128_read_id(const w25q128_ctx_t *p_ctx, uint32_t *p_id);
w25q128_status_t w25q128_read_status(const w25q128_ctx_t *p_ctx, uint8_t *p_status);
w25q128_status_t w25q128_read_registers(const w25q128_ctx_t *p_ctx,
                                       w25q128_registers_t *p_registers);
w25q128_status_t w25q128_read(const w25q128_ctx_t *p_ctx, uint32_t address,
                              uint8_t *p_data, uint32_t length);
/* No implicit erase. Reject 0-to-1 transitions before programming each page;
 * read back each programmed page. On failure earlier pages may be committed. */
w25q128_status_t w25q128_program(const w25q128_ctx_t *p_ctx, uint32_t address,
                                 const uint8_t *p_data, uint32_t length);
/* Preserve untouched bytes using read/erase/program; skip identical sectors.
 * p_scratch must provide 4 KiB and must not overlap p_data. No power-fail atomicity.
 * A failed update may leave a partially rewritten sector; do not blindly retry. */
w25q128_status_t w25q128_update(const w25q128_ctx_t *p_ctx, uint32_t address,
                                const uint8_t *p_data, uint32_t length,
                                uint8_t *p_scratch, uint32_t scratch_len);
/* Erase addresses must be aligned. All erased bytes are checked for 0xFF.
 * Mutations conservatively reject any BP/CMP/WPS protection configuration. */
w25q128_status_t w25q128_erase_sector(const w25q128_ctx_t *p_ctx, uint32_t address);
w25q128_status_t w25q128_erase_block32(const w25q128_ctx_t *p_ctx, uint32_t address);
w25q128_status_t w25q128_erase_block64(const w25q128_ctx_t *p_ctx, uint32_t address);
w25q128_status_t w25q128_erase_chip(const w25q128_ctx_t *p_ctx);

#endif
