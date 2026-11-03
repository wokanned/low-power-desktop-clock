#include "weather_feed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)

static char s_output[256];
static uint32_t s_tick;
static uint32_t s_id;
static uint32_t s_sequence;
static uint32_t s_crc;
static char s_command[WEATHER_FEED_LINE_MAX + 1U];
static uint32_t s_tests;
static char s_large_body[WEATHER_FEED_BODY_MAX + 1U];
static char s_large_guide[WEATHER_FEED_GUIDE_MAX + 1U];
static char s_large_title[WEATHER_FEED_TITLE_MAX + 1U];
static char s_large_source[WEATHER_FEED_SOURCE_MAX + 1U];
static char s_large_id[WEATHER_FEED_ID_MAX + 1U];

static uint8_t send_text(const uint8_t *p_data, uint16_t length)
{
    CHECK(length < sizeof(s_output)); memcpy(s_output, p_data, length);
    s_output[length] = '\0'; return 1U;
}

static uint32_t crc_update(uint32_t crc, const char *p_text)
{
    uint8_t bit;
    while (*p_text != '\0') {
        crc ^= (uint8_t)*p_text++;
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
        }
    }
    return crc;
}

static void command(const char *p_text, const char *p_expected)
{
    s_output[0] = '\0'; CHECK(weather_feed_command(p_text, s_tick) == 1U);
    if (strstr(s_output, p_expected) == NULL) {
        fprintf(stderr, "Command: %s\nExpected: %s\nActual: %s\n",
            p_text, p_expected, s_output); exit(1);
    }
    s_tests++;
}

static void begin(uint8_t days, uint8_t alerts, uint8_t state, uint16_t glyphs)
{
    s_id++; s_sequence = 0U; s_crc = UINT32_MAX;
    (void)snprintf(s_command, sizeof(s_command),
        "WDATA BEGIN %lu,1,%u,%u,%u,2026-11-02T10:35,2026-11-02T10:35,%u",
        (unsigned long)s_id, (unsigned int)days, (unsigned int)alerts,
        (unsigned int)state, (unsigned int)glyphs);
    command(s_command, "OK WDATA BEGIN");
}

static void record(const char *p_verb, const char *p_payload)
{
    (void)snprintf(s_command, sizeof(s_command), "WDATA %s %lu,%lu,%s",
        p_verb, (unsigned long)s_id, (unsigned long)s_sequence, p_payload);
    command(s_command, "OK WDATA");
    s_crc = crc_update(s_crc, s_command); s_crc = crc_update(s_crc, "\n");
    s_sequence++;
}

static void finish(const char *p_expected)
{
    (void)snprintf(s_command, sizeof(s_command), "WDATA COMMIT %lu,%lu,%08lX",
        (unsigned long)s_id, (unsigned long)s_sequence,
        (unsigned long)(s_crc ^ UINT32_MAX));
    command(s_command, p_expected);
}

static void base(void)
{
    record("NOW", "1,235,180,270,72,2,50,3,4,2026-11-02T10:30");
    record("METRICS", "251,60,42,0,1012,180");
}

static void text_record(uint8_t field, const char *p_text)
{
    char payload[190];
    static const char s_hex[] = "0123456789ABCDEF";
    size_t length = strlen(p_text);
    size_t i;
    int written;
    CHECK(length <= 80U);
    written = snprintf(payload, sizeof(payload), "0,%u,0,", (unsigned int)field);
    CHECK(written > 0);
    for (i = 0U; i < length; i++) {
        payload[(size_t)written + i * 2U] = s_hex[((uint8_t)p_text[i]) >> 4];
        payload[(size_t)written + i * 2U + 1U] = s_hex[((uint8_t)p_text[i]) & 15U];
    }
    payload[(size_t)written + length * 2U] = '\0'; record("TEXT", payload);
}

static void text_chunks(uint8_t index, uint8_t field, const char *p_text)
{
    char payload[190];
    static const char s_hex[] = "0123456789ABCDEF";
    size_t offset = 0U;
    size_t length = strlen(p_text);
    size_t chunk;
    size_t i;
    int written;
    while (offset < length) {
        chunk = length - offset; if (chunk > 80U) { chunk = 80U; }
        written = snprintf(payload, sizeof(payload), "%u,%u,%u,",
            (unsigned int)index, (unsigned int)field, (unsigned int)offset);
        CHECK(written > 0);
        for (i = 0U; i < chunk; i++) {
            payload[(size_t)written + i * 2U] = s_hex[((uint8_t)p_text[offset + i]) >> 4];
            payload[(size_t)written + i * 2U + 1U] = s_hex[((uint8_t)p_text[offset + i]) & 15U];
        }
        payload[(size_t)written + chunk * 2U] = '\0'; record("TEXT", payload); offset += chunk;
    }
}

