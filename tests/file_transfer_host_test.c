#include "file_transfer.h"
#include "storage_files.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t s_output[4096];
static uint16_t s_output_length;
static uint8_t s_frame[FILE_TRANSFER_FRAME_MAX];
static uint16_t s_frame_length;
static file_store_request_t s_accepted;
static file_store_response_t s_result;
static uint32_t s_calls;
static uint32_t s_attempts;
static int32_t s_submit_result;
static w25q128_status_t s_flash_result;
static uint8_t s_done;
static uint32_t s_now;

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8U |
        (uint32_t)p[2] << 16U | (uint32_t)p[3] << 24U;
}
static void write32(uint8_t *p, uint32_t value)
{
    uint8_t i;
    for (i = 0U; i < 4U; i++) { p[i] = (uint8_t)(value >> (8U * i)); }
}
static uint32_t crc32(const uint8_t *p, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    uint8_t bit;
    for (i = 0U; i < length; i++) {
        crc ^= p[i];
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}
static uint8_t send_bytes(const uint8_t *p, uint16_t length)
{
    assert(s_output_length + length < sizeof(s_output));
    memcpy(s_output + s_output_length, p, length);
    s_output_length = (uint16_t)(s_output_length + length);
    return 1U;
}
static void clear_output(void)
{
    memset(s_output, 0, sizeof(s_output));
    s_output_length = 0U;
}
int32_t storage_file_request(const file_store_request_t *p_request)
{
    assert(p_request != NULL && p_request->token != 0U);
    s_attempts++;
    if (s_submit_result != FILE_STORE_OK) { return s_submit_result; }
    s_accepted = *p_request;
    s_calls++;
    s_done = 0U;
    return FILE_STORE_OK;
}
uint8_t storage_file_take_result(uint32_t token, file_store_response_t *p_response)
{
    assert(token == s_accepted.token && p_response != NULL);
    if (s_done == 0U) { return 0U; }
    *p_response = s_result;
    p_response->token = token;
    p_response->operation = s_accepted.operation;
    s_done = 0U;
    return 1U;
}
void storage_get_files_snapshot(file_store_snapshot_t *p_snapshot)
{
    memset(p_snapshot, 0, sizeof(*p_snapshot));
    p_snapshot->state = FILE_STORE_READY;
    p_snapshot->total_blocks = FILE_STORE_BLOCK_COUNT;
    p_snapshot->used_blocks = 2U;
    p_snapshot->flash_result = s_flash_result;
}
static void make_frame(uint8_t op, uint32_t session, uint32_t seq,
                        uint32_t offset, const uint8_t *p_payload, uint16_t length)
{
    memset(s_frame, 0, sizeof(s_frame));
    memcpy(s_frame, "DCFS", 4U);
    s_frame[4] = 1U;
    s_frame[5] = op;
    write32(s_frame + 8U, session);
    write32(s_frame + 12U, seq);
    write32(s_frame + 16U, offset);
    s_frame[32] = (uint8_t)length;
    s_frame[33] = (uint8_t)(length >> 8U);
    if (length != 0U) { memcpy(s_frame + 36U, p_payload, length); }
    write32(s_frame + 36U + length, crc32(s_frame + 4U, 32U + length));
    s_frame_length = (uint16_t)(40U + length);
}
static void feed_frame(void)
{
    uint16_t i;
    for (i = 0U; i < s_frame_length; i++) { file_transfer_feed(s_frame[i], s_now); }
}
static void complete(int32_t result)
{
    memset(&s_result, 0, sizeof(s_result));
    s_result.result = result;
    s_done = 1U;
    file_transfer_poll(s_now);
}
static void start_session(void)
{
    file_transfer_init(send_bytes);
    clear_output();
    assert(file_transfer_console("FS TRANSFER", s_now));
    assert(file_transfer_active());
    assert(strcmp((const char *)s_output, "OK FS TRANSFER\r\n") == 0);
    clear_output();
    make_frame(0U, 0x12345678U, 0U, 0U, NULL, 0U);
    feed_frame();
    assert(s_output_length == 40U && s_output[6] == 1U);
    assert(read32(s_output + 8U) == 0x12345678U);
}

static void test_rejected_request(void)
{
    uint8_t cached[FILE_TRANSFER_FRAME_MAX];
    uint32_t calls;
    uint32_t attempts;
    s_submit_result = FILE_STORE_OK;
    start_session();
    clear_output();
    calls = s_calls;
    s_submit_result = FILE_STORE_ERROR_NOT_READY;
    make_frame(FILE_STORE_OP_MKDIR, 0x12345678U, 1U, 0U,
               (const uint8_t *)"/new", 4U);
    feed_frame();
    assert(s_calls == calls && !file_transfer_busy());
    assert(s_output_length == 40U && s_output[6] == FILE_TRANSFER_FLAG_FINAL);
    assert((int32_t)read32(s_output + 28U) == FILE_STORE_ERROR_NOT_READY);
    memcpy(cached, s_output, s_output_length);
    clear_output();
    attempts = s_attempts;
    s_submit_result = FILE_STORE_OK;
    feed_frame(); /* A deterministic rejection is also a replayable final. */
    assert(s_attempts == attempts && s_output_length == 40U);
    assert(memcmp(cached, s_output, s_output_length) == 0);
    clear_output();
    make_frame(FILE_STORE_OP_MKDIR, 0x12345678U, 2U, 0U,
               (const uint8_t *)"/next", 5U);
    feed_frame();
    assert(s_calls == calls + 1U && file_transfer_busy());
    complete(FILE_STORE_OK);
    assert(!file_transfer_busy());

    file_transfer_init(send_bytes);
    clear_output();
    s_submit_result = FILE_STORE_ERROR_NOT_READY;
    assert(file_transfer_console("FS MKDIR /new", s_now));
    assert(strcmp((const char *)s_output, "ERR FS -1003\r\n") == 0);
    s_submit_result = FILE_STORE_OK;
    clear_output();
    assert(file_transfer_console("FS LIST /", s_now));
    memset(&s_result, 0, sizeof(s_result));
    memcpy(s_result.data, "entry", 5U);
    s_result.length = 5U;
    s_result.offset = 1U;
    s_result.type = 1U;
    s_done = 1U;
    s_submit_result = FILE_STORE_ERROR_NOT_READY;
    file_transfer_poll(s_now); /* Failure to submit the next list entry is final. */
    assert(!file_transfer_busy());
    assert(strstr((const char *)s_output, "FS ENTRY 1 0 entry\r\n") != NULL);
    assert(strstr((const char *)s_output, "ERR FS -1003\r\n") != NULL);
    s_submit_result = FILE_STORE_OK;
    clear_output();
    s_flash_result = W25Q128_ERROR_VERIFY;
    assert(file_transfer_console("FS STATUS", s_now));
    assert(strstr((const char *)s_output, " FLASH -7\r\n") != NULL);
    s_flash_result = W25Q128_OK;
}

static void test_rejected_cleanup(void)
{
    uint8_t cached[FILE_TRANSFER_FRAME_MAX];
    uint32_t calls;
    uint32_t attempts;
    s_submit_result = FILE_STORE_OK;
    start_session();
    clear_output();
    calls = s_calls;
    s_submit_result = FILE_STORE_ERROR_NOT_READY;
    make_frame(FILE_TRANSFER_OP_EXIT, 0x12345678U, 1U, 0U, NULL, 0U);
    feed_frame();
    file_transfer_poll(s_now);
    assert(s_calls == calls && !file_transfer_busy() && file_transfer_active());
    assert(s_output_length == 40U && s_output[5] == FILE_TRANSFER_OP_EXIT);
    assert(s_output[6] == FILE_TRANSFER_FLAG_FINAL);
    assert((int32_t)read32(s_output + 28U) == FILE_STORE_ERROR_NOT_READY);
    memcpy(cached, s_output, s_output_length);
    attempts = s_attempts;
    clear_output();
    s_now += 5001U;
    file_transfer_poll(s_now);
    feed_frame(); /* Failed cleanup still retains its final EXIT reply. */
    assert(s_attempts == attempts && s_output_length == 40U);
    assert(memcmp(cached, s_output, s_output_length) == 0);
    s_now += 7501U;
    file_transfer_poll(s_now);
    assert(!file_transfer_active() && !file_transfer_busy());
    clear_output();
    assert(file_transfer_console("FS STATUS", s_now));
    assert(strncmp((const char *)s_output, "FS STATE", 8U) == 0);

    start_session();
    clear_output();
    s_now += 30001U;
    file_transfer_poll(s_now); /* An unaccepted idle cancellation cannot trap RX. */
    assert(!file_transfer_active() && !file_transfer_busy());
    assert(s_calls == calls && s_output_length == 0U);
    attempts = s_attempts;
    file_transfer_poll(s_now);
    assert(s_attempts == attempts);
    s_submit_result = FILE_STORE_OK;
}

static void test_busy_retry(void)
{
    uint32_t calls;
    uint32_t attempts;
    s_submit_result = FILE_STORE_OK;
    start_session();
    clear_output();
    calls = s_calls;
    s_submit_result = FILE_STORE_ERROR_BUSY;
    make_frame(FILE_STORE_OP_MKDIR, 0x12345678U, 1U, 0U,
               (const uint8_t *)"/new", 4U);
    feed_frame();
    assert(s_calls == calls && !file_transfer_busy());
    assert(s_output_length == 40U && s_output[6] == FILE_TRANSFER_FLAG_BUSY);
    clear_output();
    s_submit_result = FILE_STORE_OK;
    feed_frame(); /* BUSY does not consume the sequence or cache a final. */
    assert(s_calls == calls + 1U && file_transfer_busy());
    complete(FILE_STORE_OK);

    start_session();
    clear_output();
    calls = s_calls;
    s_submit_result = FILE_STORE_ERROR_BUSY;
    make_frame(FILE_TRANSFER_OP_EXIT, 0x12345678U, 1U, 0U, NULL, 0U);
    feed_frame();
    file_transfer_poll(s_now);
    assert(s_calls == calls && file_transfer_active() && file_transfer_busy());
    assert(s_output_length == 0U);
    clear_output();
    feed_frame();
    assert(s_output_length == 40U && s_output[6] == FILE_TRANSFER_FLAG_BUSY);
    clear_output();
    s_submit_result = FILE_STORE_OK;
    file_transfer_poll(s_now);
    assert(s_calls == calls + 1U && s_accepted.operation == FILE_STORE_OP_UPLOAD_CANCEL);
    attempts = s_attempts;
    s_submit_result = FILE_STORE_ERROR_NOT_READY;
    s_now += 31000U;
    file_transfer_poll(s_now); /* An accepted cleanup must retain its ownership. */
    assert(s_attempts == attempts && file_transfer_active() && file_transfer_busy());
    clear_output();
    complete(FILE_STORE_OK);
    assert(!file_transfer_busy() && file_transfer_active());
    assert(s_output_length == 40U && s_output[5] == FILE_TRANSFER_OP_EXIT);
    assert((int32_t)read32(s_output + 28U) == FILE_STORE_OK);
    s_now += 7501U;
    file_transfer_poll(s_now);
    assert(!file_transfer_active());
    s_submit_result = FILE_STORE_OK;
}

int main(void)
{
    uint8_t payload[256];
    uint8_t cached[FILE_TRANSFER_FRAME_MAX];
    uint16_t cached_length;
    uint32_t calls;
    uint16_t i;
    assert(crc32((const uint8_t *)"123456789", 9U) == 0xCBF43926UL);
    file_transfer_init(send_bytes);
    assert(!file_transfer_console("FORMAT 24", 0U));
    assert(file_transfer_console("FS FORMAT", 0U));
    assert(s_calls == 0U);
    clear_output();
    assert(file_transfer_console("FS FORMAT CONFIRM", 0U));
    assert(s_calls == 1U && s_accepted.confirm != 0U);
    complete(0);
    assert(strcmp((const char *)s_output, "OK FS\r\n") == 0);
    s_calls = 0U;
    s_now = 100U;
    start_session();
    clear_output();
    feed_frame(); /* Lost HELLO ACK. */
    assert(s_output_length == 40U && s_output[6] == 1U && s_calls == 0U);
    clear_output();
    make_frame(FILE_STORE_OP_UPLOAD_BEGIN, 0x12345678U, 1U, 0U,
               (const uint8_t *)"/image.bin", 10U);
    feed_frame();
    assert(s_calls == 1U && s_accepted.operation == FILE_STORE_OP_UPLOAD_BEGIN);
    assert(strcmp(s_accepted.path, "/image.bin") == 0);
    assert(s_output[6] == 2U && file_transfer_busy());
    clear_output();
    feed_frame(); /* Pending duplicate must not execute twice. */
    assert(s_calls == 1U && s_output[6] == 2U);
    clear_output();
    s_frame[36] = 'X';
    write32(s_frame + s_frame_length - 4U, crc32(s_frame + 4U, s_frame_length - 8U));
    feed_frame();
    assert(s_calls == 1U && (int32_t)read32(s_output + 28U) == FILE_STORE_ERROR_SEQUENCE);
    clear_output();
    complete(0);
    assert(s_output[6] == 1U && !file_transfer_busy());
    cached_length = s_output_length;
    memcpy(cached, s_output, cached_length);
    clear_output();
    make_frame(FILE_STORE_OP_UPLOAD_BEGIN, 0x12345678U, 1U, 0U,
               (const uint8_t *)"/image.bin", 10U);
    feed_frame();
    assert(s_calls == 1U && s_output_length == cached_length);
    assert(memcmp(s_output, cached, cached_length) == 0);
    clear_output();
    for (i = 0U; i < sizeof(payload); i++) { payload[i] = (uint8_t)i; }
    make_frame(FILE_STORE_OP_UPLOAD_WRITE, 0x12345678U, 2U, 0U, payload, sizeof(payload));
    s_frame[s_frame_length - 1U] ^= 1U;
    feed_frame(); /* Corrupt CRC is ignored and next magic resynchronizes. */
    assert(s_calls == 1U && s_output_length == 0U);
    s_frame[s_frame_length - 1U] ^= 1U;
    feed_frame();
    assert(s_calls == 2U && s_accepted.length == 256U);
    assert(memcmp(s_accepted.data, payload, sizeof(payload)) == 0);
    complete(0);
    clear_output();
    make_frame(FILE_STORE_OP_REMOVE, 0xAAAAAAAAU, 3U, 0U, (const uint8_t *)"/old", 4U);
    feed_frame();
    assert(s_calls == 2U && s_output_length == 0U);
    make_frame(FILE_STORE_OP_UPLOAD_COMMIT, 0x12345678U, 4U, 0U, NULL, 0U);
    feed_frame();
    assert(s_calls == 2U && (int32_t)read32(s_output + 28U) == FILE_STORE_ERROR_SEQUENCE);
    clear_output();
    make_frame(FILE_STORE_OP_UPLOAD_COMMIT, 0x12345678U, 3U, 0U, NULL, 0U);
    feed_frame();
    assert(s_calls == 3U);
    s_now += 31000U;
    clear_output();
    file_transfer_poll(s_now);
    assert(s_calls == 3U && file_transfer_active() && file_transfer_busy());
    assert(s_output_length == 40U && s_output[6] == 2U);
    clear_output();
    complete(0); /* A long operation earns a fresh idle window after completion. */
    assert(s_calls == 3U && file_transfer_active() && !file_transfer_busy());
    s_now += 30001U;
    file_transfer_poll(s_now);
    assert(s_calls == 4U && s_accepted.operation == FILE_STORE_OP_UPLOAD_CANCEL);
    assert(file_transfer_active());
    complete(0);
    assert(!file_transfer_active());
    s_now += 1U;
    start_session();
    clear_output();
    calls = s_calls;
    /* An oversized length cannot overrun the parser or consume the next frame. */
    make_frame(FILE_STORE_OP_STATUS, 0x12345678U, 1U, 0U, NULL, 0U);
    s_frame[32] = 1U;
    s_frame[33] = 1U;
    feed_frame();
    assert(s_calls == calls && s_output_length == 0U);
    file_transfer_rx_error();
    make_frame(FILE_TRANSFER_OP_EXIT, 0x12345678U, 1U, 0U, NULL, 0U);
    feed_frame();
    file_transfer_poll(s_now);
    assert(s_calls == calls + 1U && s_accepted.operation == FILE_STORE_OP_UPLOAD_CANCEL);
    complete(0);
    assert(s_output[5] == FILE_TRANSFER_OP_EXIT && s_output[6] == 1U);
    clear_output();
    feed_frame(); /* Final EXIT reply is replayable while lingering. */
    assert(s_calls == calls + 1U && s_output_length == 40U);
    assert(s_output[5] == FILE_TRANSFER_OP_EXIT && s_output[6] == 1U);
    s_now += 5001U;
    file_transfer_poll(s_now);
    assert(file_transfer_active());
    clear_output();
    feed_frame(); /* First 5-second ACK retry still falls within the grace period. */
    assert(s_calls == calls + 1U && s_output_length == 40U);
    assert(s_output[5] == FILE_TRANSFER_OP_EXIT && s_output[6] == 1U);
    s_now += 7501U;
    file_transfer_poll(s_now);
    assert(!file_transfer_active());
    test_rejected_request();
    test_rejected_cleanup();
    test_busy_retry();
    puts("PASS: framed CRC, replay, 256-byte data, safe timeout, rejected requests and recoverable cleanup");
    return 0;
}
