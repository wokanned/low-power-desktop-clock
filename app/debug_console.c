#include "debug_console.h"
#include <string.h>

static uint8_t debug_console_utf8_valid(const uint8_t *p_text, uint8_t length)
{
    uint8_t i = 0U;
    uint8_t first;
    uint8_t second;
    while (i < length) {
        first = p_text[i];
        if (first < 0x80U) { i++; continue; }
        if ((first >= 0xC2U) && (first <= 0xDFU)) {
            if ((uint8_t)(i + 1U) >= length ||
                (p_text[i + 1U] & 0xC0U) != 0x80U) { return 0U; }
            i = (uint8_t)(i + 2U);
            continue;
        }
        if ((first >= 0xE0U) && (first <= 0xEFU)) {
            if ((uint8_t)(i + 2U) >= length) { return 0U; }
            second = p_text[i + 1U];
            if (((second & 0xC0U) != 0x80U) ||
                ((p_text[i + 2U] & 0xC0U) != 0x80U) ||
                ((first == 0xE0U) && (second < 0xA0U)) ||
                ((first == 0xEDU) && (second >= 0xA0U))) { return 0U; }
            i = (uint8_t)(i + 3U);
            continue;
        }
        if ((first >= 0xF0U) && (first <= 0xF4U)) {
            if ((uint8_t)(i + 3U) >= length) { return 0U; }
            second = p_text[i + 1U];
            if (((second & 0xC0U) != 0x80U) ||
                ((p_text[i + 2U] & 0xC0U) != 0x80U) ||
                ((p_text[i + 3U] & 0xC0U) != 0x80U) ||
                ((first == 0xF0U) && (second < 0x90U)) ||
                ((first == 0xF4U) && (second >= 0x90U))) { return 0U; }
            i = (uint8_t)(i + 4U);
            continue;
        }
        return 0U;
    }
    return 1U;
}

void debug_console_parser_reset(debug_console_parser_t *p_parser)
{
    memset(p_parser, 0, sizeof(*p_parser));
}

static uint8_t debug_console_weather_line(const char *p_line, uint8_t length)
{
    return (uint8_t)(length >= 8U && memcmp(p_line, "WEATHER", 7U) == 0 &&
                     (p_line[7] == ' ' || p_line[7] == ','));
}

static debug_command_t debug_console_command(const char *p_line)
{
    if (strcmp(p_line, "CONSOLE ON") == 0) { return DEBUG_COMMAND_ON; }
    if (strcmp(p_line, "CONSOLE OFF") == 0) { return DEBUG_COMMAND_OFF; }
    if (strcmp(p_line, "FORMAT 12") == 0) { return DEBUG_COMMAND_FORMAT_12; }
    if (strcmp(p_line, "FORMAT 24") == 0) { return DEBUG_COMMAND_FORMAT_24; }
    if (strcmp(p_line, "ERRORS") == 0) { return DEBUG_COMMAND_ERRORS; }
    if ((strncmp(p_line, "WEATHER", 7U) == 0) &&
        ((p_line[7] == '\0') || (p_line[7] == ' ') || (p_line[7] == ','))) {
        return DEBUG_COMMAND_WEATHER;
    }
    if (strcmp(p_line, "PAGE WEATHER") == 0) { return DEBUG_COMMAND_PAGE_WEATHER; }
    if (strcmp(p_line, "PAGE CLOCK") == 0) { return DEBUG_COMMAND_PAGE_CLOCK; }
    return DEBUG_COMMAND_UNKNOWN;
}

debug_command_t debug_console_parser_feed(debug_console_parser_t *p_parser,
                                         uint8_t byte)
{
    debug_command_t command;
    uint8_t line_max;
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
        if (!debug_console_utf8_valid((const uint8_t *)p_parser->line,
                                      p_parser->length)) {
            p_parser->length = 0U;
            return DEBUG_COMMAND_RX_ERROR;
        }
        command = debug_console_command(p_parser->line);
        p_parser->length = 0U;
        return command;
    }
    p_parser->previous_cr = 0U;
    if (p_parser->discard != 0U) { return DEBUG_COMMAND_NONE; }
    line_max = debug_console_weather_line(p_parser->line, p_parser->length) != 0U ?
        DEBUG_CONSOLE_WEATHER_LINE_MAX : DEBUG_CONSOLE_LINE_MAX;
    if (byte < 0x20U || byte == 0x7FU ||
        p_parser->length >= line_max) {
        p_parser->discard = p_parser->length >= line_max ? 2U : 1U;
        p_parser->length = 0U;
        return DEBUG_COMMAND_NONE;
    }
    p_parser->line[p_parser->length++] = (char)byte;
    return DEBUG_COMMAND_NONE;
}
