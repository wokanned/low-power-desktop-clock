#include "display.h"
#include "clock_service.h"
#include "epd_bringup.h"
#include "epd_clock_view.h"
#include "epd_weather_pages.h"
#include "forecast.h"
#include "weather_feed.h"
#include "cmsis_os.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static clock_view_model_t s_clock;
static uint32_t s_tick;
static uint32_t s_id;
static uint32_t s_sequence;
static uint32_t s_crc;
static uint32_t s_commands;
static char s_command[WEATHER_FEED_LINE_MAX + 1U];
static char s_output[256];
static uint8_t s_frame[EPD_CLOCK_VIEW_BW_BYTES + 2U];
static uint8_t s_previous_frame[EPD_CLOCK_VIEW_BW_BYTES];
static const char *s_output_directory;

uint32_t xTaskGetTickCount(void) { return s_tick; }
void clock_service_get_model(clock_view_model_t *p_model) { *p_model = s_clock; }
void clock_service_note_sync_success(void) { }
void Debug_Log(const char *p_text) { (void)p_text; }
epd_gdey029t94_status_t epd_bringup_run_clock(void) { return EPD_GDEY029T94_OK; }
epd_gdey029t94_status_t epd_bringup_get_last_status(void) { return EPD_GDEY029T94_OK; }
uint32_t epd_bringup_get_completed_frames(void) { return 0U; }
osThreadId_t osThreadNew(osThreadFunc_t function, void *p_argument,
                        const osThreadAttr_t *p_attributes)
{
    (void)function; (void)p_argument; (void)p_attributes;
    return (osThreadId_t)1;
}
osThreadId_t osThreadGetId(void) { return (osThreadId_t)1; }
int32_t osThreadSuspend(osThreadId_t thread) { (void)thread; return 0; }
int32_t osDelay(uint32_t ticks) { s_tick += ticks; return 0; }

static uint8_t s_send(const uint8_t *p_data, uint16_t length)
{
    assert(length < sizeof(s_output));
    memcpy(s_output, p_data, length); s_output[length] = '\0';
    return 1U;
}

static uint32_t s_crc_text(uint32_t crc, const char *p_text)
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

static void s_send_command(const char *p_text, const char *p_expected)
{
    s_output[0] = '\0';
    assert(weather_feed_command(p_text, s_tick) == 1U);
    if (strstr(s_output, p_expected) == NULL) {
        fprintf(stderr, "%s\nexpected %s; got %s\n", p_text, p_expected, s_output);
        exit(1);
    }
    s_commands++;
}

static void s_record(const char *p_verb, const char *p_payload)
{
    int32_t length = snprintf(s_command, sizeof(s_command), "WDATA %s %lu,%lu,%s",
        p_verb, (unsigned long)s_id, (unsigned long)s_sequence, p_payload);
    assert(length > 0 && (size_t)length < sizeof(s_command));
    s_send_command(s_command, "OK WDATA");
    s_crc = s_crc_text(s_crc, s_command); s_crc = s_crc_text(s_crc, "\n");
    s_sequence++;
}

static void s_begin(uint8_t alerts, weather_alert_state_t state)
{
    s_id++; s_sequence = 0U; s_crc = UINT32_MAX;
    (void)snprintf(s_command, sizeof(s_command),
        "WDATA BEGIN %lu,1,3,%u,%u,2026-11-02T10:35,2026-11-02T10:35,0",
        (unsigned long)s_id, (unsigned int)alerts, (unsigned int)state);
    s_send_command(s_command, "OK WDATA BEGIN");
    s_record("NOW", "1,235,180,270,72,2,50,3,4,2026-11-02T10:30");
    s_record("METRICS", "251,60,42,0,1012,180");
    s_record("DAY", "0,2026-11-02,1,180,270,20");
    s_record("DAY", "1,2026-11-03,4,190,260,NA");
    s_record("DAY", "2,2026-11-04,0,200,280,0");
}

