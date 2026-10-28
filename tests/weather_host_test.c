#include "weather.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define VALID_SHENZHEN "WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,2,235,180,270,65,2,35,2026-10-26T10:30"
#define VALID_ZHANJIANG "WEATHER \xE6\xB9\x9B\xE6\xB1\x9F,13,-999,-999,999,100,8,999,2000-02-29T00:00"
#define EXPIRY_TICKS 10800000UL

static void reject(const char *p_line)
{
    weather_data_t data;
    weather_data_t before;
    memset(&data, 0xA5, sizeof(data));
    before = data;
    assert(!weather_parse_line(p_line, &data));
    assert(memcmp(&data, &before, sizeof(data)) == 0);
}

static void check_stale_snapshot_ticks(const weather_data_t *p_data)
{
    /* The second sample was taken before tick wrap, but publication was after. */
    static const uint32_t starts[] = { 1000U, 32U };
    weather_snapshot_t snapshot;
    uint32_t revision;
    uint32_t i;
    for (i = 0U; i < (uint32_t)(sizeof(starts) / sizeof(starts[0])); i++) {
        uint32_t start = starts[i];
        uint32_t stale_tick = start - 40U;
        weather_init();
        assert(weather_publish(p_data, start));
        weather_get_snapshot(start, &snapshot);
        revision = snapshot.revision;
        assert(!weather_poll_expiry(stale_tick));
        weather_get_snapshot(stale_tick, &snapshot);
        assert(snapshot.available == 1U && snapshot.fresh == 1U);
        assert(snapshot.expired == 0U && snapshot.revision == revision);
        assert(snapshot.data.city == p_data->city);

        /* Stale reads must neither expire the record nor move its deadline. */
        weather_get_snapshot(start + EXPIRY_TICKS - 1U, &snapshot);
        assert(snapshot.fresh == 1U && snapshot.revision == revision);
        assert(weather_poll_expiry(start + EXPIRY_TICKS));
        weather_get_snapshot(start + EXPIRY_TICKS, &snapshot);
        assert(snapshot.fresh == 0U && snapshot.expired == 1U);
        assert(snapshot.revision == revision + 1U);
        assert(!weather_poll_expiry(start + EXPIRY_TICKS + 1U));
    }
}

int main(void)
{
    static const char *const invalid[] = {
        "WEATHER 深圳,14,235,180,270,65,2,35,2026-10-26T10:30",
        "WEATHER 上海,2,235,180,270,65,2,35,2026-10-26T10:30",
        "WEATHER 深圳,2,1000,180,270,65,2,35,2026-10-26T10:30",
        "WEATHER 深圳,2,-1000,-999,270,65,2,35,2026-10-26T10:30",
        "WEATHER 深圳,2,235,271,270,65,2,35,2026-10-26T10:30",
        "WEATHER 深圳,2,235,180,270,101,2,35,2026-10-26T10:30",
        "WEATHER 深圳,2,235,180,270,65,9,35,2026-10-26T10:30",
        "WEATHER 深圳,2,235,180,270,65,0,1,2026-10-26T10:30",
        "WEATHER 深圳,2,235,180,270,65,2,1000,2026-10-26T10:30",
        "WEATHER 深圳,2,235,180,270,65,2,35,2026-02-29T10:30",
        "WEATHER 深圳,2,235,180,270,65,2,35,2099-04-31T10:30",
        "WEATHER 深圳,2,235,180,270,65,2,35,2100-02-28T10:30",
        "WEATHER 深圳,2,235,180,270,65,2,35,2026-10-26T24:00",
        "WEATHER 深圳,2,235,180,270,65,2,35,2026-10-26T10:60",
        "WEATHER 深圳,2,235,180,270,65,2,35,2026/10/26T10:30",
        "WEATHER 深圳, 2,235,180,270,65,2,35,2026-10-26T10:30",
        "WEATHER 深圳,2,235,180,270,65,2,35,2026-10-26T10:30,extra",
        "WEATHER 深圳,2,235,180,270,65,2,35"
    };
    weather_data_t shenzhen;
    weather_data_t zhanjiang;
    weather_data_t parsed;
    weather_snapshot_t snapshot;
    uint32_t start;
    uint32_t revision;
    uint8_t i;

    assert(weather_parse_line(VALID_SHENZHEN, &shenzhen));
    assert(shenzhen.city == WEATHER_CITY_SHENZHEN);
    assert(shenzhen.temperature_tenths == 235 && shenzhen.minimum_tenths == 180);
    assert(shenzhen.maximum_tenths == 270 && shenzhen.condition == 2U);
    assert(weather_parse_line(VALID_ZHANJIANG, &zhanjiang));
    assert(zhanjiang.city == WEATHER_CITY_ZHANJIANG);
    assert(zhanjiang.temperature_tenths == -999 && zhanjiang.humidity == 100U);
    assert(zhanjiang.wind_speed_tenths == 999U);
    for (i = 0U; i < (uint8_t)(sizeof(invalid) / sizeof(invalid[0])); i++) {
        reject(invalid[i]);
    }
    assert(!weather_parse_line("WEATHER Shenzhen,2,235,180,270,65,2,35,2026-10-26T10:30",
                               &parsed));

    memset(&parsed, 0, sizeof(parsed));
    assert(weather_parse_line(VALID_SHENZHEN, &parsed));
    weather_init();
    weather_get_snapshot(0U, &snapshot);
    assert(snapshot.available == 0U && snapshot.fresh == 0U && snapshot.expired == 0U);
    start = 0xFFF00000UL;
    assert(weather_publish(&shenzhen, start));
    weather_get_snapshot(start, &snapshot);
    assert(snapshot.available == 1U && snapshot.fresh == 1U && snapshot.expired == 0U);
    revision = snapshot.revision;
    assert(weather_publish(&shenzhen, start + 100U));
    weather_get_snapshot(start + 100U + EXPIRY_TICKS - 1U, &snapshot);
    assert(snapshot.fresh == 1U && snapshot.revision == revision);
    assert(weather_poll_expiry(start + 100U + EXPIRY_TICKS));
    weather_get_snapshot(start + 100U + EXPIRY_TICKS, &snapshot);
    assert(snapshot.available == 1U && snapshot.fresh == 0U && snapshot.expired == 1U);
    assert(snapshot.data.temperature_tenths == 235);
    assert(snapshot.revision == revision + 1U);
    assert(weather_publish(&zhanjiang, start + 100U + EXPIRY_TICKS + 1U));
    weather_get_snapshot(start + 100U + EXPIRY_TICKS + 1U, &snapshot);
    assert(snapshot.fresh == 1U && snapshot.expired == 0U);
    assert(snapshot.revision == revision + 2U);
    assert(snapshot.data.city == WEATHER_CITY_ZHANJIANG);

    check_stale_snapshot_ticks(&shenzhen);

    weather_init();
    start = 0xFFFFFFFFUL - 500U;
    assert(weather_publish(&shenzhen, start));
    weather_get_snapshot(start + EXPIRY_TICKS - 1U, &snapshot);
    assert(snapshot.fresh == 1U);
    assert(weather_poll_expiry(start + EXPIRY_TICKS));
    weather_get_snapshot(start + EXPIRY_TICKS, &snapshot);
    assert(snapshot.expired == 1U && snapshot.available == 1U);
    shenzhen.wind_direction = 9U;
    assert(!weather_publish(&shenzhen, start + EXPIRY_TICKS + 1U));

    puts("PASS: weather protocol, atomic replacement, duplicate reset, stale ticks and expiry across wrap");
    return 0;
}
