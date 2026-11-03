#include "weather_feed.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define FEED_SLOT_COUNT 2U
#define FEED_TIMEOUT_TICKS ((uint32_t)configTICK_RATE_HZ * 60UL)
#define FEED_EXPIRY_TICKS ((uint32_t)configTICK_RATE_HZ * 10800UL)
#define FEED_HALF_RANGE 0x80000000UL
#define FEED_FIELD_COUNT 6U
#define FEED_TOKEN_COUNT 20U
#define FEED_TEXT_CHUNK_MAX 80U
#define FEED_NO_SLOT 255U
#define FEED_CRC_POLYNOMIAL 0xEDB88320UL

static weather_feed_batch_t s_batches[FEED_SLOT_COUNT];
static uint16_t s_pins[FEED_SLOT_COUNT];
static uint8_t s_active = FEED_NO_SLOT;
static uint8_t s_staging = FEED_NO_SLOT;
static uint8_t s_console;
static uint8_t s_have_now;
static uint8_t s_have_metrics;
static uint8_t s_have_days;
static uint8_t s_have_alerts;
static uint16_t s_have_glyphs;
static uint16_t s_expected[WEATHER_FEED_MAX_ALERTS][FEED_FIELD_COUNT];
static uint16_t s_written[WEATHER_FEED_MAX_ALERTS][FEED_FIELD_COUNT];
static uint32_t s_sequence;
static uint32_t s_crc;
static uint32_t s_activity;
static uint32_t s_published_tick;
static uint32_t s_revision;
static uint32_t s_commit_count;
static uint32_t s_attention_revision;
static uint8_t s_attention_index;
static char s_attention_id[WEATHER_FEED_ID_MAX + 1U];
static uint8_t s_expired;
static uint32_t s_last_id;
static uint32_t s_last_crc;
static uint32_t s_last_sequence;
static char s_previous[WEATHER_FEED_LINE_MAX + 1U];
static char s_line[WEATHER_FEED_LINE_MAX + 1U];
static char *s_fields[FEED_TOKEN_COUNT];
static char s_reply[96];
static weather_feed_send_fn s_send;

static uint32_t s_crc_update(uint32_t crc, const uint8_t *p_data, size_t length)
{
    size_t i;
    uint8_t bit;
    for (i = 0U; i < length; i++) {
        crc ^= p_data[i];
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1) ^ ((crc & 1U) != 0U ? FEED_CRC_POLYNOMIAL : 0U);
        }
    }
    return crc;
}

static void s_send_text(const char *p_text)
{
    if (s_console != 0U && s_send != NULL) {
        (void)s_send((const uint8_t *)p_text, (uint16_t)strlen(p_text));
    }
}

static void s_ack(uint32_t id, uint32_t sequence)
{
    (void)snprintf(s_reply, sizeof(s_reply), "OK WDATA %lu,%lu\r\n",
        (unsigned long)id, (unsigned long)sequence);
    s_send_text(s_reply);
}

static uint8_t s_unsigned(const char *p_text, uint32_t maximum, uint32_t *p_value)
{
    uint32_t value = 0U;
    uint32_t digit;
    if (p_text == NULL || *p_text == '\0') { return 0U; }
    while (*p_text != '\0') {
        if (*p_text < '0' || *p_text > '9') { return 0U; }
        digit = (uint32_t)(*p_text - '0');
        if (digit > maximum || value > (maximum - digit) / 10U) { return 0U; }
        value = value * 10U + digit;
        p_text++;
    }
    *p_value = value;
    return 1U;
}

static uint8_t s_signed(const char *p_text, int32_t minimum, int32_t maximum,
                         int32_t *p_value)
{
    uint32_t value;
    uint8_t negative = (uint8_t)(*p_text == '-');
    if (negative != 0U && minimum >= 0) { return 0U; }
    if (negative != 0U) { p_text++; }
    if (!s_unsigned(p_text, (uint32_t)(negative != 0U ? -minimum : maximum), &value)) {
        return 0U;
    }
    *p_value = negative != 0U ? -(int32_t)value : (int32_t)value;
    return (uint8_t)(*p_value >= minimum && *p_value <= maximum);
}

static uint8_t s_hex_digit(char value)
{
    if (value >= '0' && value <= '9') { return (uint8_t)(value - '0'); }
    if (value >= 'a' && value <= 'f') { return (uint8_t)(value - 'a' + 10); }
    if (value >= 'A' && value <= 'F') { return (uint8_t)(value - 'A' + 10); }
    return 255U;
}

static uint8_t s_hex(const char *p_text, uint8_t *p_data, uint16_t length,
                      uint8_t allow_zero)
{
    uint16_t i;
    uint8_t high;
    uint8_t low;
    if (strlen(p_text) != (size_t)length * 2U) { return 0U; }
    for (i = 0U; i < length; i++) {
        high = s_hex_digit(p_text[i * 2U]);
        low = s_hex_digit(p_text[i * 2U + 1U]);
        if (high > 15U || low > 15U ||
            (allow_zero == 0U && high == 0U && low == 0U)) { return 0U; }
    }
    for (i = 0U; i < length; i++) {
        p_data[i] = (uint8_t)((s_hex_digit(p_text[i * 2U]) << 4) |
                             s_hex_digit(p_text[i * 2U + 1U]));
    }
    return 1U;
}