static void s_text(uint8_t alert_index, uint8_t field, const char *p_text)
{
    static const char s_digits[] = "0123456789ABCDEF";
    char payload[190];
    size_t length = strlen(p_text);
    size_t i;
    int32_t prefix;
    assert(length <= 80U);
    prefix = snprintf(payload, sizeof(payload), "%u,%u,0,", (unsigned int)alert_index,
        (unsigned int)field);
    assert(prefix > 0);
    for (i = 0U; i < length; i++) {
        payload[(size_t)prefix + i * 2U] = s_digits[(uint8_t)p_text[i] >> 4];
        payload[(size_t)prefix + i * 2U + 1U] = s_digits[(uint8_t)p_text[i] & 15U];
    }
    payload[(size_t)prefix + length * 2U] = '\0';
    s_record("TEXT", payload);
}

static void s_alert_at(uint8_t index, const char *p_id, const char *p_body, uint8_t important)
{
    char payload[190];
    (void)snprintf(payload, sizeof(payload),
        "%u,0,4,3,4,%u,2026-11-02T10:20,-,2026-11-02T13:20,%u,0,4,3,%u,0",
        (unsigned int)index, (unsigned int)important, (unsigned int)strlen(p_id),
        (unsigned int)strlen(p_body));
    s_record("ALERT", payload);
    s_text(index, 0U, p_id); s_text(index, 2U, "RAIN"); s_text(index, 3U, "MET");
    s_text(index, 4U, p_body);
}

static void s_alert(const char *p_body, uint8_t important)
{
    s_alert_at(0U, "A1", p_body, important);
}

static void s_commit(const char *p_expected)
{
    (void)snprintf(s_command, sizeof(s_command), "WDATA COMMIT %lu,%lu,%08lX",
        (unsigned long)s_id, (unsigned long)s_sequence,
        (unsigned long)(s_crc ^ UINT32_MAX));
    s_send_command(s_command, p_expected);
}

static display_snapshot_t s_snapshot(void)
{
    display_snapshot_t snapshot;
    display_get_snapshot(s_tick, &snapshot);
    return snapshot;
}

static void s_export(const char *p_name)
{
    char path[512];
    FILE *p_file;
    uint16_t x;
    uint16_t y;
    uint16_t native_x;
    uint16_t native_y;
    if (s_output_directory == NULL) { return; }
    (void)snprintf(path, sizeof(path), "%s/%s.pgm", s_output_directory, p_name);
    p_file = fopen(path, "wb"); assert(p_file != NULL);
    fprintf(p_file, "P5\n296 128\n255\n");
    for (y = 0U; y < 128U; y++) {
        for (x = 0U; x < 296U; x++) {
            native_x = (uint16_t)(127U - y); native_y = (uint16_t)(295U - x);
            fputc((s_frame[1U + native_y * 16U + native_x / 8U] &
                (uint8_t)(0x80U >> (native_x % 8U))) != 0U ? 255 : 0, p_file);
        }
    }
    assert(fclose(p_file) == 0);
}

static void s_render(weather_page_kind_t page, const char *p_name)
{
    display_snapshot_t snapshot = s_snapshot();
    weather_feed_lease_t lease;
    assert(weather_feed_acquire(snapshot.feed.revision, &lease));
    memset(s_frame, 0xA5, sizeof(s_frame));
    assert(epd_weather_pages_draw_bw_for_date(s_frame + 1U, EPD_CLOCK_VIEW_BW_BYTES,
        lease.p_batch, page, snapshot.alert_index, snapshot.text_page,
        snapshot.feed.fresh, &snapshot.clock.calendar) != 0U);
    weather_feed_release(&lease);
    assert(s_frame[0] == 0xA5U && s_frame[sizeof(s_frame) - 1U] == 0xA5U);
    s_export(p_name);
}

