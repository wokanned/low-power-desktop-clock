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

int main(void)
{
    struct { uint32_t before; debug_console_parser_t parser; uint32_t after; } guarded;
    uint32_t i;
    guarded.before = 0x12345678U;
    guarded.after = 0xABCDEF12U;
    debug_console_parser_reset(&guarded.parser);
    assert(feed(&guarded.parser, "CONSOLE ON\r\n") == DEBUG_COMMAND_ON);
    assert(feed(&guarded.parser, "CONSOLE OFF\r") == DEBUG_COMMAND_OFF);
    assert(feed(&guarded.parser, "FORMAT 12\n") == DEBUG_COMMAND_FORMAT_12);
    assert(feed(&guarded.parser, "FORMAT 24\n") == DEBUG_COMMAND_FORMAT_24);
    assert(feed(&guarded.parser, "ERRORS\n") == DEBUG_COMMAND_ERRORS);
    assert(feed(&guarded.parser, "\r\n\n") == DEBUG_COMMAND_NONE);
    assert(feed(&guarded.parser, "console on\n") == DEBUG_COMMAND_UNKNOWN);
    assert(feed(&guarded.parser, "CONSOLE ON extra\n") == DEBUG_COMMAND_UNKNOWN);
    assert(feed(&guarded.parser, "FORMAT 12\x01\n") == DEBUG_COMMAND_RX_ERROR);
    assert(feed(&guarded.parser, "FORMAT 12\x80\n") == DEBUG_COMMAND_RX_ERROR);
    for (i = 0U; i < DEBUG_CONSOLE_LINE_MAX; i++) {
        assert(debug_console_parser_feed(&guarded.parser, 'A') == DEBUG_COMMAND_NONE);
    }
    assert(debug_console_parser_feed(&guarded.parser, '\n') == DEBUG_COMMAND_UNKNOWN);
    for (i = 0U; i < 4096U; i++) {
        assert(debug_console_parser_feed(&guarded.parser, 'B') == DEBUG_COMMAND_NONE);
    }
    assert(feed(&guarded.parser, "CONSOLE ON\r\n") == DEBUG_COMMAND_RX_OVERFLOW);
    assert(feed(&guarded.parser, "CONSOLE ON\n") == DEBUG_COMMAND_ON);
    assert(feed(&guarded.parser, "FORMAT ") == DEBUG_COMMAND_NONE);
    debug_console_parser_reset(&guarded.parser);
    assert(feed(&guarded.parser, "12\n") == DEBUG_COMMAND_UNKNOWN);
    assert(guarded.before == 0x12345678U && guarded.after == 0xABCDEF12U);
    puts("PASS: bounded console parser, exact commands, CR/LF and corrupt-line recovery");
    return 0;
}