static uint8_t s_hex_crc(const char *p_text, uint32_t *p_crc)
{
    uint8_t i;
    uint8_t digit;
    uint32_t crc = 0U;
    if (strlen(p_text) != 8U) { return 0U; }
    for (i = 0U; i < 8U; i++) {
        digit = s_hex_digit(p_text[i]);
        if (digit > 15U) { return 0U; }
        crc = (crc << 4) | digit;
    }
    *p_crc = crc;
    return 1U;
}

static uint8_t s_date(const char *p_text, clock_calendar_t *p_date, uint8_t stamp)
{
    static const uint8_t s_positions[6] = {0U, 5U, 8U, 11U, 14U, 17U};
    uint16_t values[6] = {0U, 0U, 0U, 0U, 0U, 0U};
    uint8_t i;
    uint8_t j;
    uint8_t digits;
    size_t length = strlen(p_text);
    uint8_t count = stamp != 0U ? (length == 19U ? 6U : 5U) : 3U;
    if ((stamp != 0U ? (length != 16U && length != 19U) : length != 10U) ||
        p_text[4] != '-' || p_text[7] != '-' ||
        (stamp != 0U && (p_text[10] != 'T' || p_text[13] != ':' ||
         (length == 19U && p_text[16] != ':')))) { return 0U; }
    for (i = 0U; i < count; i++) {
        digits = i == 0U ? 4U : 2U;
        for (j = 0U; j < digits; j++) {
            if (p_text[s_positions[i] + j] < '0' ||
                p_text[s_positions[i] + j] > '9') { return 0U; }
            values[i] = (uint16_t)(values[i] * 10U +
                (uint16_t)(p_text[s_positions[i] + j] - '0'));
        }
    }
    memset(p_date, 0, sizeof(*p_date));
    p_date->year = values[0]; p_date->month = (uint8_t)values[1];
    p_date->day = (uint8_t)values[2]; p_date->hour = (uint8_t)values[3];
    p_date->minute = (uint8_t)values[4];
    p_date->second = (uint8_t)values[5];
    p_date->weekday = clock_calendar_weekday(p_date->year, p_date->month, p_date->day);
    return clock_calendar_valid(p_date);
}

static uint8_t s_stamp_valid(const char *p_text)
{
    clock_calendar_t date;
    return (uint8_t)(strcmp(p_text, "-") == 0 || s_date(p_text, &date, 1U) != 0U);
}

static void s_stamp_copy(char *p_target, const char *p_source)
{
    if (strcmp(p_source, "-") == 0) { p_target[0] = '\0'; }
    else if (strlen(p_source) == 16U) {
        memcpy(p_target, p_source, 16U); memcpy(p_target + 16U, ":00", 4U);
    } else { memcpy(p_target, p_source, WEATHER_FEED_STAMP_MAX + 1U); }
}

static uint8_t s_split(char *p_text)
{
    uint8_t count = 0U;
    char *p_start = p_text;
    for (;;) {
        if (count >= FEED_TOKEN_COUNT || *p_start == '\0') { return 0U; }
        s_fields[count++] = p_start;
        while (*p_text != ',' && *p_text != '\0') { p_text++; }
        if (*p_text == '\0') { return count; }
        *p_text++ = '\0'; p_start = p_text;
    }
}

static char *s_text_field(weather_feed_alert_t *p_alert, uint8_t field,
                           uint16_t *p_capacity)
{
    switch (field) {
    case 0U: *p_capacity = WEATHER_FEED_ID_MAX; return p_alert->id;
    case 1U: *p_capacity = WEATHER_FEED_ID_MAX; return p_alert->replaces;
    case 2U: *p_capacity = WEATHER_FEED_TITLE_MAX; return p_alert->title;
    case 3U: *p_capacity = WEATHER_FEED_SOURCE_MAX; return p_alert->source;
    case 4U: *p_capacity = WEATHER_FEED_BODY_MAX; return p_alert->body;
    default: *p_capacity = WEATHER_FEED_GUIDE_MAX; return p_alert->guide;
    }
}

/* Strict UTF-8, no controls, overlong encodings, surrogate or >U+10FFFF. */
static uint8_t s_next_codepoint(const uint8_t **pp_text, uint32_t *p_codepoint)
{
    const uint8_t *p_text = *pp_text;
    uint32_t codepoint;
    uint32_t minimum;
    uint8_t count;
    uint8_t i;
    if (*p_text < 0x80U) {
        if ((*p_text < 0x20U && *p_text != '\n' && *p_text != '\r') ||
            *p_text == 0x7FU) { return 0U; }
        *p_codepoint = *p_text; *pp_text = p_text + 1U; return 1U;
    }
    if (*p_text >= 0xC2U && *p_text <= 0xDFU) {
        codepoint = (uint32_t)(*p_text & 0x1FU); count = 1U; minimum = 0x80U;
    } else if (*p_text >= 0xE0U && *p_text <= 0xEFU) {
        codepoint = (uint32_t)(*p_text & 0x0FU); count = 2U; minimum = 0x800U;
    } else if (*p_text >= 0xF0U && *p_text <= 0xF4U) {
        codepoint = (uint32_t)(*p_text & 0x07U); count = 3U; minimum = 0x10000U;
    } else { return 0U; }
    p_text++;
    for (i = 0U; i < count; i++) {
        if ((*p_text & 0xC0U) != 0x80U) { return 0U; }
        codepoint = (codepoint << 6) | (uint32_t)(*p_text++ & 0x3FU);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFUL ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) { return 0U; }
    *p_codepoint = codepoint; *pp_text = p_text; return 1U;
}

