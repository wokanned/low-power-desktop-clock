#ifndef FORECAST_PROTOCOL_H
#define FORECAST_PROTOCOL_H

#include <stdint.h>

/* One task owns this transport-independent command session. The caller owns
 * framing/UTF-8 validation and console mode; send must copy/send synchronously.
 * No HAL, UART instance, Flash IO or RTC IO is performed here. */
typedef uint8_t (*forecast_protocol_send_t)(const uint8_t *p_data, uint16_t length);
void forecast_protocol_init(forecast_protocol_send_t send);
uint8_t forecast_protocol_command(const char *p_line, uint32_t now_tick);
void forecast_protocol_poll(uint32_t now_tick);
/* Call with zero when any TIME attempt starts/fails; one only after verified
 * RTC success. A new batch consumes one permission at BEGIN. */
void forecast_protocol_time_result(uint8_t success);
/* Console session boundaries discard collection/permission, never accepted IO. */
void forecast_protocol_reset_session(void);
uint8_t forecast_protocol_busy(void);

#endif
