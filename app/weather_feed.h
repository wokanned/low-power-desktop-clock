#ifndef WEATHER_FEED_H
#define WEATHER_FEED_H

#include "weather.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEATHER_FEED_LINE_MAX 223U
#define WEATHER_FEED_MAX_DAYS 3U
#define WEATHER_FEED_MAX_ALERTS 3U
#define WEATHER_FEED_ID_MAX 63U
#define WEATHER_FEED_TITLE_MAX 127U
#define WEATHER_FEED_SOURCE_MAX 95U
#define WEATHER_FEED_BODY_MAX 768U
#define WEATHER_FEED_GUIDE_MAX 512U
#define WEATHER_FEED_MAX_GLYPHS 128U
#define WEATHER_FEED_GLYPH_BYTES 85U
#define WEATHER_FEED_STAMP_MAX 19U

typedef enum {
    WEATHER_ALERT_UNKNOWN = 0, WEATHER_ALERT_ACTIVE, WEATHER_ALERT_NONE,
    WEATHER_ALERT_OFFLINE, WEATHER_ALERT_CLEARED
} weather_alert_state_t;

typedef enum {
    WEATHER_ALERT_RAIN = 0, WEATHER_ALERT_STORM, WEATHER_ALERT_WIND,
    WEATHER_ALERT_TYPHOON, WEATHER_ALERT_HEAT, WEATHER_ALERT_FOG,
    WEATHER_ALERT_SNOW, WEATHER_ALERT_FLOOD, WEATHER_ALERT_OTHER
} weather_alert_type_t;

typedef enum {
    WEATHER_ALERT_COLOR_UNKNOWN = 0, WEATHER_ALERT_WHITE, WEATHER_ALERT_BLUE,
    WEATHER_ALERT_YELLOW, WEATHER_ALERT_ORANGE, WEATHER_ALERT_RED
} weather_alert_color_t;

typedef enum {
    WEATHER_METRIC_FEELS = 0, WEATHER_METRIC_UV, WEATHER_METRIC_AQI_CN,
    WEATHER_METRIC_RAIN_1H, WEATHER_METRIC_PRESSURE, WEATHER_METRIC_VISIBILITY,
    WEATHER_METRIC_COUNT
} weather_metric_index_t;

/* FEELS: 0.1 C; UV: 0.1; AQI-CN: integer; rain: 0.1 mm;
 * pressure: integer hPa; visibility: 0.1 km. Missing values have no valid bit. */
typedef struct {
    int32_t value[WEATHER_METRIC_COUNT];
    uint8_t valid_mask;
} weather_feed_metrics_t;

typedef struct {
    clock_calendar_t date;
    uint8_t condition;
    int16_t minimum_tenths;
    int16_t maximum_tenths;
    uint8_t rain_probability;
    uint8_t rain_probability_valid;
} weather_feed_day_t;

typedef struct {
    char id[WEATHER_FEED_ID_MAX + 1U];
    char replaces[WEATHER_FEED_ID_MAX + 1U];
    char title[WEATHER_FEED_TITLE_MAX + 1U];
    char source[WEATHER_FEED_SOURCE_MAX + 1U];
    char body[WEATHER_FEED_BODY_MAX + 1U];
    char guide[WEATHER_FEED_GUIDE_MAX + 1U];
    char issued[WEATHER_FEED_STAMP_MAX + 1U];
    char effective[WEATHER_FEED_STAMP_MAX + 1U];
    char expires[WEATHER_FEED_STAMP_MAX + 1U];
    weather_alert_type_t type;
    weather_alert_color_t color;
    uint8_t severity; /* 0 unknown, 1 minor, 2 moderate, 3 severe, 4 extreme */
    uint8_t urgency;  /* 0 unknown, 1 past, 2 future, 3 expected, 4 immediate */
    uint8_t important; /* Explicit sender policy; no hard-coded color threshold. */
    uint8_t expired;
    uint8_t pending; /* Effective time is later than the last trusted observation. */
} weather_feed_alert_t;

/* 26x26 row-major continuous bits, MSB first, 1=black. Last four bits are 0. */
typedef struct {
    uint32_t codepoint;
    uint8_t bitmap[WEATHER_FEED_GLYPH_BYTES];
} weather_feed_glyph_t;

typedef struct {
    uint32_t id;
    uint32_t revision;
    weather_city_t city;
    weather_data_t live;
    uint8_t live_valid;
    uint8_t wind_bft_valid;
    uint8_t wind_bft_min;
    uint8_t wind_bft_max;
    weather_feed_metrics_t metrics;
    uint8_t day_count;
    weather_feed_day_t days[WEATHER_FEED_MAX_DAYS];
    weather_alert_state_t alert_state;
    uint8_t alert_count;
    char checked[WEATHER_FEED_STAMP_MAX + 1U];
    char last_success[WEATHER_FEED_STAMP_MAX + 1U];
    weather_feed_alert_t alerts[WEATHER_FEED_MAX_ALERTS];
    uint16_t glyph_count;
    weather_feed_glyph_t glyphs[WEATHER_FEED_MAX_GLYPHS];
} weather_feed_batch_t;

/* Small coherent metadata; never put the full batch on a task stack. */
typedef struct {
    uint32_t revision;
    uint32_t commit_count;
    uint32_t attention_revision;
    weather_city_t city;
    weather_alert_state_t alert_state;
    uint8_t available;
    uint8_t fresh;
    uint8_t alert_count;
    uint8_t alert_mask; /* Batch indices of unexpired alerts. */
    uint8_t attention_index;
} weather_feed_snapshot_t;

typedef struct {
    const weather_feed_batch_t *p_batch;
    uint8_t slot;
} weather_feed_lease_t;

typedef uint8_t (*weather_feed_send_fn)(const uint8_t *p_data, uint16_t length);

/* debugTask owns init/session/command/poll. These functions do not access IO. */
void weather_feed_init(weather_feed_send_fn send);
void weather_feed_set_console(uint8_t enabled);
uint8_t weather_feed_command(const char *p_line, uint32_t now_tick);
void weather_feed_poll(uint32_t now_tick);
uint8_t weather_feed_busy(void);
void weather_feed_get_snapshot(uint32_t now_tick, weather_feed_snapshot_t *p_snapshot);
/* A lease pins an immutable batch through drawing. Release on every path.
 * BEGIN returns BUSY if reusing a still-pinned old batch would corrupt it. */
bool weather_feed_acquire(uint32_t revision, weather_feed_lease_t *p_lease);
void weather_feed_release(weather_feed_lease_t *p_lease);
/* Apply RTC-based validity only to metadata/publication; caller supplies a
 * trustworthy calendar. Does not perform RTC IO or refresh the feed TTL. */
void weather_feed_expire(const clock_calendar_t *p_now);

#endif
