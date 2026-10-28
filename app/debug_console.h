#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

#include <stdint.h>
#include "clock_model.h"

#define DEBUG_CONSOLE_LINE_MAX 31U
#define DEBUG_CONSOLE_WEATHER_LINE_MAX 127U
#define DEBUG_CONSOLE_FILES_LINE_MAX 223U
#define DEBUG_CONSOLE_TIME_LINE_LENGTH 24U

typedef enum {
    DEBUG_COMMAND_NONE = 0, DEBUG_COMMAND_ON, DEBUG_COMMAND_OFF,
    DEBUG_COMMAND_FORMAT_12, DEBUG_COMMAND_FORMAT_24,
    DEBUG_COMMAND_ERRORS, DEBUG_COMMAND_UNKNOWN, DEBUG_COMMAND_RX_ERROR,
    DEBUG_COMMAND_RX_OVERFLOW, DEBUG_COMMAND_WEATHER,
    DEBUG_COMMAND_PAGE_WEATHER, DEBUG_COMMAND_PAGE_CLOCK, DEBUG_COMMAND_TIME,
    DEBUG_COMMAND_FILES
} debug_command_t;

typedef struct {
    char line[DEBUG_CONSOLE_FILES_LINE_MAX + 1U];
    uint16_t length;
    uint8_t discard;
    uint8_t previous_cr;
} debug_console_parser_t;

void debug_console_parser_reset(debug_console_parser_t *p_parser);
/* A damaged or overlong line is discarded through its terminating CR/LF. */
debug_command_t debug_console_parser_feed(debug_console_parser_t *p_parser,
                                         uint8_t byte);
/* Fixed ASCII TIME YYYY-MM-DDTHH:MM:SS; output is unchanged on failure. */
uint8_t debug_console_parse_time(const char *p_line, clock_calendar_t *p_calendar);

#endif