static uint8_t s_text_valid(const char *p_text, const weather_feed_batch_t *p_batch,
                            uint8_t need_glyphs, uint8_t allow_breaks)
{
    const uint8_t *p_cursor = (const uint8_t *)p_text;
    uint32_t codepoint;
    uint16_t i;
    while (*p_cursor != 0U) {
        if (s_next_codepoint(&p_cursor, &codepoint) == 0U) { return 0U; }
        if (codepoint < 32U && allow_breaks == 0U) { return 0U; }
        if (need_glyphs == 0U || codepoint <= 126U || codepoint == 176U) { continue; }
        for (i = 0U; i < p_batch->glyph_count; i++) {
            if (p_batch->glyphs[i].codepoint == codepoint) { break; }
        }
        if (i == p_batch->glyph_count) { return 0U; }
    }
    return 1U;
}

static uint8_t s_begin(uint8_t count, uint32_t now_tick)
{
    uint32_t values[6];
    uint8_t slot;
    if (count != 8U || !s_unsigned(s_fields[0], UINT32_MAX, &values[0]) ||
        values[0] == 0U || !s_unsigned(s_fields[1], 1U, &values[1]) ||
        !s_unsigned(s_fields[2], WEATHER_FEED_MAX_DAYS, &values[2]) ||
        !s_unsigned(s_fields[3], WEATHER_FEED_MAX_ALERTS, &values[3]) ||
        !s_unsigned(s_fields[4], WEATHER_ALERT_CLEARED, &values[4]) ||
        !s_stamp_valid(s_fields[5]) || !s_stamp_valid(s_fields[6]) ||
        !s_unsigned(s_fields[7], WEATHER_FEED_MAX_GLYPHS, &values[5]) ||
        (values[4] == WEATHER_ALERT_ACTIVE && values[3] == 0U) ||
        (values[4] != WEATHER_ALERT_ACTIVE && values[3] != 0U) ||
        ((values[4] == WEATHER_ALERT_ACTIVE || values[4] == WEATHER_ALERT_NONE ||
          values[4] == WEATHER_ALERT_CLEARED) && strcmp(s_fields[5], "-") == 0)) {
        return 0U;
    }
    taskENTER_CRITICAL();
    slot = s_active == 0U ? 1U : 0U;
    if (s_staging != FEED_NO_SLOT || s_pins[slot] != 0U) {
        taskEXIT_CRITICAL(); s_send_text("ERR WDATA BUSY\r\n"); return 2U;
    }
    s_staging = slot;
    s_batches[slot].revision = 0U;
    taskEXIT_CRITICAL();
    memset(&s_batches[slot], 0, sizeof(s_batches[slot]));
    s_batches[slot].id = values[0]; s_batches[slot].city = (weather_city_t)values[1];
    s_batches[slot].day_count = (uint8_t)values[2];
    s_batches[slot].alert_count = (uint8_t)values[3];
    s_batches[slot].alert_state = (weather_alert_state_t)values[4];
    s_stamp_copy(s_batches[slot].checked, s_fields[5]);
    s_stamp_copy(s_batches[slot].last_success, s_fields[6]);
    s_batches[slot].glyph_count = (uint16_t)values[5];
    s_have_now = 0U; s_have_metrics = 0U; s_have_days = 0U;
    s_have_alerts = 0U; s_have_glyphs = 0U; s_sequence = 0U;
    s_crc = UINT32_MAX; s_activity = now_tick; s_previous[0] = '\0';
    memset(s_expected, 0, sizeof(s_expected)); memset(s_written, 0, sizeof(s_written));
    s_send_text("OK WDATA BEGIN\r\n");
    return 1U;
}

static uint8_t s_now(uint8_t count, weather_feed_batch_t *p_batch)
{
    int32_t values[9];
    uint8_t i;
    clock_calendar_t source;
    if (s_have_now != 0U) { return 0U; }
    if (count == 3U && strcmp(s_fields[2], "NA") == 0) {
        s_have_now = 1U; return 1U;
    }
    if (count != 12U || strlen(s_fields[11]) != WEATHER_SOURCE_TIME_LENGTH ||
        !s_date(s_fields[11], &source, 1U)) { return 0U; }
    for (i = 0U; i < 9U; i++) {
        if (!s_signed(s_fields[i + 2U], i >= 1U && i <= 3U ? -999 : 0,
            i >= 1U && i <= 3U ? 999 : (i == 4U ? 100 :
            (i == 5U ? 8 : (i >= 7U ? 12 : (i == 0U ? 13 : 999)))), &values[i])) {
            return 0U;
        }
    }
    if (values[3] < values[2] || (values[5] == 0 && values[6] != 0) ||
        values[7] > values[8]) { return 0U; }
    p_batch->live.city = p_batch->city;
    p_batch->live.condition = (uint8_t)values[0];
    p_batch->live.temperature_tenths = (int16_t)values[1];
    p_batch->live.minimum_tenths = (int16_t)values[2];
    p_batch->live.maximum_tenths = (int16_t)values[3];
    p_batch->live.humidity = (uint8_t)values[4];
    p_batch->live.wind_direction = (uint8_t)values[5];
    p_batch->live.wind_speed_tenths = (uint16_t)values[6];
    p_batch->wind_bft_valid = 1U; p_batch->wind_bft_min = (uint8_t)values[7];
    p_batch->wind_bft_max = (uint8_t)values[8];
    memcpy(p_batch->live.source_time, s_fields[11], WEATHER_SOURCE_TIME_LENGTH + 1U);
    p_batch->live_valid = 1U; s_have_now = 1U; return 1U;
}

