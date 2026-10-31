#include "../app/forecast.h"
#include "../app/display.h"
#include "../app/clock_service.h"
#include "../app/epd_bringup.h"
#include "../app/epd_weather_view.h"
#include "../app/epd_clock_view.h"
#include "stubs/display/cmsis_os.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static clock_view_model_t s_clock;
static uint32_t s_tick;
static uint32_t s_syncs;
static uint8_t s_frame[EPD_CLOCK_VIEW_BW_BYTES];
static uint8_t s_reference[EPD_CLOCK_VIEW_BW_BYTES];
static uint8_t s_gray[EPD_CLOCK_VIEW_GRAY4_BYTES];

uint32_t xTaskGetTickCount(void) { return s_tick; }
void clock_service_get_model(clock_view_model_t *p_model) { *p_model = s_clock; }
void clock_service_note_sync_success(void) { s_syncs++; }
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

static forecast_batch_t s_batch(uint16_t year, uint8_t month, uint8_t day,
                                 uint8_t count)
{
    forecast_batch_t batch;
    clock_calendar_t date;
    uint8_t i;
    memset(&batch, 0, sizeof(batch));
    date = (clock_calendar_t){year, month, day, 1U, 0U, 0U, 0U};
    date.weekday = clock_calendar_weekday(year, month, day);
    batch.id = 42U;
    batch.city = WEATHER_CITY_SHENZHEN;
    batch.count = count;
    for (i = 0U; i < count && i < FORECAST_MAX_DAYS; i++) {
        batch.days[i].date = date;
        batch.days[i].condition = (uint8_t)(i + 1U);
        batch.days[i].minimum_tenths = (int16_t)(-999 + i);
        batch.days[i].maximum_tenths = (int16_t)(999 - i);
        clock_calendar_add_seconds(&date, 86400UL);
    }
    return batch;
}

