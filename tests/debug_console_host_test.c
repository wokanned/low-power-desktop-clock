#include "debug_console.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static debug_command_t feed(debug_console_parser_t *p_parser, const char *p_text)
{
    debug_command_t command = DEBUG_COMMAND_NONE;
    debug_command_t next;
    while (*p_text != '\0') {
        next = debug_console_parser_feed(p_parser, (uint8_t)*p_text++);
        if (next != DEBUG_COMMAND_NONE) {
            assert(command == DEBUG_COMMAND_NONE);
            command = next;
        }
    }
    return command;
}

static void make_weather_line(char *p_line, uint32_t length)
{
    static const char s_prefix[] = "WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,";
    static const char s_suffix[] = "2,235,180,270,65,2,35,2026-10-26T10:30";
    uint32_t prefix_length = (uint32_t)strlen(s_prefix);
    uint32_t suffix_length = (uint32_t)strlen(s_suffix);
    uint32_t padding = length - prefix_length - suffix_length;
    assert(length >= prefix_length + suffix_length && length <= 128U);
    memcpy(p_line, s_prefix, prefix_length);
    memset(p_line + prefix_length, '0', padding);
    memcpy(p_line + prefix_length + padding, s_suffix, suffix_length + 1U);
    assert(strlen(p_line) == length);
}

static void test_time_parser(void)
{
    static const char *const s_invalid[] = {
        "", "TIME", "TIME ", "TIME 2026-10-27T14:30",
        "TIME 1999-12-31T23:59:59", "TIME 2100-01-01T00:00:00",
        "TIME 2026-00-27T14:30:00", "TIME 2026-13-27T14:30:00",
        "TIME 2026-10-00T14:30:00", "TIME 2026-04-31T14:30:00",
        "TIME 2001-02-29T14:30:00", "TIME 2026-10-27T24:30:00",
        "TIME 2026-10-27T14:60:00", "TIME 2026-10-27T14:30:60",
        "TIME 2026/10/27T14:30:00", "TIME 2026-10-27 14:30:00",
        "TIME 2026-10-27T14-30:00", "TIME 2026-10-27T14:30-00",
        "TIME 2026-10-27T14:30:0x", "TIME 2026-10-27T14:30:00Z",
        "TIME 2026-10-27T14:30:00 ", " TIME 2026-10-27T14:30:00",
        "TIME  2026-10-27T14:30:00", "time 2026-10-27T14:30:00",
        "TIME 2026-10-27T14:30:\xEF\xBC\x90\xEF\xBC\x90"
    };
    clock_calendar_t calendar;
    clock_calendar_t before;
    uint32_t i;
    assert(debug_console_parse_time("TIME 2026-10-27T14:30:00", &calendar));
    assert(calendar.year == 2026U && calendar.month == 10U && calendar.day == 27U);
    assert(calendar.hour == 14U && calendar.minute == 30U && calendar.second == 0U);
    assert(calendar.weekday == 2U);
    assert(debug_console_parse_time("TIME 2000-01-01T00:00:00", &calendar));
    assert(calendar.weekday == 6U && calendar.second == 0U);
    assert(debug_console_parse_time("TIME 2000-02-29T23:59:59", &calendar));
    assert(calendar.weekday == 2U && calendar.second == 59U);
    assert(debug_console_parse_time("TIME 2099-12-31T23:59:59", &calendar));
    assert(calendar.weekday == 4U);
    before = calendar;
    for (i = 0U; i < sizeof(s_invalid) / sizeof(s_invalid[0]); i++) {
        assert(!debug_console_parse_time(s_invalid[i], &calendar));
        assert(memcmp(&before, &calendar, sizeof(calendar)) == 0);
    }
    assert(!debug_console_parse_time(NULL, &calendar));
    assert(!debug_console_parse_time("TIME 2026-10-27T14:30:00", NULL));
}