static uint8_t s_metrics(uint8_t count, weather_feed_batch_t *p_batch)
{
    static const int32_t s_minimum[WEATHER_METRIC_COUNT] = {-999, 0, 0, 0, 100, 0};
    static const int32_t s_maximum[WEATHER_METRIC_COUNT] = {999, 250, 500, 9999, 1200, 9999};
    weather_feed_metrics_t metrics;
    uint8_t i;
    if (count != 8U || s_have_metrics != 0U) { return 0U; }
    memset(&metrics, 0, sizeof(metrics));
    for (i = 0U; i < WEATHER_METRIC_COUNT; i++) {
        if (strcmp(s_fields[i + 2U], "NA") == 0) { continue; }
        if (!s_signed(s_fields[i + 2U], s_minimum[i], s_maximum[i], &metrics.value[i])) {
            return 0U;
        }
        metrics.valid_mask |= (uint8_t)(1U << i);
    }
    p_batch->metrics = metrics; s_have_metrics = 1U; return 1U;
}

static uint8_t s_day(uint8_t count, weather_feed_batch_t *p_batch)
{
    weather_feed_day_t day;
    clock_calendar_t next;
    uint32_t index;
    uint32_t condition;
    uint32_t rain = 0U;
    int32_t minimum;
    int32_t maximum;
    memset(&day, 0, sizeof(day));
    if (count != 8U || !s_unsigned(s_fields[2], WEATHER_FEED_MAX_DAYS - 1U, &index) ||
        index != s_have_days || index >= p_batch->day_count ||
        !s_date(s_fields[3], &day.date, 0U) ||
        !s_unsigned(s_fields[4], 13U, &condition) ||
        !s_signed(s_fields[5], -999, 999, &minimum) ||
        !s_signed(s_fields[6], -999, 999, &maximum) || maximum < minimum ||
        (strcmp(s_fields[7], "NA") != 0 && !s_unsigned(s_fields[7], 100U, &rain))) {
        return 0U;
    }
    if (index > 0U) {
        next = p_batch->days[index - 1U].date; clock_calendar_add_seconds(&next, 86400U);
        if (next.year != day.date.year || next.month != day.date.month ||
            next.day != day.date.day) { return 0U; }
    }
    day.condition = (uint8_t)condition; day.minimum_tenths = (int16_t)minimum;
    day.maximum_tenths = (int16_t)maximum; day.rain_probability = (uint8_t)rain;
    day.rain_probability_valid = (uint8_t)(strcmp(s_fields[7], "NA") != 0);
    p_batch->days[index] = day; s_have_days++; return 1U;
}

static uint8_t s_alert(uint8_t count, weather_feed_batch_t *p_batch)
{
    uint32_t values[12];
    uint8_t i;
    uint16_t capacity;
    weather_feed_alert_t *p_alert;
    if (count != 17U || !s_unsigned(s_fields[2], WEATHER_FEED_MAX_ALERTS - 1U, &values[0]) ||
        values[0] != s_have_alerts || values[0] >= p_batch->alert_count ||
        !s_unsigned(s_fields[3], WEATHER_ALERT_OTHER, &values[1]) ||
        !s_unsigned(s_fields[4], WEATHER_ALERT_RED, &values[2]) ||
        !s_unsigned(s_fields[5], 4U, &values[3]) ||
        !s_unsigned(s_fields[6], 4U, &values[4]) ||
        !s_unsigned(s_fields[7], 1U, &values[5]) ||
        !s_stamp_valid(s_fields[8]) || !s_stamp_valid(s_fields[9]) ||
        !s_stamp_valid(s_fields[10])) { return 0U; }
    p_alert = &p_batch->alerts[values[0]];
    for (i = 0U; i < FEED_FIELD_COUNT; i++) {
        (void)s_text_field(p_alert, i, &capacity);
        if (!s_unsigned(s_fields[i + 11U], capacity, &values[i + 6U])) { return 0U; }
    }
    if (values[6] == 0U || values[8] == 0U || values[10] == 0U) { return 0U; }
    p_alert->type = (weather_alert_type_t)values[1];
    p_alert->color = (weather_alert_color_t)values[2];
    p_alert->severity = (uint8_t)values[3]; p_alert->urgency = (uint8_t)values[4];
    p_alert->important = (uint8_t)values[5];
    s_stamp_copy(p_alert->issued, s_fields[8]); s_stamp_copy(p_alert->effective, s_fields[9]);
    s_stamp_copy(p_alert->expires, s_fields[10]);
    if (p_alert->effective[0] != '\0' && p_alert->expires[0] != '\0' &&
        strcmp(p_alert->effective, p_alert->expires) > 0) { return 0U; }
    for (i = 0U; i < FEED_FIELD_COUNT; i++) {
        s_expected[values[0]][i] = (uint16_t)values[i + 6U];
    }
    s_have_alerts++; return 1U;
}

