#ifndef TIME_PROTOCOL_H
#define TIME_PROTOCOL_H

#include <stdint.h>

/* One debug-task-owned session. Caller gates console mode and file/forecast
 * transactions; send must consume/copy bytes synchronously on that task. */
typedef uint8_t (*time_protocol_send_t)(const uint8_t *p_data, uint16_t length);
void time_protocol_init(time_protocol_send_t send);
uint8_t time_protocol_command(const char *p_line, uint32_t receive_tick_ms);
void time_protocol_poll(void);
uint8_t time_protocol_busy(void);
/* Consume a successful TIME2 event for current-session forecast permission. */
uint8_t time_protocol_take_sync_success(void);

#endif
