#include "file_transfer.h"
#include "storage_files.h"
#include <stdio.h>
#include <string.h>

#define FT_PAYLOAD_MAX 256U
#define FT_TRAILER_SIZE 4U
#define FT_IDLE_MS 30000UL
#define FT_FRAME_IDLE_MS 1000UL
#define FT_PROGRESS_MS 1000UL
#define FT_EXIT_LINGER_MS 7500UL
#define FT_CONFIRM_LENGTH 7U
#define FT_SESSION_OFFSET 8U
#define FT_SEQUENCE_OFFSET 12U
#define FT_OFFSET_OFFSET 16U
#define FT_SIZE_OFFSET 20U
#define FT_CRC_OFFSET 24U
#define FT_RESULT_OFFSET 28U
#define FT_LENGTH_OFFSET 32U
#define FT_RESERVED_OFFSET 34U

static file_transfer_send_t s_send;
static file_store_request_t s_request;
static file_store_response_t s_response;
static uint8_t s_rx[FILE_TRANSFER_FRAME_MAX];
static uint8_t s_pending_frame[FILE_TRANSFER_FRAME_MAX];
static uint8_t s_last_request[FILE_TRANSFER_FRAME_MAX];
static uint8_t s_last_response[FILE_TRANSFER_FRAME_MAX];
static uint8_t s_tx[FILE_TRANSFER_FRAME_MAX];
static uint16_t s_rx_length;
static uint16_t s_rx_expected;
static uint16_t s_pending_length;
static uint16_t s_last_request_length;
static uint16_t s_last_response_length;
static uint32_t s_session;
static uint32_t s_sequence;
static uint32_t s_pending_sequence;
static uint32_t s_token;
static uint32_t s_last_activity;
static uint32_t s_last_byte;
static uint32_t s_last_progress;
static uint8_t s_active;
static uint8_t s_pending;
static uint8_t s_pending_binary;
static uint8_t s_cleanup;
static uint8_t s_exit_reply;
static uint8_t s_listing;
static char s_text[160];

static uint16_t s_read16(const uint8_t *p_bytes)
{
    return (uint16_t)((uint16_t)p_bytes[0] | ((uint16_t)p_bytes[1] << 8U));
}

static uint32_t s_read32(const uint8_t *p_bytes)
{
    return (uint32_t)p_bytes[0] | ((uint32_t)p_bytes[1] << 8U) |
        ((uint32_t)p_bytes[2] << 16U) | ((uint32_t)p_bytes[3] << 24U);
}

static void s_write16(uint8_t *p_bytes, uint16_t value)
{
    p_bytes[0] = (uint8_t)value;
    p_bytes[1] = (uint8_t)(value >> 8U);
}

static void s_write32(uint8_t *p_bytes, uint32_t value)
{
    uint8_t i;
    for (i = 0U; i < 4U; i++) { p_bytes[i] = (uint8_t)(value >> (8U * i)); }
}