static uint8_t s_text(uint8_t count, weather_feed_batch_t *p_batch)
{
    uint32_t index;
    uint32_t field;
    uint32_t offset;
    uint16_t capacity;
    uint16_t length;
    char *p_target;
    if (count != 6U || !s_unsigned(s_fields[2], WEATHER_FEED_MAX_ALERTS - 1U, &index) ||
        index >= s_have_alerts || !s_unsigned(s_fields[3], FEED_FIELD_COUNT - 1U, &field) ||
        !s_unsigned(s_fields[4], WEATHER_FEED_BODY_MAX, &offset)) { return 0U; }
    length = (uint16_t)(strlen(s_fields[5]) / 2U);
    p_target = s_text_field(&p_batch->alerts[index], (uint8_t)field, &capacity);
    if (length == 0U || length > FEED_TEXT_CHUNK_MAX ||
        offset != s_written[index][field] || offset + length > s_expected[index][field] ||
        offset + length > capacity ||
        !s_hex(s_fields[5], (uint8_t *)&p_target[offset], length, 0U)) { return 0U; }
    s_written[index][field] = (uint16_t)(offset + length);
    p_target[offset + length] = '\0'; return 1U;
}

static uint8_t s_glyph(uint8_t count, weather_feed_batch_t *p_batch)
{
    uint32_t index;
    uint32_t codepoint;
    uint16_t i;
    weather_feed_glyph_t *p_glyph;
    if (count != 5U || !s_unsigned(s_fields[2], WEATHER_FEED_MAX_GLYPHS - 1U, &index) ||
        index != s_have_glyphs || index >= p_batch->glyph_count ||
        !s_unsigned(s_fields[3], 0x10FFFFUL, &codepoint) || codepoint <= 126U ||
        codepoint == 176U || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) { return 0U; }
    for (i = 0U; i < s_have_glyphs; i++) {
        if (p_batch->glyphs[i].codepoint == codepoint) { return 0U; }
    }
    p_glyph = &p_batch->glyphs[index];
    if (!s_hex(s_fields[4], p_glyph->bitmap, WEATHER_FEED_GLYPH_BYTES, 1U) ||
        (p_glyph->bitmap[WEATHER_FEED_GLYPH_BYTES - 1U] & 0x0FU) != 0U) { return 0U; }
    p_glyph->codepoint = codepoint; s_have_glyphs++; return 1U;
}

static uint8_t s_complete(const weather_feed_batch_t *p_batch)
{
    uint8_t i;
    uint8_t j;
    uint16_t capacity;
    const char *p_text;
    if (s_have_now == 0U || s_have_metrics == 0U || s_have_days != p_batch->day_count ||
        s_have_alerts != p_batch->alert_count || s_have_glyphs != p_batch->glyph_count) {
        return 0U;
    }
    for (i = 0U; i < p_batch->alert_count; i++) {
        for (j = 0U; j < FEED_FIELD_COUNT; j++) {
            if (s_written[i][j] != s_expected[i][j]) { return 0U; }
            p_text = s_text_field((weather_feed_alert_t *)&p_batch->alerts[i], j, &capacity);
            if (s_text_valid(p_text, p_batch, (uint8_t)(j >= 2U),
                (uint8_t)(j >= 4U)) == 0U) { return 0U; }
        }
        for (j = 0U; j < i; j++) {
            if (strcmp(p_batch->alerts[i].id, p_batch->alerts[j].id) == 0) { return 0U; }
        }
    }
    return 1U;
}

static uint8_t s_alert_changed(const weather_feed_alert_t *p_new,
                               const weather_feed_alert_t *p_old)
{
    return (uint8_t)(p_new->type != p_old->type || p_new->color != p_old->color ||
        p_new->severity != p_old->severity || p_new->urgency != p_old->urgency ||
        p_new->important != p_old->important ||
        strcmp(p_new->title, p_old->title) != 0 || strcmp(p_new->body, p_old->body) != 0 ||
        strcmp(p_new->guide, p_old->guide) != 0 || strcmp(p_new->source, p_old->source) != 0 ||
        strcmp(p_new->issued, p_old->issued) != 0 ||
        strcmp(p_new->effective, p_old->effective) != 0 ||
        strcmp(p_new->expires, p_old->expires) != 0);
}