static void alert(const char *p_id, const char *p_body, uint8_t important,
                    const char *p_effective)
{
    char payload[190];
    (void)snprintf(payload, sizeof(payload),
        "0,0,4,3,4,%u,2026-11-02T10:20,%s,2026-11-02T13:20,%u,0,4,3,%u,0",
        (unsigned int)important, p_effective, (unsigned int)strlen(p_id),
        (unsigned int)strlen(p_body));
    record("ALERT", payload);
    text_record(0U, p_id); text_record(2U, "RAIN");
    text_record(3U, "MET"); text_record(4U, p_body);
}

static weather_feed_snapshot_t snapshot(void)
{
    weather_feed_snapshot_t result;
    weather_feed_get_snapshot(s_tick, &result); return result;
}

int main(void)
{
    weather_feed_snapshot_t state;
    weather_feed_lease_t lease;
    clock_calendar_t now = {2026U, 11U, 2U, 1U, 13U, 20U, 0U};
    uint32_t revision;
    uint32_t attention;
    uint32_t saved_sequence;
    uint32_t saved_crc;
    char saved_command[WEATHER_FEED_LINE_MAX + 1U];
    char glyph[190];
    size_t i;
    size_t j;
    weather_feed_init(send_text); weather_feed_set_console(1U); s_tick = 100U;
    CHECK(weather_feed_command("WEATHER ignored", s_tick) == 0U);
    state = snapshot(); CHECK(state.available == 0U);
    begin(3U, 0U, WEATHER_ALERT_NONE, 0U); base();
    record("DAY", "0,2026-11-02,1,180,270,20");
    record("DAY", "1,2026-11-03,4,190,260,NA");
    record("DAY", "2,2026-11-04,0,200,280,0");
    finish("OK WDATA COMMIT"); state = snapshot();
    CHECK(state.available != 0U && state.fresh != 0U && state.city == WEATHER_CITY_ZHANJIANG);
    CHECK(state.alert_state == WEATHER_ALERT_NONE && state.attention_revision == 0U);
    revision = state.revision; CHECK(weather_feed_acquire(revision, &lease));
    CHECK(sizeof(*lease.p_batch) < 18000U);
    CHECK(lease.p_batch->live.temperature_tenths == 235);
    CHECK(lease.p_batch->wind_bft_min == 3U && lease.p_batch->wind_bft_max == 4U);
    CHECK(lease.p_batch->metrics.valid_mask == 63U);
    CHECK(lease.p_batch->metrics.value[WEATHER_METRIC_PRESSURE] == 1012);
    CHECK(lease.p_batch->days[1].rain_probability_valid == 0U);
    finish("OK WDATA COMMIT"); CHECK(snapshot().revision == revision);
    /* Leases survive one publication; reuse waits for the reader. */
    begin(0U, 1U, WEATHER_ALERT_ACTIVE, 0U); base(); alert("A1", "heavy rain", 1U, "-");
    finish("OK WDATA COMMIT"); state = snapshot(); attention = state.attention_revision;
    CHECK(state.alert_count == 1U && attention != 0U && state.alert_mask == 1U);
    command("WDATA BEGIN 900,1,0,0,2,2026-11-02T10:35,-,0", "ERR WDATA BUSY");
    CHECK(lease.p_batch->revision == revision); weather_feed_release(&lease);
    /* Changed poll/check ID alone never steals focus again. */
    begin(0U, 1U, WEATHER_ALERT_ACTIVE, 0U); base(); alert("A1", "heavy rain", 1U, "-");
    revision = snapshot().revision;
    saved_sequence = snapshot().commit_count;
    finish("OK WDATA COMMIT"); CHECK(snapshot().attention_revision == attention);
    CHECK(snapshot().revision == revision && snapshot().commit_count == saved_sequence + 1U);
    begin(0U, 1U, WEATHER_ALERT_ACTIVE, 0U); base(); alert("A1", "rain upgraded", 1U, "-");
    finish("OK WDATA COMMIT"); CHECK(snapshot().attention_revision != attention);
    attention = snapshot().attention_revision;
    /* Offline preserves warning body, last success, and focus identity. */
    begin(0U, 0U, WEATHER_ALERT_OFFLINE, 0U); base(); finish("OK WDATA COMMIT");
    state = snapshot(); CHECK(state.alert_state == WEATHER_ALERT_OFFLINE && state.alert_count == 1U);
    CHECK(state.attention_revision == attention);
    CHECK(weather_feed_acquire(state.revision, &lease));
    CHECK(strcmp(lease.p_batch->alerts[0].body, "rain upgraded") == 0);
    weather_feed_release(&lease);
    /* RTC expiry is bounded and never edits a pinned image. */
    CHECK(weather_feed_acquire(state.revision, &lease)); weather_feed_expire(&now);
    CHECK(snapshot().alert_count == 1U); weather_feed_release(&lease); weather_feed_expire(&now);
    CHECK(snapshot().alert_count == 0U && snapshot().attention_revision == 0U);
    begin(0U, 0U, WEATHER_ALERT_NONE, 0U); base(); finish("OK WDATA COMMIT");
    CHECK(snapshot().alert_state == WEATHER_ALERT_CLEARED);
    /* Missing field, sequence corruption and CRC failures preserve old batch. */
    revision = snapshot().revision;
    begin(0U, 0U, WEATHER_ALERT_NONE, 0U);
    record("NOW", "NA"); finish("ERR WDATA INCOMPLETE");
    CHECK(snapshot().revision == revision);
    record("METRICS", "NA,NA,NA,NA,NA,NA");
    (void)snprintf(saved_command, sizeof(saved_command), "WDATA COMMIT %lu,%lu,00000000",
        (unsigned long)s_id, (unsigned long)s_sequence);
    command(saved_command, "ERR WDATA CRC"); finish("OK WDATA COMMIT");
    CHECK(weather_feed_acquire(snapshot().revision, &lease));
    CHECK(lease.p_batch->live_valid == 0U && lease.p_batch->metrics.valid_mask == 0U);
    weather_feed_release(&lease);
    begin(0U, 0U, WEATHER_ALERT_NONE, 0U); record("NOW", "NA");
    memcpy(saved_command, s_command, sizeof(saved_command));
    saved_sequence = s_sequence; saved_crc = s_crc;
    command(saved_command, "OK WDATA"); CHECK(s_sequence == saved_sequence && s_crc == saved_crc);
    command("WDATA METRICS 999,1,NA,NA,NA,NA,NA,NA", "ERR WDATA TRANSACTION");
    command("WDATA CANCEL", "OK WDATA CANCEL"); CHECK(weather_feed_busy() == 0U);
    /* Invalid UTF8 and missing dynamic Chinese glyphs fail atomically. */
    begin(0U, 1U, WEATHER_ALERT_ACTIVE, 0U); base(); alert("A2", "\xE6\x9A\xB4", 0U, "-");
    finish("ERR WDATA INCOMPLETE"); command("WDATA CANCEL", "OK WDATA CANCEL");
    begin(0U, 1U, WEATHER_ALERT_ACTIVE, 1U); base(); alert("A2", "\xE6\x9A\xB4", 0U, "-");
    memcpy(glyph, "0,26292,", 8U);
    for (i = 8U; i < 178U; i++) { glyph[i] = '0'; } glyph[178] = '\0';
    record("GLYPH", glyph); finish("OK WDATA COMMIT");
    CHECK(weather_feed_acquire(snapshot().revision, &lease));
    CHECK(lease.p_batch->glyph_count == 1U && lease.p_batch->glyphs[0].codepoint == 26292U);
    weather_feed_release(&lease);
    begin(0U, 1U, WEATHER_ALERT_ACTIVE, 0U); base(); alert("A3", "\xC0\xAF", 0U, "-");
    finish("ERR WDATA INCOMPLETE"); command("WDATA CANCEL", "OK WDATA CANCEL");
    begin(0U, 1U, WEATHER_ALERT_ACTIVE, 0U); base(); alert("A5", "line 1\r\nline 2", 0U, "-");
    finish("OK WDATA COMMIT");
    CHECK(weather_feed_acquire(snapshot().revision, &lease));
    CHECK(strcmp(lease.p_batch->alerts[0].body, "line 1\r\nline 2") == 0);
    weather_feed_release(&lease);
    /* Pending important alert starts once and same content is then deduped. */
    begin(0U, 1U, WEATHER_ALERT_ACTIVE, 0U); base(); alert("A4", "future", 1U, "2026-11-02T11:00");
    finish("OK WDATA COMMIT"); CHECK(snapshot().attention_revision == 0U);
    now.hour = 11U; now.minute = 0U; weather_feed_expire(&now);
    attention = snapshot().attention_revision; CHECK(attention != 0U);
    weather_feed_expire(&now); CHECK(snapshot().attention_revision == attention);
    /* Timeouts and freshness follow unsigned monotonic ticks through wrap. */
    s_tick = UINT32_MAX - 1000U; begin(0U, 0U, WEATHER_ALERT_NONE, 0U);
    weather_feed_poll(s_tick + 59999U); CHECK(weather_feed_busy() != 0U);
    weather_feed_poll(s_tick + 60000U); CHECK(weather_feed_busy() == 0U);
    begin(0U, 0U, WEATHER_ALERT_NONE, 0U); base(); finish("OK WDATA COMMIT");
    weather_feed_get_snapshot(s_tick + 10800000U - 1U, &state); CHECK(state.fresh != 0U);
    weather_feed_get_snapshot(s_tick + 10800000U, &state); CHECK(state.fresh == 0U);
    begin(0U, 0U, WEATHER_ALERT_NONE, 0U); weather_feed_set_console(0U);
    CHECK(weather_feed_busy() == 0U); weather_feed_set_console(1U);
    /* Minimum command length cannot replay stale parser memory. */
    command("WDATA", "ERR WDATA ARGUMENT"); CHECK(weather_feed_busy() == 0U);
    command("WDATA BEGIN 1,1,0,0,2,2026-02-29T10:35:00,-,0", "ERR WDATA ARGUMENT");
    command("WDATA BEGIN 1,1,0,0,2,2026-11-02T10:35:60,-,0", "ERR WDATA ARGUMENT");
    command("WDATA BEGIN 1,1,0,4,1,2026-11-02T10:35:00,-,0", "ERR WDATA ARGUMENT");
    command("WDATA BEGIN 1,1,0,0,2,2026-11-02T10:35:00,-,129", "ERR WDATA ARGUMENT");
    /* Full resource envelope: 3 complete longest alerts and 128 dynamic glyphs. */
    memset(s_large_id, 'I', sizeof(s_large_id) - 1U);
    memset(s_large_title, 'T', sizeof(s_large_title) - 1U);
    memset(s_large_source, 'S', sizeof(s_large_source) - 1U);
    memset(s_large_body, 'B', sizeof(s_large_body) - 1U);
    memset(s_large_guide, 'G', sizeof(s_large_guide) - 1U);
    begin(0U, WEATHER_FEED_MAX_ALERTS, WEATHER_ALERT_ACTIVE, WEATHER_FEED_MAX_GLYPHS);
    base();
    for (i = 0U; i < WEATHER_FEED_MAX_ALERTS; i++) {
        char payload[190];
        s_large_id[0] = (char)('A' + i);
        (void)snprintf(payload, sizeof(payload),
            "%u,0,4,3,4,0,2026-11-02T10:20:00,-,-,63,0,127,95,768,512", (unsigned int)i);
        record("ALERT", payload);
        text_chunks((uint8_t)i, 0U, s_large_id); text_chunks((uint8_t)i, 2U, s_large_title);
        text_chunks((uint8_t)i, 3U, s_large_source); text_chunks((uint8_t)i, 4U, s_large_body);
        text_chunks((uint8_t)i, 5U, s_large_guide);
    }
    for (i = 0U; i < WEATHER_FEED_MAX_GLYPHS; i++) {
        int prefix = snprintf(glyph, sizeof(glyph), "%u,%u,", (unsigned int)i,
            (unsigned int)(0x4E00U + i));
        CHECK(prefix > 0);
        for (j = 0U; j < WEATHER_FEED_GLYPH_BYTES * 2U; j++) {
            glyph[(size_t)prefix + j] = 'F';
        }
        glyph[(size_t)prefix + WEATHER_FEED_GLYPH_BYTES * 2U - 1U] = '0';
        glyph[(size_t)prefix + WEATHER_FEED_GLYPH_BYTES * 2U] = '\0';
        record("GLYPH", glyph);
    }
    finish("OK WDATA COMMIT"); state = snapshot(); CHECK(state.alert_count == 3U);
    CHECK(weather_feed_acquire(state.revision, &lease));
    CHECK(lease.p_batch->glyph_count == WEATHER_FEED_MAX_GLYPHS);
    CHECK(strlen(lease.p_batch->alerts[2].body) == WEATHER_FEED_BODY_MAX);
    CHECK(strlen(lease.p_batch->alerts[2].guide) == WEATHER_FEED_GUIDE_MAX);
    CHECK(lease.p_batch->glyphs[127].bitmap[84] == 0xF0U);
    weather_feed_release(&lease);
    printf("PASS: weather feed, %lu command assertions; batch=%lu bytes, glyph=%lu bytes\n",
        (unsigned long)s_tests, (unsigned long)sizeof(weather_feed_batch_t),
        (unsigned long)sizeof(weather_feed_glyph_t));
    return 0;
}