int main(void)
{
    struct { uint32_t before; debug_console_parser_t parser; uint32_t after; } guarded;
    char weather_line[129];
    char files_line[226];
    uint32_t i;
    guarded.before = 0x12345678U;
    guarded.after = 0xABCDEF12U;
    debug_console_parser_reset(&guarded.parser);
    assert(feed(&guarded.parser, "CONSOLE ON\r\n") == DEBUG_COMMAND_ON);
    assert(feed(&guarded.parser, "CONSOLE OFF\r") == DEBUG_COMMAND_OFF);
    assert(feed(&guarded.parser, "FORMAT 12\n") == DEBUG_COMMAND_FORMAT_12);
    assert(feed(&guarded.parser, "FORMAT 24\n") == DEBUG_COMMAND_FORMAT_24);
    assert(feed(&guarded.parser, "ERRORS\n") == DEBUG_COMMAND_ERRORS);
    assert(feed(&guarded.parser, "TIME 2026-10-") == DEBUG_COMMAND_NONE);
    assert(feed(&guarded.parser, "27T14:30:00") == DEBUG_COMMAND_NONE);
    assert(feed(&guarded.parser, "\r\n") == DEBUG_COMMAND_TIME);
    assert(strcmp(guarded.parser.line, "TIME 2026-10-27T14:30:00") == 0);
    assert(feed(&guarded.parser, "TIME 2000-02-29T23:59:59\n") == DEBUG_COMMAND_TIME);
    assert(feed(&guarded.parser, "TIME 2099-12-31T23:59:59\r") == DEBUG_COMMAND_TIME);
    assert(feed(&guarded.parser, "TIME\n") == DEBUG_COMMAND_TIME);
    assert(feed(&guarded.parser, "TIMEX 2026-10-27T14:30:00\n") == DEBUG_COMMAND_UNKNOWN);
    assert(feed(&guarded.parser, "\r\n\n") == DEBUG_COMMAND_NONE);
    assert(feed(&guarded.parser, "console on\n") == DEBUG_COMMAND_UNKNOWN);
    assert(feed(&guarded.parser, "CONSOLE ON extra\n") == DEBUG_COMMAND_UNKNOWN);
    assert(feed(&guarded.parser, "FORMAT 12\x01\n") == DEBUG_COMMAND_RX_ERROR);
    assert(feed(&guarded.parser, "FORMAT 12\x7F\n") == DEBUG_COMMAND_RX_ERROR);
    assert(feed(&guarded.parser, "FORMAT 12\x80\n") == DEBUG_COMMAND_RX_ERROR);
    assert(feed(&guarded.parser, "WEATHER \xC0\xAF\n") == DEBUG_COMMAND_RX_ERROR);
    assert(feed(&guarded.parser, "WEATHER \xED\xA0\x80\n") == DEBUG_COMMAND_RX_ERROR);
    assert(feed(&guarded.parser, "WEATHER \xF4\x90\x80\x80\n") == DEBUG_COMMAND_RX_ERROR);
    assert(feed(&guarded.parser, "WEATHER \xE6\xB7\n") == DEBUG_COMMAND_RX_ERROR);
    assert(feed(&guarded.parser,
        "WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,2,235,180,270,65,2,35,2026-10-26T10:30\r") ==
        DEBUG_COMMAND_WEATHER);
    assert(strncmp(guarded.parser.line, "WEATHER ", 8U) == 0);
    assert(feed(&guarded.parser, "PAGE WEATHER\n") == DEBUG_COMMAND_PAGE_WEATHER);
    assert(feed(&guarded.parser, "PAGE CLOCK\n") == DEBUG_COMMAND_PAGE_CLOCK);
    assert(feed(&guarded.parser, "FS STATUS\n") == DEBUG_COMMAND_FILES);
    memcpy(files_line, "FS RENAME ", 10U);
    memset(files_line + 10U, 'x', 213U);
    files_line[223] = '\n';
    files_line[224] = '\0';
    assert(feed(&guarded.parser, files_line) == DEBUG_COMMAND_FILES);
    files_line[223] = 'x';
    files_line[224] = '\n';
    files_line[225] = '\0';
    assert(feed(&guarded.parser, files_line) == DEBUG_COMMAND_RX_OVERFLOW);
    assert(feed(&guarded.parser, "WEATHER,\n") == DEBUG_COMMAND_WEATHER);
    for (i = 0U; i < 31U; i++) {
        assert(debug_console_parser_feed(&guarded.parser, 'A') == DEBUG_COMMAND_NONE);
    }
    assert(debug_console_parser_feed(&guarded.parser, '\n') == DEBUG_COMMAND_UNKNOWN);
    for (i = 0U; i < 32U; i++) {
        assert(debug_console_parser_feed(&guarded.parser, 'A') == DEBUG_COMMAND_NONE);
    }
    assert(debug_console_parser_feed(&guarded.parser, '\r') == DEBUG_COMMAND_RX_OVERFLOW);
    assert(debug_console_parser_feed(&guarded.parser, '\n') == DEBUG_COMMAND_NONE);
    assert(feed(&guarded.parser, "FORMAT 12\n") == DEBUG_COMMAND_FORMAT_12);
    assert(feed(&guarded.parser, "WEATHERX") == DEBUG_COMMAND_NONE);
    for (i = 8U; i < 32U; i++) {
        assert(debug_console_parser_feed(&guarded.parser, '0') == DEBUG_COMMAND_NONE);
    }
    assert(debug_console_parser_feed(&guarded.parser, '\n') == DEBUG_COMMAND_RX_OVERFLOW);
    make_weather_line(weather_line, 127U);
    assert(feed(&guarded.parser, weather_line) == DEBUG_COMMAND_NONE);
    assert(debug_console_parser_feed(&guarded.parser, '\r') == DEBUG_COMMAND_WEATHER);
    assert(strcmp(guarded.parser.line, weather_line) == 0);
    assert(debug_console_parser_feed(&guarded.parser, '\n') == DEBUG_COMMAND_NONE);
    make_weather_line(weather_line, 128U);
    assert(feed(&guarded.parser, weather_line) == DEBUG_COMMAND_NONE);
    assert(feed(&guarded.parser, "\r\n") == DEBUG_COMMAND_RX_OVERFLOW);
    assert(feed(&guarded.parser, "PAGE CLOCK\n") == DEBUG_COMMAND_PAGE_CLOCK);
    for (i = 0U; i < 4096U; i++) {
        assert(debug_console_parser_feed(&guarded.parser, 'B') == DEBUG_COMMAND_NONE);
    }
    assert(feed(&guarded.parser, "CONSOLE ON\r\n") == DEBUG_COMMAND_RX_OVERFLOW);
    assert(feed(&guarded.parser, "CONSOLE ON\n") == DEBUG_COMMAND_ON);
    assert(feed(&guarded.parser, "FORMAT ") == DEBUG_COMMAND_NONE);
    debug_console_parser_reset(&guarded.parser);
    assert(feed(&guarded.parser, "12\n") == DEBUG_COMMAND_UNKNOWN);
    assert(guarded.before == 0x12345678U && guarded.after == 0xABCDEF12U);
    test_time_parser();
    puts("PASS: strict TIME calendar, legacy 31/32 and WEATHER 127/128 limits, UTF-8 and recovery");
    return 0;
}