static void s_publish(uint32_t now_tick, uint32_t crc)
{
    weather_feed_batch_t *p_new = &s_batches[s_staging];
    const weather_feed_batch_t *p_old = s_active != FEED_NO_SLOT ? &s_batches[s_active] : NULL;
    uint8_t i;
    uint8_t j;
    uint8_t active_count = 0U;
    uint8_t attention = FEED_NO_SLOT;
    uint8_t changed;
    /* Failure retains warnings and glyphs from the last successful check. */
    if (p_new->alert_state == WEATHER_ALERT_OFFLINE && p_old != NULL &&
        p_new->city == p_old->city) {
        p_new->alert_count = p_old->alert_count;
        memcpy(p_new->alerts, p_old->alerts, sizeof(p_new->alerts));
        p_new->glyph_count = p_old->glyph_count;
        memcpy(p_new->glyphs, p_old->glyphs, sizeof(p_new->glyphs));
        memcpy(p_new->last_success, p_old->last_success, sizeof(p_new->last_success));
    } else if (p_new->alert_state != WEATHER_ALERT_UNKNOWN) {
        memcpy(p_new->last_success, p_new->checked, sizeof(p_new->last_success));
        if (p_new->alert_state == WEATHER_ALERT_NONE && p_old != NULL &&
            p_new->city == p_old->city && p_old->alert_count != 0U) {
            p_new->alert_state = WEATHER_ALERT_CLEARED;
        }
    }
    for (i = 0U; i < p_new->alert_count && p_new->alert_state == WEATHER_ALERT_ACTIVE; i++) {
        if (p_new->alerts[i].expires[0] != '\0' && p_new->checked[0] != '\0' &&
            strcmp(p_new->alerts[i].expires, p_new->checked) <= 0) {
            p_new->alerts[i].expired = 1U; continue;
        }
        active_count++;
        if (p_new->alerts[i].effective[0] != '\0' &&
            strcmp(p_new->alerts[i].effective, p_new->checked) > 0) {
            p_new->alerts[i].pending = 1U; continue;
        }
        if (p_new->alerts[i].important == 0U) { continue; }
        if (p_old != NULL && p_old->city == p_new->city) {
            for (j = 0U; j < p_old->alert_count; j++) {
                if (strcmp(p_new->alerts[i].id, p_old->alerts[j].id) == 0 ||
                    (p_new->alerts[i].replaces[0] != '\0' &&
                     strcmp(p_new->alerts[i].replaces, p_old->alerts[j].id) == 0)) { break; }
            }
            if (j < p_old->alert_count && !s_alert_changed(&p_new->alerts[i], &p_old->alerts[j])) {
                continue;
            }
        }
        if (attention == FEED_NO_SLOT ||
            p_new->alerts[i].severity > p_new->alerts[attention].severity ||
            (p_new->alerts[i].severity == p_new->alerts[attention].severity &&
             p_new->alerts[i].urgency > p_new->alerts[attention].urgency)) { attention = i; }
    }
    if (p_new->alert_state == WEATHER_ALERT_ACTIVE && active_count == 0U) {
        p_new->alert_state = WEATHER_ALERT_CLEARED;
    }
    changed = (uint8_t)(p_old == NULL ||
        memcmp((const uint8_t *)p_new + offsetof(weather_feed_batch_t, city),
               (const uint8_t *)p_old + offsetof(weather_feed_batch_t, city),
               sizeof(*p_new) - offsetof(weather_feed_batch_t, city)) != 0);
    taskENTER_CRITICAL();
    if (changed != 0U) { s_revision++; if (s_revision == 0U) { s_revision = 1U; } }
    s_commit_count++; if (s_commit_count == 0U) { s_commit_count = 1U; }
    p_new->revision = s_revision;
    if (attention != FEED_NO_SLOT) {
        s_attention_revision = s_revision; s_attention_index = attention;
        memcpy(s_attention_id, p_new->alerts[attention].id, sizeof(s_attention_id));
    } else if (s_attention_revision != 0U) {
        for (i = 0U; i < p_new->alert_count; i++) {
            if (strcmp(s_attention_id, p_new->alerts[i].id) == 0 &&
                p_new->alerts[i].expired == 0U && p_new->alerts[i].pending == 0U &&
                p_new->alerts[i].important != 0U) { break; }
        }
        if (i < p_new->alert_count) { s_attention_index = i; }
        else { s_attention_revision = 0U; s_attention_id[0] = '\0'; }
    }
    s_active = s_staging; s_staging = FEED_NO_SLOT;
    s_published_tick = now_tick; s_expired = 0U;
    s_last_id = p_new->id; s_last_crc = crc; s_last_sequence = s_sequence;
    taskEXIT_CRITICAL();
}

void weather_feed_init(weather_feed_send_fn send)
{
    memset(s_batches, 0, sizeof(s_batches)); memset(s_pins, 0, sizeof(s_pins));
    s_active = FEED_NO_SLOT; s_staging = FEED_NO_SLOT; s_console = 0U;
    s_revision = 0U; s_commit_count = 0U; s_attention_revision = 0U; s_attention_index = 0U;
    s_attention_id[0] = '\0'; s_sequence = 0U; s_activity = 0U; s_published_tick = 0U;
    s_last_id = 0U; s_last_crc = 0U; s_last_sequence = 0U; s_expired = 0U;
    s_send = send;
}

void weather_feed_set_console(uint8_t enabled)
{
    s_console = (uint8_t)(enabled != 0U);
    if (s_console == 0U) { s_staging = FEED_NO_SLOT; }
}

uint8_t weather_feed_busy(void) { return (uint8_t)(s_staging != FEED_NO_SLOT); }

void weather_feed_poll(uint32_t now_tick)
{
    if (s_staging != FEED_NO_SLOT && (uint32_t)(now_tick - s_activity) >= FEED_TIMEOUT_TICKS &&
        (uint32_t)(now_tick - s_activity) < FEED_HALF_RANGE) {
        s_staging = FEED_NO_SLOT; s_send_text("ERR WDATA TIMEOUT\r\n");
    }
    taskENTER_CRITICAL();
    if (s_active != FEED_NO_SLOT && (uint32_t)(now_tick - s_published_tick) >= FEED_EXPIRY_TICKS &&
        (uint32_t)(now_tick - s_published_tick) < FEED_HALF_RANGE) { s_expired = 1U; }
    taskEXIT_CRITICAL();
}

