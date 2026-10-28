/* Binary-pipe test peer for Python CLI <-> real C protocol <-> real littlefs.
 * No STM32, UART or target Flash is operated. Transport records are test-only
 * u32le length + bytes; inside them the unchanged DCFS frames are used. */
#include "file_store.h"
#include "file_transfer.h"
#include "storage_files.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define PEER_PARTITION_END (FILE_STORE_BASE + FILE_STORE_BLOCK_COUNT * FILE_STORE_BLOCK_SIZE)
#define PEER_STEPS 60000U
#define PEER_OUTPUT_MAX 65536U

static uint8_t s_flash[W25Q128_TOTAL_SIZE];
static w25q128_ctx_t s_context;
static file_store_request_t s_request;
static file_store_response_t s_response;
static uint8_t s_queued;
static uint8_t s_running;
static uint8_t s_ready;
static uint32_t s_now;
static uint32_t s_dirty_address;
static uint32_t s_dirty_length;
static uint8_t s_output[PEER_OUTPUT_MAX];
static uint32_t s_output_length;

static void s_check_range(uint32_t address, uint32_t length)
{
    assert(address >= FILE_STORE_BASE && address <= PEER_PARTITION_END);
    assert(length <= PEER_PARTITION_END - address);
}

static void s_dirty(uint32_t address, uint32_t length)
{
    s_check_range(address, length);
    s_dirty_address = address;
    s_dirty_length = length;
}

w25q128_status_t w25q128_read(const w25q128_ctx_t *p_context, uint32_t address,
                              uint8_t *p_data, uint32_t length)
{
    assert(p_context == &s_context);
    s_check_range(address, length);
    memcpy(p_data, s_flash + address, length);
    return W25Q128_OK;
}

w25q128_status_t w25q128_program(const w25q128_ctx_t *p_context, uint32_t address,
                                 const uint8_t *p_data, uint32_t length)
{
    uint32_t i;
    assert(p_context == &s_context);
    s_check_range(address, length);
    assert(address % W25Q128_PAGE_SIZE == 0U);
    assert(length % W25Q128_PAGE_SIZE == 0U);
    assert(s_dirty_address == address && s_dirty_length == length);
    for (i = 0U; i < length; i++) {
        assert((s_flash[address + i] & p_data[i]) == p_data[i]);
        s_flash[address + i] &= p_data[i];
    }
    return W25Q128_OK;
}

w25q128_status_t w25q128_erase_sector(const w25q128_ctx_t *p_context,
                                      uint32_t address)
{
    assert(p_context == &s_context);
    s_check_range(address, W25Q128_SECTOR_SIZE);
    assert(address % W25Q128_SECTOR_SIZE == 0U);
    assert(s_dirty_address == address && s_dirty_length == W25Q128_SECTOR_SIZE);
    memset(s_flash + address, 0xFF, W25Q128_SECTOR_SIZE);
    return W25Q128_OK;
}

int32_t storage_file_request(const file_store_request_t *p_request)
{
    if (s_queued != 0U || s_running != 0U || s_ready != 0U) {
        return FILE_STORE_ERROR_BUSY;
    }
    s_request = *p_request;
    s_queued = 1U;
    return 0;
}

uint8_t storage_file_take_result(uint32_t token, file_store_response_t *p_response)
{
    if (s_ready == 0U || s_response.token != token) { return 0U; }
    *p_response = s_response;
    s_ready = 0U;
    return 1U;
}

void storage_get_files_snapshot(file_store_snapshot_t *p_snapshot)
{
    file_store_get_snapshot(p_snapshot);
}

void storage_set_scan_paused(uint8_t paused) { (void)paused; }

static uint8_t s_send(const uint8_t *p_bytes, uint16_t length)
{
    assert(length <= sizeof(s_output) - s_output_length);
    memcpy(s_output + s_output_length, p_bytes, length);
    s_output_length += length;
    return 1U;
}

static void s_pump(void)
{
    uint32_t step;
    for (step = 0U; step < PEER_STEPS; step++) {
        if (s_queued != 0U) {
            s_queued = 0U;
            s_running = file_store_execute(&s_request, &s_response);
            if (s_running == 0U) { s_ready = 1U; }
        }
        file_store_step();
        if (s_running != 0U && file_store_poll(&s_response) != 0U) {
            s_running = 0U;
            s_ready = 1U;
        }
        file_transfer_poll(++s_now);
        if (file_transfer_busy() == 0U && s_running == 0U && s_queued == 0U) {
            return;
        }
    }
    assert(!"peer operation failed to complete");
}

static uint32_t s_read32(const uint8_t *p_bytes)
{
    return (uint32_t)p_bytes[0] | (uint32_t)p_bytes[1] << 8U |
        (uint32_t)p_bytes[2] << 16U | (uint32_t)p_bytes[3] << 24U;
}

int main(void)
{
    uint8_t header[4];
    uint8_t frame[FILE_TRANSFER_FRAME_MAX];
    uint32_t length;
    uint32_t i;
    file_store_snapshot_t snapshot;
#ifdef _WIN32
    assert(_setmode(_fileno(stdin), _O_BINARY) != -1);
    assert(_setmode(_fileno(stdout), _O_BINARY) != -1);
#endif
    memset(s_flash, 0xA5, sizeof(s_flash));
    memset(s_flash + FILE_STORE_BASE, 0xFF,
           FILE_STORE_BLOCK_COUNT * FILE_STORE_BLOCK_SIZE);
    file_store_init(&s_context, s_dirty);
    for (i = 0U; i < PEER_STEPS; i++) {
        file_store_get_snapshot(&snapshot);
        if (snapshot.state != FILE_STORE_INITIALIZING) { break; }
        file_store_step();
    }
    assert(snapshot.state == FILE_STORE_UNFORMATTED);
    file_transfer_init(s_send);
    assert(file_transfer_console("FS TRANSFER", 0U) != 0U);
    s_output_length = 0U;
    while (fread(header, 1U, sizeof(header), stdin) == sizeof(header)) {
        length = s_read32(header);
        assert(length <= sizeof(frame));
        assert(fread(frame, 1U, length, stdin) == length);
        s_output_length = 0U;
        /* A complete record represents a new serial transaction. The gap
         * also exercises the C parser's truncated-frame timeout recovery. */
        s_now += 1100U;
        for (i = 0U; i < length; i++) { file_transfer_feed(frame[i], s_now); }
        s_pump();
        for (i = 0U; i < sizeof(header); i++) {
            header[i] = (uint8_t)(s_output_length >> (8U * i));
        }
        assert(fwrite(header, 1U, sizeof(header), stdout) == sizeof(header));
        assert(fwrite(s_output, 1U, s_output_length, stdout) == s_output_length);
        assert(fflush(stdout) == 0);
    }
    return 0;
}