static void s_test_rotation(void)
{
    static const display_page_t s_pages[] = {
        DISPLAY_PAGE_WEATHER, DISPLAY_PAGE_FORECAST, DISPLAY_PAGE_DATA, DISPLAY_PAGE_CLOCK
    };
    display_snapshot_t snapshot;
    uint8_t i;
    uint8_t update;
    assert(display_select_page(DISPLAY_PAGE_CLOCK));
    s_begin(0U, WEATHER_ALERT_NONE);
    snapshot = s_snapshot(); assert(snapshot.feed.available == 0U && snapshot.page == DISPLAY_PAGE_CLOCK);
    s_commit("OK WDATA COMMIT");
    snapshot = s_snapshot(); assert(snapshot.feed.available != 0U && snapshot.page == DISPLAY_PAGE_CLOCK);
    for (i = 0U; i < 4U; i++) {
        for (update = 0U; update < (i == 0U ? 6U : 2U); update++) {
            s_tick += 30000U;
            s_begin(0U, WEATHER_ALERT_NONE); s_commit("OK WDATA COMMIT");
        }
        snapshot = s_snapshot(); assert(snapshot.page == s_pages[i]);
    }
    assert(display_page_command("PAGE LIVE")); s_render(WEATHER_PAGE_LIVE, "live");
    memcpy(s_previous_frame, s_frame + 1U, sizeof(s_previous_frame));
    assert(display_page_command("PAGE FORECAST")); s_render(WEATHER_PAGE_FORECAST, "forecast");
    assert(memcmp(s_previous_frame, s_frame + 1U, sizeof(s_previous_frame)) != 0);
    memcpy(s_previous_frame, s_frame + 1U, sizeof(s_previous_frame));
    s_clock.calendar.day = 3U; s_clock.calendar.weekday = 2U;
    s_render(WEATHER_PAGE_FORECAST, "forecast_next_date");
    assert(memcmp(s_previous_frame, s_frame + 1U, sizeof(s_previous_frame)) != 0);
    s_clock.calendar.day = 2U; s_clock.calendar.weekday = 1U;
    assert(display_page_command("PAGE DATA")); s_render(WEATHER_PAGE_DATA, "data");
}

static void s_test_attention(void)
{
    display_snapshot_t snapshot;
    uint32_t attention;
    uint32_t origin;
    weather_feed_lease_t lease;
    assert(display_select_page(DISPLAY_PAGE_CLOCK));
    s_begin(1U, WEATHER_ALERT_ACTIVE); s_alert("heavy rain", 1U);
    assert(s_snapshot().page == DISPLAY_PAGE_CLOCK);
    s_commit("OK WDATA COMMIT");
    snapshot = s_snapshot(); attention = snapshot.feed.attention_revision;
    assert(snapshot.page == DISPLAY_PAGE_ALERT && attention != 0U);
    s_render(WEATHER_PAGE_ALERT, "alert");
    origin = s_tick;
    s_tick = origin + 59999U; assert(s_snapshot().page == DISPLAY_PAGE_ALERT);
    s_tick = origin + 60000U; assert(s_snapshot().page == DISPLAY_PAGE_CLOCK);
    s_tick++;
    s_begin(1U, WEATHER_ALERT_ACTIVE); s_alert("heavy rain", 1U); s_commit("OK WDATA COMMIT");
    snapshot = s_snapshot();
    assert(snapshot.feed.attention_revision == attention && snapshot.page == DISPLAY_PAGE_CLOCK);
    s_begin(1U, WEATHER_ALERT_ACTIVE); s_alert("rain updated", 1U); s_commit("OK WDATA COMMIT");
    snapshot = s_snapshot();
    assert(snapshot.feed.attention_revision != attention && snapshot.page == DISPLAY_PAGE_ALERT);
    assert(display_page_command("PAGE DATA"));
    assert(s_snapshot().page == DISPLAY_PAGE_DATA);
    assert(display_page_command("PAGE TEXT 1 1")); s_render(WEATHER_PAGE_TEXT, "text");
    assert(weather_feed_acquire(0U, &lease));
    assert(epd_weather_pages_text_count(lease.p_batch, 0U) == 2U);
    weather_feed_release(&lease);
    assert(display_page_command("PAGE SOURCE 1")); s_render(WEATHER_PAGE_SOURCE, "source");
    assert(!display_page_command("PAGE TEXT 1 999"));
    assert(!display_page_command("PAGE ALERT 0"));
    assert(!display_page_command("PAGE SOURCE 4"));
}