uint8_t weather_feed_command(const char *p_line, uint32_t now_tick)
{
    char *p_arguments;
    uint8_t count;
    uint8_t result = 0U;
    size_t length = 0U;
    uint32_t id;
    uint32_t sequence;
    uint32_t crc;
    weather_feed_batch_t *p_batch;
    if (p_line == NULL || strncmp(p_line, "WDATA", 5U) != 0 ||
        (p_line[5] != ' ' && p_line[5] != '\0')) { return 0U; }
    while (length <= WEATHER_FEED_LINE_MAX && p_line[length] != '\0') { length++; }
    if (length > WEATHER_FEED_LINE_MAX) { s_send_text("ERR WDATA LENGTH\r\n"); return 1U; }
    if (length < 7U) { s_send_text("ERR WDATA ARGUMENT\r\n"); return 1U; }
    weather_feed_poll(now_tick);
    if (strcmp(p_line, "WDATA STATUS") == 0) {
        (void)snprintf(s_reply, sizeof(s_reply), "OK WDATA STATUS %lu,%u,%lu\r\n",
            (unsigned long)(s_active != FEED_NO_SLOT ? s_batches[s_active].id : 0U),
            (unsigned int)weather_feed_busy(), (unsigned long)s_sequence);
        s_send_text(s_reply); return 1U;
    }
    if (strcmp(p_line, "WDATA CANCEL") == 0) {
        s_staging = FEED_NO_SLOT; s_send_text("OK WDATA CANCEL\r\n"); return 1U;
    }
    memcpy(s_line, p_line, length + 1U);
    p_arguments = strchr(&s_line[6], ' ');
    if (p_arguments == NULL) { s_send_text("ERR WDATA ARGUMENT\r\n"); return 1U; }
    *p_arguments++ = '\0'; count = s_split(p_arguments);
    if (strcmp(&s_line[6], "BEGIN") == 0) {
        result = s_begin(count, now_tick);
        if (result == 0U) { s_send_text("ERR WDATA ARGUMENT\r\n"); }
        return 1U;
    }
    if (count < 2U || !s_unsigned(s_fields[0], UINT32_MAX, &id) ||
        !s_unsigned(s_fields[1], 65535U, &sequence)) {
        s_send_text("ERR WDATA ARGUMENT\r\n"); return 1U;
    }
    if (strcmp(&s_line[6], "COMMIT") == 0) {
        if (count != 3U || !s_hex_crc(s_fields[2], &crc)) {
            s_send_text("ERR WDATA ARGUMENT\r\n"); return 1U;
        }
        if (s_staging == FEED_NO_SLOT) {
            if (id == s_last_id && sequence == s_last_sequence && crc == s_last_crc && id != 0U) {
                s_send_text("OK WDATA COMMIT\r\n");
            } else { s_send_text("ERR WDATA TRANSACTION\r\n"); }
            return 1U;
        }
        p_batch = &s_batches[s_staging];
        if (id != p_batch->id || sequence != s_sequence) {
            s_send_text("ERR WDATA SEQUENCE\r\n"); return 1U;
        }
        if (crc != (s_crc ^ UINT32_MAX)) { s_send_text("ERR WDATA CRC\r\n"); return 1U; }
        if (!s_complete(p_batch)) { s_send_text("ERR WDATA INCOMPLETE\r\n"); return 1U; }
        if (id == s_last_id) {
            if (sequence == s_last_sequence && crc == s_last_crc) {
                s_staging = FEED_NO_SLOT; s_send_text("OK WDATA COMMIT\r\n");
            } else { s_send_text("ERR WDATA CONFLICT\r\n"); }
            return 1U;
        }
        s_publish(now_tick, crc); s_send_text("OK WDATA COMMIT\r\n"); return 1U;
    }
    if (s_staging == FEED_NO_SLOT || id != s_batches[s_staging].id) {
        s_send_text("ERR WDATA TRANSACTION\r\n"); return 1U;
    }
    if (sequence != s_sequence) {
        if (s_sequence != 0U && sequence == s_sequence - 1U && strcmp(p_line, s_previous) == 0) {
            s_activity = now_tick; s_ack(id, sequence);
        } else { s_send_text("ERR WDATA SEQUENCE\r\n"); }
        return 1U;
    }
    p_batch = &s_batches[s_staging];
    if (strcmp(&s_line[6], "NOW") == 0) { result = s_now(count, p_batch); }
    else if (strcmp(&s_line[6], "METRICS") == 0) { result = s_metrics(count, p_batch); }
    else if (strcmp(&s_line[6], "DAY") == 0) { result = s_day(count, p_batch); }
    else if (strcmp(&s_line[6], "ALERT") == 0) { result = s_alert(count, p_batch); }
    else if (strcmp(&s_line[6], "TEXT") == 0) { result = s_text(count, p_batch); }
    else if (strcmp(&s_line[6], "GLYPH") == 0) { result = s_glyph(count, p_batch); }
    if (result == 0U) { s_send_text("ERR WDATA ARGUMENT\r\n"); return 1U; }
    s_crc = s_crc_update(s_crc, (const uint8_t *)p_line, length);
    s_crc = s_crc_update(s_crc, (const uint8_t *)"\n", 1U);
    memcpy(s_previous, p_line, length + 1U); s_sequence++; s_activity = now_tick;
    s_ack(id, sequence); return 1U;
}