static uint32_t s_crc32(const uint8_t *p_data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    uint8_t bit;
    for (i = 0U; i < length; i++) {
        crc ^= p_data[i];
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

static void s_text_send(const char *p_text)
{
    if (s_send != NULL) { (void)s_send((const uint8_t *)p_text, (uint16_t)strlen(p_text)); }
}

static uint16_t s_encode(uint8_t op, uint8_t flags, uint32_t sequence,
                         int32_t result, const file_store_response_t *p_response)
{
    uint16_t length = p_response != NULL ? p_response->length : 0U;
    if (length > FT_PAYLOAD_MAX) { length = 0U; result = FILE_STORE_ERROR_ARGUMENT; }
    memset(s_tx, 0, FILE_TRANSFER_HEADER_SIZE);
    memcpy(s_tx, "DCFS", 4U);
    s_tx[4] = FILE_TRANSFER_VERSION;
    s_tx[5] = op;
    s_tx[6] = flags;
    s_tx[7] = p_response != NULL ? p_response->type : 0U;
    s_write32(s_tx + FT_SESSION_OFFSET, s_session);
    s_write32(s_tx + FT_SEQUENCE_OFFSET, sequence);
    if (p_response != NULL) {
        s_write32(s_tx + FT_OFFSET_OFFSET, p_response->offset);
        s_write32(s_tx + FT_SIZE_OFFSET, p_response->size);
        s_write32(s_tx + FT_CRC_OFFSET, p_response->crc32);
        memcpy(s_tx + FILE_TRANSFER_HEADER_SIZE, p_response->data, length);
    }
    s_write32(s_tx + FT_RESULT_OFFSET, (uint32_t)result);
    s_write16(s_tx + FT_LENGTH_OFFSET, length);
    s_write32(s_tx + FILE_TRANSFER_HEADER_SIZE + length,
        s_crc32(s_tx + 4U, FILE_TRANSFER_HEADER_SIZE - 4U + length));
    return (uint16_t)(FILE_TRANSFER_HEADER_SIZE + length + FT_TRAILER_SIZE);
}

static void s_reply(uint8_t op, uint8_t flags, uint32_t sequence, int32_t result)
{
    uint16_t length = s_encode(op, flags, sequence, result, NULL);
    if (s_send != NULL) { (void)s_send(s_tx, length); }
}

static int32_t s_submit(void)
{
    int32_t result;
    s_token++;
    if (s_token == 0U) { s_token++; }
    s_request.token = s_token;
    result = storage_file_request(&s_request);
    if (result == FILE_STORE_OK) { s_pending = 1U; }
    return result;
}

static void s_text_submit_error(int32_t result)
{
    if (result == FILE_STORE_ERROR_BUSY) { s_text_send("ERR BUSY\r\n"); }
    else {
        (void)snprintf(s_text, sizeof(s_text), "ERR FS %ld\r\n", (long)result);
        s_text_send(s_text);
    }
}

static uint8_t s_path(char *p_target, const uint8_t *p_bytes, uint16_t length)
{
    uint16_t i;
    if (length == 0U || length > FILE_STORE_PATH_MAX) { return 0U; }
    for (i = 0U; i < length; i++) {
        if (p_bytes[i] < 0x20U || p_bytes[i] >= 0x7FU) { return 0U; }
    }
    memcpy(p_target, p_bytes, length);
    p_target[length] = '\0';
    return 1U;
}

static uint8_t s_decode(void)
{
    const uint8_t *p_payload = s_rx + FILE_TRANSFER_HEADER_SIZE;
    uint16_t length = s_read16(s_rx + FT_LENGTH_OFFSET);
    uint16_t split;
    uint8_t op = s_rx[5];
    memset(&s_request, 0, sizeof(s_request));
    s_request.operation = (file_store_operation_t)op;
    s_request.offset = s_read32(s_rx + FT_OFFSET_OFFSET);
    s_request.size = s_read32(s_rx + FT_SIZE_OFFSET);
    s_request.crc32 = s_read32(s_rx + FT_CRC_OFFSET);
    switch (op) {
    case FILE_STORE_OP_LIST:
    case FILE_STORE_OP_MKDIR:
    case FILE_STORE_OP_READ:
    case FILE_STORE_OP_REMOVE:
    case FILE_STORE_OP_UPLOAD_BEGIN:
    case FILE_STORE_OP_STAT:
        if (!s_path(s_request.path, p_payload, length)) { return 0U; }
        if (op == FILE_STORE_OP_READ) {
            if (s_request.size == 0U || s_request.size > FILE_STORE_DATA_MAX) { return 0U; }
            s_request.length = (uint16_t)s_request.size;
        }
        return 1U;
    case FILE_STORE_OP_RENAME:
        for (split = 0U; split < length && p_payload[split] != 0U; split++) { }
        if (split == length || !s_path(s_request.path, p_payload, split)) { return 0U; }
        return s_path(s_request.path2, p_payload + split + 1U,
                      (uint16_t)(length - split - 1U));
    case FILE_STORE_OP_UPLOAD_WRITE:
        if (length == 0U) { return 0U; }
        s_request.length = length;
        memcpy(s_request.data, p_payload, length);
        return 1U;
    case FILE_STORE_OP_UPLOAD_COMMIT:
    case FILE_STORE_OP_UPLOAD_CANCEL:
        return length == 0U;
    case FILE_STORE_OP_FORMAT:
    case FILE_STORE_OP_SELFTEST:
        if (length != FT_CONFIRM_LENGTH || memcmp(p_payload, "CONFIRM", FT_CONFIRM_LENGTH) != 0) {
            return 0U;
        }
        s_request.confirm = 1U;
        return 1U;
    default: return 0U;
    }
}

static void s_cache_final(uint16_t request_length, uint16_t response_length)
{
    memcpy(s_last_request, s_pending_frame, request_length);
    s_last_request_length = request_length;
    memcpy(s_last_response, s_tx, response_length);
    s_last_response_length = response_length;
}

static void s_process(uint32_t now)
{
    file_store_snapshot_t snapshot;
    uint32_t session = s_read32(s_rx + FT_SESSION_OFFSET);
    uint32_t sequence = s_read32(s_rx + FT_SEQUENCE_OFFSET);
    uint16_t length;
    uint8_t op = s_rx[5];
    int32_t result;
    if (s_rx[4] != FILE_TRANSFER_VERSION || s_rx[6] != 0U || s_rx[7] != 0U ||
        s_read16(s_rx + FT_RESERVED_OFFSET) != 0U ||
        s_read32(s_rx + FT_RESULT_OFFSET) != 0U || session == 0U) { return; }
    if (s_session == 0U) {
        if (op != FILE_TRANSFER_OP_HELLO || sequence != 0U ||
            s_read16(s_rx + FT_LENGTH_OFFSET) != 0U) { return; }
        s_session = session;
        s_sequence = 1U;
        s_last_activity = now;
        memcpy(s_pending_frame, s_rx, s_rx_length);
        length = s_encode(op, FILE_TRANSFER_FLAG_FINAL, sequence, 0, NULL);
        s_cache_final(s_rx_length, length);
        if (s_send != NULL) { (void)s_send(s_tx, length); }
        return;
    }
    if (session != s_session) { return; }
    s_last_activity = now;
    if (s_last_request_length == s_rx_length &&
        memcmp(s_last_request, s_rx, s_rx_length) == 0) {
        if (s_send != NULL) { (void)s_send(s_last_response, s_last_response_length); }
        return;
    }
    if (s_active == 2U) { return; }
    if (s_pending != 0U || s_cleanup != 0U) {
        result = s_pending_length == s_rx_length &&
            memcmp(s_pending_frame, s_rx, s_rx_length) == 0 ?
            FILE_STORE_ERROR_BUSY : FILE_STORE_ERROR_SEQUENCE;
        s_reply(op, FILE_TRANSFER_FLAG_BUSY, sequence, result);
        return;
    }
    if (sequence != s_sequence || sequence == 0xFFFFFFFFUL) {
        s_reply(op, FILE_TRANSFER_FLAG_FINAL, sequence, FILE_STORE_ERROR_SEQUENCE);
        return;
    }
    memcpy(s_pending_frame, s_rx, s_rx_length);
    s_pending_length = s_rx_length;
    s_pending_sequence = sequence;
    s_pending_binary = 1U;
    s_last_progress = now;
    if (op == FILE_STORE_OP_STATUS && s_read16(s_rx + FT_LENGTH_OFFSET) == 0U) {
        storage_get_files_snapshot(&snapshot);
        memset(&s_response, 0, sizeof(s_response));
        s_response.offset = (uint32_t)snapshot.state;
        s_response.size = snapshot.total_blocks;
        s_response.crc32 = snapshot.used_blocks;
        s_response.type = (uint8_t)((snapshot.upload_active != 0U ? 1U : 0U) |
                                    (snapshot.busy != 0U ? 2U : 0U));
        length = s_encode(op, FILE_TRANSFER_FLAG_FINAL, sequence, snapshot.result, &s_response);
        s_cache_final(s_pending_length, length);
        s_sequence++;
        if (s_send != NULL) { (void)s_send(s_tx, length); }
        return;
    }
    if (op == FILE_TRANSFER_OP_EXIT && s_read16(s_rx + FT_LENGTH_OFFSET) == 0U) {
        s_cleanup = 1U;
        s_exit_reply = 1U;
        return;
    }
    if (!s_decode()) {
        length = s_encode(op, FILE_TRANSFER_FLAG_FINAL, sequence, FILE_STORE_ERROR_ARGUMENT, NULL);
        s_cache_final(s_pending_length, length);
        s_sequence++;
        if (s_send != NULL) { (void)s_send(s_tx, length); }
        return;
    }
    result = s_submit();
    if (result != FILE_STORE_OK && result != FILE_STORE_ERROR_BUSY) {
        length = s_encode(op, FILE_TRANSFER_FLAG_FINAL, sequence, result, NULL);
        s_cache_final(s_pending_length, length);
        s_sequence++;
        if (s_send != NULL) { (void)s_send(s_tx, length); }
        return;
    }
    s_reply(op, FILE_TRANSFER_FLAG_BUSY, sequence, FILE_STORE_ERROR_BUSY);
}

void file_transfer_init(file_transfer_send_t send)
{
    s_send = send;
    s_rx_length = 0U;
    s_rx_expected = 0U;
    s_active = 0U;
    s_pending = 0U;
    s_pending_binary = 0U;
    s_cleanup = 0U;
    s_exit_reply = 0U;
    s_listing = 0U;
    s_session = 0U;
    s_sequence = 1U;
    s_token = 0U;
    s_last_request_length = 0U;
    s_last_response_length = 0U;
}

uint8_t file_transfer_active(void) { return s_active; }
uint8_t file_transfer_busy(void) { return (uint8_t)(s_pending != 0U || s_cleanup != 0U); }

static void s_enter(uint32_t now)
{
    s_active = 1U;
    s_session = 0U;
    s_sequence = 1U;
    s_last_activity = now;
    s_last_byte = now;
    s_last_request_length = 0U;
    s_last_response_length = 0U;
    s_rx_length = 0U;
    s_rx_expected = 0U;
    s_pending_length = 0U;
    s_text_send("OK FS TRANSFER\r\n");
}

uint8_t file_transfer_console(const char *p_line, uint32_t now)
{
    const char *p_path;
    const char *p_split;
    size_t length;
    int32_t result;
    file_store_snapshot_t snapshot;
    if (p_line == NULL || strncmp(p_line, "FS", 2U) != 0 ||
        (p_line[2] != '\0' && p_line[2] != ' ')) { return 0U; }
    if (file_transfer_busy() != 0U) { s_text_send("ERR BUSY\r\n"); return 1U; }
    if (strcmp(p_line, "FS TRANSFER") == 0) { s_enter(now); return 1U; }
    if (strcmp(p_line, "FS STATUS") == 0) {
        storage_get_files_snapshot(&snapshot);
        (void)snprintf(s_text, sizeof(s_text),
            "FS STATE %u RESULT %ld BLOCKS %lu/%lu UPLOAD %u BUSY %u FLASH %ld\r\n",
            (unsigned int)snapshot.state, (long)snapshot.result,
            (unsigned long)snapshot.used_blocks, (unsigned long)snapshot.total_blocks,
            (unsigned int)snapshot.upload_active, (unsigned int)snapshot.busy,
            (long)snapshot.flash_result);
        s_text_send(s_text);
        return 1U;
    }
    memset(&s_request, 0, sizeof(s_request));
    s_pending_binary = 0U;
    s_listing = 0U;
    p_path = NULL;
    if (strcmp(p_line, "FS FORMAT CONFIRM") == 0) {
        s_request.operation = FILE_STORE_OP_FORMAT;
        s_request.confirm = 1U;
    } else if (strcmp(p_line, "FS SELFTEST CONFIRM") == 0) {
        s_request.operation = FILE_STORE_OP_SELFTEST;
        s_request.confirm = 1U;
    } else if (strncmp(p_line, "FS LIST ", 8U) == 0) {
        s_request.operation = FILE_STORE_OP_LIST;
        s_listing = 1U;
        p_path = p_line + 8U;
    } else if (strncmp(p_line, "FS MKDIR ", 9U) == 0) {
        s_request.operation = FILE_STORE_OP_MKDIR;
        p_path = p_line + 9U;
    } else if (strncmp(p_line, "FS REMOVE ", 10U) == 0) {
        s_request.operation = FILE_STORE_OP_REMOVE;
        p_path = p_line + 10U;
    } else if (strncmp(p_line, "FS RENAME ", 10U) == 0) {
        s_request.operation = FILE_STORE_OP_RENAME;
        p_path = p_line + 10U;
        p_split = strchr(p_path, ' ');
        if (p_split == NULL || !s_path(s_request.path, (const uint8_t *)p_path,
            (uint16_t)(p_split - p_path))) { s_text_send("ERR FS ARGUMENT\r\n"); return 1U; }
        p_path = p_split + 1;
        length = strlen(p_path);
        if (!s_path(s_request.path2, (const uint8_t *)p_path, (uint16_t)length) ||
            strchr(p_path, ' ') != NULL) { s_text_send("ERR FS ARGUMENT\r\n"); return 1U; }
        p_path = NULL;
    } else {
        s_text_send("ERR FS COMMAND\r\n");
        return 1U;
    }
    if (p_path != NULL) {
        length = strlen(p_path);
        if (length > FILE_STORE_PATH_MAX || strchr(p_path, ' ') != NULL ||
            !s_path(s_request.path, (const uint8_t *)p_path, (uint16_t)length)) {
            s_listing = 0U;
            s_text_send("ERR FS ARGUMENT\r\n");
            return 1U;
        }
    }
    result = s_submit();
    if (result != FILE_STORE_OK) { s_listing = 0U; s_text_submit_error(result); }
    return 1U;
}

void file_transfer_rx_error(void)
{
    s_rx_length = 0U;
    s_rx_expected = 0U;
}

void file_transfer_feed(uint8_t byte, uint32_t now)
{
    static const uint8_t s_magic[4] = {'D', 'C', 'F', 'S'};
    uint16_t payload_length;
    uint32_t crc;
    if (s_active == 0U) { return; }
    if ((uint32_t)(now - s_last_byte) >= FT_FRAME_IDLE_MS) { file_transfer_rx_error(); }
    s_last_byte = now;
    if (s_rx_length < 4U) {
        if (byte == s_magic[s_rx_length]) { s_rx[s_rx_length++] = byte; }
        else { s_rx_length = byte == s_magic[0] ? 1U : 0U; s_rx[0] = byte; }
        return;
    }
    s_rx[s_rx_length++] = byte;
    if (s_rx_length == FILE_TRANSFER_HEADER_SIZE) {
        payload_length = s_read16(s_rx + FT_LENGTH_OFFSET);
        if (payload_length > FT_PAYLOAD_MAX) { file_transfer_rx_error(); return; }
        s_rx_expected = (uint16_t)(FILE_TRANSFER_HEADER_SIZE + payload_length + FT_TRAILER_SIZE);
    }
    if (s_rx_expected != 0U && s_rx_length == s_rx_expected) {
        crc = s_crc32(s_rx + 4U, (uint32_t)s_rx_length - 4U - FT_TRAILER_SIZE);
        if (crc == s_read32(s_rx + s_rx_length - FT_TRAILER_SIZE)) { s_process(now); }
        file_transfer_rx_error();
    }
}

static void s_complete_binary(void)
{
    uint16_t length;
    length = s_encode((uint8_t)s_response.operation, FILE_TRANSFER_FLAG_FINAL,
                      s_pending_sequence, s_response.result, &s_response);
    s_cache_final(s_pending_length, length);
    s_sequence++;
    if (s_send != NULL) { (void)s_send(s_tx, length); }
}

static void s_complete_text(void)
{
    int32_t result;
    if (s_response.result == FILE_STORE_OK && s_listing != 0U &&
        s_response.length != 0U && s_response.length <= FILE_STORE_NAME_MAX) {
        s_response.data[s_response.length] = '\0';
        (void)snprintf(s_text, sizeof(s_text), "FS ENTRY %u %lu %s\r\n",
            (unsigned int)s_response.type, (unsigned long)s_response.size,
            (const char *)s_response.data);
        s_text_send(s_text);
        s_request.offset = s_response.offset;
        result = s_submit();
        if (result != FILE_STORE_OK) { s_listing = 0U; s_text_submit_error(result); }
        return;
    }
    s_listing = 0U;
    if (s_response.result == FILE_STORE_OK) { s_text_send("OK FS\r\n"); }
    else {
        (void)snprintf(s_text, sizeof(s_text), "ERR FS %ld\r\n", (long)s_response.result);
        s_text_send(s_text);
    }
}

static void s_finish_cleanup(int32_t result, uint32_t now)
{
    uint16_t length;
    /* Called only after a retained result was taken, or when cancellation was
     * rejected before ownership transfer. Never abandon an accepted request. */
    if (s_exit_reply != 0U) {
        length = s_encode(FILE_TRANSFER_OP_EXIT, FILE_TRANSFER_FLAG_FINAL,
            s_pending_sequence, result, NULL);
        s_cache_final(s_pending_length, length);
        s_sequence++;
        s_active = 2U;
        s_last_activity = now;
        if (s_send != NULL) { (void)s_send(s_tx, length); }
    } else {
        s_active = 0U;
        s_session = 0U;
    }
    s_cleanup = 0U;
    s_exit_reply = 0U;
    s_pending_binary = 0U;
    file_transfer_rx_error();
}

void file_transfer_poll(uint32_t now)
{
    int32_t result;
    if (s_pending != 0U) {
        if (storage_file_take_result(s_request.token, &s_response)) {
            s_pending = 0U;
            /* A long operation is activity. Give the peer a full idle window
             * to consume its final result and issue the next request. */
            s_last_activity = now;
            if (s_cleanup == 2U) {
                s_finish_cleanup(s_response.result, now);
                return;
            }
            if (s_pending_binary != 0U) { s_complete_binary(); }
            else { s_complete_text(); }
        } else if (s_pending_binary != 0U &&
            (uint32_t)(now - s_last_progress) >= FT_PROGRESS_MS) {
            s_last_progress = now;
            s_reply(s_cleanup == 2U ? FILE_TRANSFER_OP_EXIT : (uint8_t)s_request.operation,
                FILE_TRANSFER_FLAG_BUSY, s_pending_sequence, FILE_STORE_ERROR_BUSY);
        }
    }
    if (s_active == 0U) { return; }
    if (s_active == 2U) {
        if ((uint32_t)(now - s_last_activity) >= FT_EXIT_LINGER_MS) {
            s_active = 0U;
            s_session = 0U;
            file_transfer_rx_error();
        }
        return;
    }
    if ((uint32_t)(now - s_last_activity) >= FT_IDLE_MS && s_pending == 0U) {
        s_cleanup = 1U;
    }
    if (s_cleanup == 1U && s_pending == 0U) {
        memset(&s_request, 0, sizeof(s_request));
        s_request.operation = FILE_STORE_OP_UPLOAD_CANCEL;
        s_pending_binary = 1U;
        result = s_submit();
        if (result == FILE_STORE_OK) { s_cleanup = 2U; s_last_progress = now; }
        else if (result != FILE_STORE_ERROR_BUSY) { s_finish_cleanup(result, now); }
    }
}
