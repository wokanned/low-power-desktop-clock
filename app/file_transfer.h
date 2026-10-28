#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stdint.h>

#define FILE_TRANSFER_HEADER_SIZE 36U
#define FILE_TRANSFER_FRAME_MAX 296U
#define FILE_TRANSFER_VERSION 1U
#define FILE_TRANSFER_FLAG_FINAL 1U
#define FILE_TRANSFER_FLAG_BUSY 2U
#define FILE_TRANSFER_OP_HELLO 0U
#define FILE_TRANSFER_OP_EXIT 14U

typedef uint8_t (*file_transfer_send_t)(const uint8_t *p_bytes, uint16_t length);

/* All calls belong to debugTask, the sole UART transmitter. No borrowed buffers.
 * Storage work continues across retries/timeouts; session cleanup waits for its
 * retained result before cancelling an upload. now is monotonic milliseconds. */
void file_transfer_init(file_transfer_send_t send);
uint8_t file_transfer_console(const char *p_line, uint32_t now);
void file_transfer_feed(uint8_t byte, uint32_t now);
void file_transfer_rx_error(void);
void file_transfer_poll(uint32_t now);
uint8_t file_transfer_active(void);
uint8_t file_transfer_busy(void);

#endif
