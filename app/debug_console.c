#include "debug_console.h"
#include <string.h>

void debug_console_parser_reset(debug_console_parser_t *p_parser)
{
    memset(p_parser, 0, sizeof(*p_parser));
}

static debug_command_t debug_console_command(const char *p_line)
{
    if (strcmp(p_line, "CONSOLE ON") == 0) { return DEBUG_COMMAND_ON; }
    if (strcmp(p_line, "CONSOLE OFF") == 0) { return DEBUG_COMMAND_OFF; }
    if (strcmp(p_line, "FORMAT 12") == 0) { return DEBUG_COMMAND_FORMAT_12; }
    if (strcmp(p_line, "FORMAT 24") == 0) { return DEBUG_COMMAND_FORMAT_24; }
    if (strcmp(p_line, "ERRORS") == 0) { return DEBUG_COMMAND_ERRORS; }
    return DEBUG_COMMAND_UNKNOWN;
}

debug_command_t debug_console_parser_feed(debug_console_parser_t *p_parser,
                                         uint8_t byte)
{
    debug_command_t command;
    if (byte == '\r' || byte == '\n') {
        if (byte == '\n' && p_parser->previous_cr != 0U) {
            p_parser->previous_cr = 0U;
            return DEBUG_COMMAND_NONE;
        }
        p_parser->previous_cr = (uint8_t)(byte == '\r');
        if (p_parser->discard != 0U) {
            command = p_parser->discard == 2U ? DEBUG_COMMAND_RX_OVERFLOW :
                                               DEBUG_COMMAND_RX_ERROR;
            p_parser->discard = 0U;
            p_parser->length = 0U;
            return command;
        }
        if (p_parser->length == 0U) { return DEBUG_COMMAND_NONE; }
        p_parser->line[p_parser->length] = '\0';
        command = debug_console_command(p_parser->line);
        p_parser->length = 0U;
        return command;
    }
    p_parser->previous_cr = 0U;
    if (p_parser->discard != 0U) { return DEBUG_COMMAND_NONE; }
    if (byte < 0x20U || byte > 0x7EU ||
        p_parser->length >= DEBUG_CONSOLE_LINE_MAX) {
        p_parser->discard = p_parser->length >= DEBUG_CONSOLE_LINE_MAX ? 2U : 1U;
        p_parser->length = 0U;
        return DEBUG_COMMAND_NONE;
    }
    p_parser->line[p_parser->length++] = (char)byte;
    return DEBUG_COMMAND_NONE;
}