static void s_test_codec(void)
{
    forecast_batch_t batch = s_batch(2028U, 2U, 27U, 7U);
    forecast_batch_t decoded;
    forecast_batch_t sentinel;
    clock_calendar_t earliest = {2028U, 2U, 27U, 1U, 0U, 0U, 0U};
    uint8_t encoded[FORECAST_ENCODED_MAX + 1U];
    uint8_t reference[FORECAST_ENCODED_MAX];
    size_t length = 999U;
    size_t i;
    assert(forecast_validate(&batch, &earliest));
    assert(forecast_encode(&batch, encoded, FORECAST_ENCODED_MAX, &length));
    assert(length == FORECAST_ENCODED_MAX);
    assert(memcmp(encoded, "DCFC", 4U) == 0 && encoded[4] == 1U);
    assert(encoded[8] == 42U && encoded[9] == 0U && encoded[12] == 0xECU && encoded[13] == 7U);
    assert(encoded[17] == 0x19U && encoded[18] == 0xFCU);
    assert(forecast_decode(encoded, length, &decoded));
    assert(forecast_equal(&batch, &decoded));
    assert(decoded.days[2].date.month == 2U && decoded.days[2].date.day == 29U);
    assert(decoded.days[3].date.month == 3U && decoded.days[3].date.day == 1U);
    earliest.day++;
    assert(!forecast_validate(&batch, &earliest));
    assert(forecast_validate(&batch, NULL));
    memset(&sentinel, 0xA5, sizeof(sentinel));
    memcpy(reference, encoded, length);
    for (i = 0U; i < length; i++) {
        encoded[i] ^= 1U;
        decoded = sentinel;
        assert(!forecast_decode(encoded, length, &decoded));
        assert(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
        encoded[i] ^= 1U;
    }
    assert(!forecast_decode(encoded, length - 1U, &decoded));
    assert(!forecast_decode(encoded, length + 1U, &decoded));
    assert(!forecast_encode(&batch, encoded, length - 1U, &length));
    assert(memcmp(encoded, reference, FORECAST_ENCODED_MAX) == 0);
    assert(length == FORECAST_ENCODED_MAX);
    batch.days[1].date.day = 29U;
    assert(!forecast_validate(&batch, NULL));
    batch = s_batch(2099U, 12U, 31U, 2U);
    assert(!forecast_validate(&batch, NULL));
    batch.count = 1U;
    assert(forecast_validate(&batch, NULL));
    batch = s_batch(2026U, 12U, 30U, 7U);
    assert(forecast_validate(&batch, NULL));
    batch.count = 0U;
    assert(!forecast_validate(&batch, NULL));
    batch.count = 8U;
    assert(!forecast_validate(&batch, NULL));
    batch = s_batch(2026U, 10U, 29U, 1U);
    batch.days[0].maximum_tenths = -1000;
    assert(!forecast_validate(&batch, NULL));
    batch.days[0].maximum_tenths = 1000;
    assert(!forecast_validate(&batch, NULL));
    batch.days[0].maximum_tenths = -999;
    batch.days[0].minimum_tenths = 1;
    assert(!forecast_validate(&batch, NULL));
    batch.days[0].minimum_tenths = -999;
    batch.days[0].condition = 14U;
    assert(!forecast_validate(&batch, NULL));
}

static void s_test_selection(void)
{
    forecast_batch_t batch = s_batch(2026U, 10U, 29U, 3U);
    forecast_batch_t readback;
    display_snapshot_t view;
    weather_data_t live;
    uint32_t syncs;
    memset(&s_clock, 0, sizeof(s_clock));
    s_clock.calendar = batch.days[0].date;
    s_clock.calendar.hour = 10U;
    s_clock.calendar_state = CLOCK_DATA_READY;
    s_clock.format = CLOCK_FORMAT_24;
    s_tick = 100U;
    weather_init();
    forecast_init();
    assert(!forecast_get_batch(&readback));
    assert(weather_parse_line("WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,2,235,180,270,65,2,35,2026-10-29T10:00", &live));
    assert(display_publish_weather(&live, s_tick));
    assert(display_select_page(DISPLAY_PAGE_CLOCK));
    display_get_snapshot(s_tick, &view);
    assert(view.weather.kind == WEATHER_VIEW_LIVE && view.weather.available);
    syncs = s_syncs;
    assert(forecast_publish(&batch)); /* Restore: no command-side effects. */
    assert(s_syncs == syncs);
    display_get_snapshot(s_tick, &view);
    assert(view.page == DISPLAY_PAGE_CLOCK && view.weather.kind == WEATHER_VIEW_LIVE);
    live.city = WEATHER_CITY_ZHANJIANG;
    assert(display_publish_weather(&live, s_tick));
    display_get_snapshot(s_tick, &view);
    assert(view.weather.kind == WEATHER_VIEW_FORECAST && view.weather.available);
    assert(view.weather.data.city == WEATHER_CITY_SHENZHEN && view.weather.data.minimum_tenths == -999);
    live.city = WEATHER_CITY_SHENZHEN;
    assert(display_publish_weather(&live, s_tick));
    display_get_snapshot(s_tick, &view);
    assert(view.weather.kind == WEATHER_VIEW_LIVE);
    s_tick += 10800000UL;
    display_get_snapshot(s_tick, &view);
    assert(view.weather.kind == WEATHER_VIEW_FORECAST && !view.weather.expired);
    assert(forecast_get_batch(&readback) && forecast_equal(&batch, &readback));
    s_clock.calendar.day = 30U;
    display_get_snapshot(s_tick, &view);
    assert(view.weather.kind == WEATHER_VIEW_FORECAST && view.weather.data.condition == 2U);
    s_clock.calendar.month = 11U;
    s_clock.calendar.day = 1U;
    display_get_snapshot(s_tick, &view);
    assert(view.weather.kind == WEATHER_VIEW_NO_TODAY && !view.weather.available);
    s_clock.calendar = batch.days[0].date;
    display_get_snapshot(s_tick, &view);
    assert(view.weather.kind == WEATHER_VIEW_FORECAST); /* TIME backward, batch still retained. */
    s_clock.calendar_state = CLOCK_DATA_ERROR;
    display_get_snapshot(s_tick, &view);
    assert(view.weather.kind == WEATHER_VIEW_CLOCK_UNAVAILABLE && !view.weather.available);
    assert(display_select_page(DISPLAY_PAGE_CLOCK));
    syncs = s_syncs;
    display_forecast_committed();
    assert(s_syncs == syncs + 1U);
    display_get_snapshot(s_tick, &view);
    assert(view.page == DISPLAY_PAGE_WEATHER);
    s_tick += 179999U;
    display_get_snapshot(s_tick, &view);
    assert(view.page == DISPLAY_PAGE_WEATHER);
    s_tick++;
    display_get_snapshot(s_tick, &view);
    assert(view.page == DISPLAY_PAGE_CLOCK);
    forecast_clear();
    display_get_snapshot(s_tick, &view);
    assert(view.weather.kind == WEATHER_VIEW_LIVE && view.weather.expired);
    batch.id = 0U;
    assert(!forecast_publish(&batch));
    assert(!forecast_get_batch(&readback));
}

static void s_test_renderer(void)
{
    weather_snapshot_t view;
    memset(&view, 0, sizeof(view));
    view.available = 1U;
    view.kind = WEATHER_VIEW_FORECAST;
    view.data.city = WEATHER_CITY_SHENZHEN;
    view.data.condition = 2U;
    view.data.minimum_tenths = -999;
    view.data.maximum_tenths = 999;
    view.forecast_date = (clock_calendar_t){2026U, 10U, 29U, 4U, 0U, 0U, 0U};
    assert(epd_weather_view_draw_bw(s_frame, sizeof(s_frame), &view, &s_clock));
    memcpy(s_reference, s_frame, sizeof(s_reference));
    /* Missing real-time fields and source_time never leak into a forecast. */
    view.data.temperature_tenths = 999;
    view.data.humidity = 100U;
    view.data.wind_direction = 8U;
    view.data.wind_speed_tenths = 999U;
    memset(view.data.source_time, 'X', sizeof(view.data.source_time));
    assert(epd_weather_view_draw_bw(s_frame, sizeof(s_frame), &view, &s_clock));
    assert(memcmp(s_frame, s_reference, sizeof(s_frame)) == 0);
    assert(epd_weather_view_draw_gray4(s_gray, sizeof(s_gray), &view, &s_clock));
    view.forecast_date.day = 30U;
    assert(epd_weather_view_draw_bw(s_frame, sizeof(s_frame), &view, &s_clock));
    assert(memcmp(s_frame, s_reference, sizeof(s_frame)) != 0);
    view.available = 0U;
    view.kind = WEATHER_VIEW_NO_TODAY;
    assert(epd_weather_view_draw_bw(s_frame, sizeof(s_frame), &view, &s_clock));
    assert(epd_weather_view_draw_gray4(s_gray, sizeof(s_gray), &view, &s_clock));
    view.kind = WEATHER_VIEW_CLOCK_UNAVAILABLE;
    assert(epd_weather_view_draw_bw(s_frame, sizeof(s_frame), &view, &s_clock));
    assert(epd_weather_view_draw_gray4(s_gray, sizeof(s_gray), &view, &s_clock));
}

int main(void)
{
    s_test_codec();
    s_test_selection();
    s_test_renderer();
    puts("PASS: forecast codec/CRC/bounds, atomic RAM publication, dates, live precedence, restore effects and placeholder rendering");
    return 0;
}