static void s_test_atomic_reject_and_expiry(void)
{
    display_snapshot_t before = s_snapshot();
    display_snapshot_t after;
    clock_calendar_t now = {2026U, 11U, 2U, 1U, 13U, 20U, 0U};
    s_begin(1U, WEATHER_ALERT_ACTIVE); s_alert("\xE6\x9A\xB4", 0U);
    s_commit("ERR WDATA INCOMPLETE");
    after = s_snapshot();
    assert(after.feed.revision == before.feed.revision && after.page == before.page);
    s_send_command("WDATA CANCEL", "OK WDATA CANCEL");
    s_begin(1U, WEATHER_ALERT_ACTIVE); s_alert("\xC0\xAF", 0U);
    s_commit("ERR WDATA INCOMPLETE");
    assert(s_snapshot().feed.revision == before.feed.revision);
    s_send_command("WDATA CANCEL", "OK WDATA CANCEL");
    assert(display_page_command("PAGE DATA"));
    s_begin(1U, WEATHER_ALERT_ACTIVE); s_alert("new rain update", 1U); s_commit("OK WDATA COMMIT");
    assert(s_snapshot().page == DISPLAY_PAGE_ALERT);
    weather_feed_expire(&now);
    after = s_snapshot();
    assert(after.feed.alert_mask == 0U && after.feed.alert_state == WEATHER_ALERT_CLEARED);
    assert(after.page == DISPLAY_PAGE_DATA);
    s_tick += 10800000U;
    assert(s_snapshot().feed.fresh == 0U);
    assert(display_page_command("PAGE DATA")); s_render(WEATHER_PAGE_DATA, "data_stale");
}

static void s_test_reordered_attention(void)
{
    display_snapshot_t before;
    display_snapshot_t after;
    uint32_t origin;
    assert(display_select_page(DISPLAY_PAGE_CLOCK));
    s_begin(2U, WEATHER_ALERT_ACTIVE);
    s_alert_at(0U, "A1", "ordered important rain", 1U);
    s_alert_at(1U, "B1", "general rain", 0U);
    s_commit("OK WDATA COMMIT");
    before = s_snapshot();
    assert(before.page == DISPLAY_PAGE_ALERT && before.alert_index == 0U);
    origin = s_tick;
    s_tick += 30000U;
    s_begin(2U, WEATHER_ALERT_ACTIVE);
    s_alert_at(0U, "B1", "general rain", 0U);
    s_alert_at(1U, "A1", "ordered important rain", 1U);
    s_commit("OK WDATA COMMIT");
    after = s_snapshot();
    assert(after.feed.attention_revision == before.feed.attention_revision);
    assert(after.page == DISPLAY_PAGE_ALERT && after.alert_index == 1U);
    s_tick = origin + 60000U;
    assert(s_snapshot().page == DISPLAY_PAGE_CLOCK);
}

int main(int argc, char **argv)
{
    if (argc > 1) { s_output_directory = argv[1]; }
    s_clock.calendar = (clock_calendar_t){2026U, 11U, 2U, 1U, 10U, 35U, 0U};
    s_clock.calendar_state = CLOCK_DATA_READY; s_clock.startup_ready = 1U;
    s_clock.format = CLOCK_FORMAT_24; s_tick = 100U;
    weather_init(); forecast_init(); weather_feed_init(s_send); weather_feed_set_console(1U);
    s_test_rotation(); s_test_attention(); s_test_reordered_attention();
    s_test_atomic_reject_and_expiry();
    printf("PASS: %lu WDATA commands, atomic publication, clock/live/forecast/data rotation, repeated updates, one-shot attention/return/manual exit, page bounds, UTF8/glyph rejection, expiry and real C page rendering\n", (unsigned long)s_commands);
    return 0;
}