void weather_feed_get_snapshot(uint32_t now_tick, weather_feed_snapshot_t *p_snapshot)
{
    uint8_t i;
    const weather_feed_batch_t *p_batch;
    uint32_t elapsed;
    if (p_snapshot == NULL) { return; }
    memset(p_snapshot, 0, sizeof(*p_snapshot));
    taskENTER_CRITICAL();
    if (s_active != FEED_NO_SLOT) {
        p_batch = &s_batches[s_active]; elapsed = (uint32_t)(now_tick - s_published_tick);
        if (elapsed >= FEED_EXPIRY_TICKS && elapsed < FEED_HALF_RANGE) { s_expired = 1U; }
        p_snapshot->available = 1U; p_snapshot->fresh = (uint8_t)(s_expired == 0U);
        p_snapshot->revision = p_batch->revision;
        p_snapshot->commit_count = s_commit_count;
        p_snapshot->city = p_batch->city; p_snapshot->alert_state = p_batch->alert_state;
        p_snapshot->attention_revision = s_attention_revision;
        p_snapshot->attention_index = s_attention_index;
        for (i = 0U; i < p_batch->alert_count; i++) {
            if (p_batch->alerts[i].expired == 0U) {
                p_snapshot->alert_count++; p_snapshot->alert_mask |= (uint8_t)(1U << i);
            }
        }
        if (s_attention_index >= p_batch->alert_count ||
            p_batch->alerts[s_attention_index].expired != 0U ||
            p_batch->alerts[s_attention_index].pending != 0U) {
            p_snapshot->attention_revision = 0U;
        }
    }
    taskEXIT_CRITICAL();
}

bool weather_feed_acquire(uint32_t revision, weather_feed_lease_t *p_lease)
{
    uint8_t slot;
    bool acquired = false;
    if (p_lease == NULL) { return false; }
    p_lease->p_batch = NULL; p_lease->slot = FEED_NO_SLOT;
    taskENTER_CRITICAL();
    for (slot = 0U; slot < FEED_SLOT_COUNT; slot++) {
        if (slot != s_staging && s_batches[slot].revision != 0U &&
            ((revision == 0U && slot == s_active) || s_batches[slot].revision == revision) &&
            s_pins[slot] < UINT16_MAX) {
            s_pins[slot]++; p_lease->p_batch = &s_batches[slot]; p_lease->slot = slot;
            acquired = true; break;
        }
    }
    taskEXIT_CRITICAL(); return acquired;
}

void weather_feed_release(weather_feed_lease_t *p_lease)
{
    if (p_lease == NULL) { return; }
    taskENTER_CRITICAL();
    if (p_lease->slot < FEED_SLOT_COUNT &&
        p_lease->p_batch == &s_batches[p_lease->slot] && s_pins[p_lease->slot] != 0U) {
        s_pins[p_lease->slot]--;
    }
    p_lease->p_batch = NULL; p_lease->slot = FEED_NO_SLOT;
    taskEXIT_CRITICAL();
}

void weather_feed_expire(const clock_calendar_t *p_now)
{
    char stamp[WEATHER_FEED_STAMP_MAX + 1U];
    weather_feed_batch_t *p_batch;
    uint8_t i;
    uint8_t changed = 0U;
    uint8_t count = 0U;
    uint8_t attention = FEED_NO_SLOT;
    if (p_now == NULL || !clock_calendar_valid(p_now)) { return; }
    (void)snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02u",
        (unsigned int)p_now->year, (unsigned int)p_now->month, (unsigned int)p_now->day,
        (unsigned int)p_now->hour, (unsigned int)p_now->minute, (unsigned int)p_now->second);
    taskENTER_CRITICAL();
    if (s_active != FEED_NO_SLOT && s_pins[s_active] == 0U) {
        p_batch = &s_batches[s_active];
        for (i = 0U; i < p_batch->alert_count; i++) {
            if (p_batch->alerts[i].expired == 0U && p_batch->alerts[i].expires[0] != '\0' &&
                strcmp(p_batch->alerts[i].expires, stamp) <= 0) {
                p_batch->alerts[i].expired = 1U; changed = 1U;
            }
            if (p_batch->alerts[i].expired == 0U && p_batch->alerts[i].pending != 0U &&
                strcmp(p_batch->alerts[i].effective, stamp) <= 0) {
                p_batch->alerts[i].pending = 0U; changed = 1U;
                if (p_batch->alerts[i].important != 0U &&
                    p_batch->alert_state == WEATHER_ALERT_ACTIVE) { attention = i; }
            }
            if (p_batch->alerts[i].expired == 0U) { count++; }
        }
        if (changed != 0U) {
            s_revision++; if (s_revision == 0U) { s_revision = 1U; }
            p_batch->revision = s_revision;
            if (attention != FEED_NO_SLOT) {
                s_attention_revision = s_revision; s_attention_index = attention;
                memcpy(s_attention_id, p_batch->alerts[attention].id, sizeof(s_attention_id));
            }
            if (count == 0U && p_batch->alert_state == WEATHER_ALERT_ACTIVE) {
                p_batch->alert_state = WEATHER_ALERT_CLEARED;
            }
        }
    }
    taskEXIT_CRITICAL();
}
