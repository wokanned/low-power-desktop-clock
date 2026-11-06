/* Real renderers, scheduler, display task, bringup and controller driver.
 * Fake only data providers, RTOS and the panel IO.  The panel model decodes
 * controller windows and reconstructs the actual visible 1bpp image. */
#include "../app/display.h"
#include "../app/epd_bringup.h"
#include "../app/epd_clock_view.h"
#include "../app/epd_weather_view.h"
#include "../app/clock_service.h"
#include "../app/clock_faults.h"
#include "../app/weather.h"
#include "../app/forecast.h"
#include "../app/weather_feed.h"
#include "../app/epd_weather_pages.h"
#include "../platform/stm32_gdey029t94.h"
epd_gdey029t94_status_t stm32_gdey029t94_stop(void)
{ return EPD_GDEY029T94_OK; }
#include "stubs/display/FreeRTOS.h"
#include "stubs/display/cmsis_os.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_BYTES EPD_GDEY029T94_FRAME_BUFFER_SIZE
#define MAX_ACTIVATIONS 160U
#define CHECK(x) do { s_checks++; if (!(x)) { \
    fprintf(stderr,"FAIL %s:%d scenario=%s phase=%lu: %s\n",__FILE__,__LINE__, \
        s_scenario,(unsigned long)s_phase,#x); exit(1); } } while (0)

typedef struct {
    uint8_t mode;
    uint16_t native_x, native_y, width, height;
    uint32_t completed_before;
} activation_t;

typedef struct {
    uint8_t dc, cs, command, update, parameter[4];
    uint8_t x_start, x_end, x_counter;
    uint16_t y_start, y_end, y_counter;
    uint8_t new_ram[FRAME_BYTES], old_ram[FRAME_BYTES], visible[FRAME_BYTES];
    uint8_t pending, busy_seen;
    uint32_t activations, full, partial, sleeps, old_writes, resets;
    activation_t trace[MAX_ACTIVATIONS];
} panel_t;

static const char *s_scenario;
static unsigned long s_checks;
static panel_t s_panel;
static clock_view_model_t s_live, s_expected;
static weather_snapshot_t s_expected_weather;
static display_page_t s_expected_page;
static uint8_t s_reference[FRAME_BYTES];
static jmp_buf s_stop;
static uint32_t s_tick, s_phase, s_polls, s_last_completed, s_retry_delays;
static uint8_t s_failed_window, s_injected, s_driver_only, s_timeout_active;
static osThreadFunc_t s_thread;
static void *s_thread_argument;
static uint32_t s_feed_id;
static uint8_t s_expected_paused;

static int scenario(const char *name) { return strcmp(s_scenario,name)==0; }

void clock_service_get_model(clock_view_model_t *model) { *model=s_live; }
void clock_service_note_sync_success(void)
{
    s_live.wifi.state=CLOCK_DATA_READY;
    s_live.wifi.value=0;
}
void Debug_Log(const char *format,...) { (void)format; }
TickType_t xTaskGetTickCount(void) { return s_tick; }

static void publish_weather(uint8_t condition)
{
    weather_data_t data;
    CHECK(weather_parse_line("WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,2,235,180,270,65,2,35,2026-10-28T16:00",&data));
    data.condition=condition;
    CHECK(display_publish_weather(&data,s_tick));
}

static uint8_t feed_send(const uint8_t *data,uint16_t length)
{ CHECK(length>=2U && data[0]=='O' && data[1]=='K');return 1U; }

static uint32_t feed_crc(uint32_t crc,const char *text)
{
    uint8_t bit;
    while(*text!='\0') {
        crc^=(uint8_t)*text++;
        for(bit=0U;bit<8U;bit++) {
            crc=(crc>>1)^((crc&1U)!=0U?0xEDB88320UL:0U);
        }
    }
    return crc;
}

static void publish_feed(void)
{
    static const char *const verbs[]={"NOW","METRICS","DAY","DAY","DAY"};
    static const char *const payloads[]={
        "1,235,180,270,72,2,50,3,4,2026-10-28T16:00",
        "251,60,42,0,1012,180", "0,2026-10-28,1,180,270,20",
        "1,2026-10-29,4,190,260,NA", "2,2026-10-30,0,200,280,0"};
    char command[224];
    uint32_t crc=UINT32_MAX;
    uint8_t i;
    s_feed_id++;
    (void)snprintf(command,sizeof(command),
        "WDATA BEGIN %lu,1,3,0,2,2026-10-28T16:00,2026-10-28T16:00,0",
        (unsigned long)s_feed_id);
    CHECK(weather_feed_command(command,s_tick)==1U);
    for(i=0U;i<5U;i++) {
        (void)snprintf(command,sizeof(command),"WDATA %s %lu,%u,%s",verbs[i],
            (unsigned long)s_feed_id,(unsigned int)i,payloads[i]);
        CHECK(weather_feed_command(command,s_tick)==1U);
        crc=feed_crc(crc,command);crc=feed_crc(crc,"\n");
    }
    (void)snprintf(command,sizeof(command),"WDATA COMMIT %lu,5,%08lX",
        (unsigned long)s_feed_id,(unsigned long)(crc^UINT32_MAX));
    CHECK(weather_feed_command(command,s_tick)==1U);
}

static void expect_frame(display_page_t page,clock_memory_mode_t memory)
{
    s_expected=s_live;
    s_expected.memory_mode=memory;
    s_expected_page=page;
    weather_get_snapshot(s_tick,&s_expected_weather);
}

static void expect_forecast(uint8_t available)
{
    expect_frame(DISPLAY_PAGE_WEATHER,CLOCK_MEMORY_SRAM);
    memset(&s_expected_weather,0,sizeof(s_expected_weather));
    s_expected_weather.kind=available?WEATHER_VIEW_FORECAST:WEATHER_VIEW_NO_TODAY;
    s_expected_weather.available=available;
    s_expected_weather.fresh=available;
    s_expected_weather.data.city=WEATHER_CITY_SHENZHEN;
    s_expected_weather.forecast_date=s_live.calendar;
    if(available) {
        s_expected_weather.data.condition=2U;
        s_expected_weather.data.minimum_tenths=180;
        s_expected_weather.data.maximum_tenths=270;
    }
}

static void publish_forecast(void)
{
    forecast_batch_t batch;
    memset(&batch,0,sizeof(batch));
    batch.id=42U;batch.city=WEATHER_CITY_SHENZHEN;batch.count=2U;
    batch.days[0].date=s_live.calendar;
    batch.days[0].condition=2U;
    batch.days[0].minimum_tenths=180;batch.days[0].maximum_tenths=270;
    batch.days[1]=batch.days[0];
    clock_calendar_add_seconds(&batch.days[1].date,86400UL);
    CHECK(forecast_publish(&batch));
}

static void verify_pixels(void)
{
    uint32_t i;
    weather_feed_snapshot_t feed;
    weather_feed_lease_t lease;
    weather_page_kind_t page;
    weather_feed_get_snapshot(s_tick,&feed);
    if(s_expected_page!=DISPLAY_PAGE_CLOCK && s_expected.faults.active_categories==0U) {
        if(feed.available!=0U) {
            page=s_expected_page==DISPLAY_PAGE_FORECAST?WEATHER_PAGE_FORECAST:
                (s_expected_page==DISPLAY_PAGE_DATA?WEATHER_PAGE_DATA:WEATHER_PAGE_LIVE);
            CHECK(weather_feed_acquire(feed.revision,&lease));
            CHECK(epd_weather_pages_draw_bw_for_date(s_reference,sizeof(s_reference),
                lease.p_batch,page,0U,0U,feed.fresh,&s_expected.calendar));
            weather_feed_release(&lease);
        } else {
            CHECK(epd_weather_view_draw_bw(s_reference,sizeof(s_reference),&s_expected_weather,&s_expected));
        }
    } else {
        CHECK(epd_clock_view_draw_bw(s_reference,sizeof(s_reference),&s_expected));
    }
    if(s_expected_paused!=0U) {
        uint32_t x,y,offset,shift;
        for(x=288U;x<296U;x++) {
            for(y=0U;y<12U;y++) {
                offset=(295U-x)*16U+(127U-y)/8U;
                shift=7U-(127U-y)%8U;
                s_reference[offset]|=(uint8_t)(1U<<shift);
                if(y>=2U && y<10U && (x==289U || x==290U || x==293U || x==294U)) {
                    s_reference[offset]&=(uint8_t)~(1U<<shift);
                }
            }
        }
    }
    for(i=0U;i<FRAME_BYTES;i++) {
        if(s_panel.visible[i]!=s_reference[i]) {
            fprintf(stderr,"pixel byte %lu actual=%02X expected=%02X\n",
                (unsigned long)i,s_panel.visible[i],s_reference[i]);
            CHECK(s_panel.visible[i]==s_reference[i]);
        }
    }
    CHECK(memcmp(s_panel.visible,s_reference,sizeof(s_reference))==0);
}

static void apply_activation(void)
{
    activation_t *a=&s_panel.trace[s_panel.activations-1U];
    uint16_t row, x;
    if(a->mode==0xF7U) {
        memcpy(s_panel.visible,s_panel.new_ram,FRAME_BYTES);
    } else {
        CHECK(a->mode==0xFFU);
        for(row=a->native_y;row<a->native_y+a->height;row++) {
            for(x=a->native_x/8U;x<(a->native_x+a->width)/8U;x++) {
                s_panel.visible[row*16U+x]=s_panel.new_ram[row*16U+x];
            }
        }
    }
}

static void activate(void)
{
    activation_t *a;
    CHECK(s_panel.activations<MAX_ACTIVATIONS);
    CHECK(s_panel.pending==0U);
    a=&s_panel.trace[s_panel.activations++];
    a->mode=s_panel.update;
    a->native_x=(uint16_t)(s_panel.x_start*8U);
    a->native_y=(uint16_t)(295U-s_panel.y_start);
    a->width=(uint16_t)((s_panel.x_end-s_panel.x_start+1U)*8U);
    a->height=(uint16_t)(s_panel.y_start-s_panel.y_end+1U);
    a->completed_before=epd_bringup_get_completed_frames();
    CHECK(s_driver_only || a->completed_before==s_last_completed);
    CHECK(a->native_x+a->width<=128U && a->native_y+a->height<=296U);
    if(a->mode==0xF7U) { s_panel.full++; }
    else {
        CHECK(a->mode==0xFFU);
        s_panel.partial++;
        if(s_driver_only==0U) {
            /* UI time/memory rectangles transformed to native 128x296.
             * Bounds are checked independently of the production constants. */
            if(s_expected_page==DISPLAY_PAGE_WEATHER) {
                CHECK(a->native_x>=104U && a->native_x+a->width<=128U);
                CHECK(a->native_y+a->height<=102U);
            } else if((s_panel.partial%2U)==1U) {
                CHECK(a->native_x>=40U && a->native_x+a->width<=128U);
                CHECK(a->native_y>=140U && a->native_y+a->height<=288U);
            } else {
                CHECK(a->native_x>=8U && a->native_x+a->width<=56U);
                CHECK(a->native_y+a->height<=64U);
            }
        }
    }
    s_panel.pending=1U;
    s_panel.busy_seen=0U;
}

static int32_t panel_write(void *handle,const uint8_t *data,uint32_t length,uint32_t timeout)
{
    uint32_t i, offset;
    uint8_t *ram;
    (void)handle;
    CHECK(s_panel.cs==0U && timeout!=0U && length!=0U);
    if(s_panel.dc==0U) {
        CHECK(length==1U);
        s_panel.command=data[0];
        if(data[0]==0x20U) { activate(); }
        return 0;
    }
    if(s_panel.command==0x24U || s_panel.command==0x26U) {
        if(s_panel.command==0x24U && scenario("second-window-failure") &&
           s_panel.partial==1U && s_failed_window==0U) {
            CHECK(epd_bringup_get_completed_frames()==1U);
            s_failed_window=1U;
            return -1;
        }
        ram=s_panel.command==0x24U?s_panel.new_ram:s_panel.old_ram;
        if(s_panel.command==0x26U) { s_panel.old_writes++; }
        for(i=0U;i<length;i++) {
            CHECK(s_panel.x_counter<=s_panel.x_end && s_panel.y_counter<296U);
            offset=(295U-s_panel.y_counter)*16U+s_panel.x_counter;
            CHECK(offset<FRAME_BYTES);
            ram[offset]=data[i];
            if(s_panel.x_counter==s_panel.x_end) {
                s_panel.x_counter=s_panel.x_start;
                s_panel.y_counter=s_panel.y_counter==s_panel.y_end?s_panel.y_start:
                    (uint16_t)(s_panel.y_counter-1U);
            } else { s_panel.x_counter++; }
        }
        return 0;
    }
    if(length<=sizeof(s_panel.parameter)) { memcpy(s_panel.parameter,data,length); }
    switch(s_panel.command) {
    case 0x44U:
        CHECK(length==2U);s_panel.x_start=data[0];s_panel.x_end=data[1];break;
    case 0x45U:
        CHECK(length==4U);s_panel.y_start=(uint16_t)(data[0]|((uint16_t)data[1]<<8));
        s_panel.y_end=(uint16_t)(data[2]|((uint16_t)data[3]<<8));break;
    case 0x4EU: CHECK(length==1U);s_panel.x_counter=data[0];break;
    case 0x4FU:
        CHECK(length==2U);s_panel.y_counter=(uint16_t)(data[0]|((uint16_t)data[1]<<8));break;
    case 0x22U: CHECK(length==1U);s_panel.update=data[0];break;
    case 0x10U:
        s_panel.sleeps++;
        /* Deep sleep retains the optical image, not the controller RAM. */
        memset(s_panel.new_ram,0xA5U,FRAME_BYTES);
        memset(s_panel.old_ram,0x5AU,FRAME_BYTES);
        break;
    default: break;
    }
    return 0;
}

static int32_t panel_busy(void *handle,uint8_t *level)
{
    (void)handle;
    *level=0U;
    if(s_panel.pending!=0U) {
        CHECK(s_driver_only || epd_bringup_get_completed_frames()==
            s_panel.trace[s_panel.activations-1U].completed_before);
        if(scenario("second-window-timeout") && s_panel.partial==2U && s_failed_window==0U) {
            CHECK(epd_bringup_get_completed_frames()==1U);
            s_failed_window=1U;
            s_timeout_active=1U;
        }
        if(s_timeout_active!=0U) { *level=1U;return 0; }
        if(s_panel.busy_seen==0U) {
            s_panel.busy_seen=1U;
            *level=1U;
            if(scenario("buttons-busy") && s_panel.full==2U && s_injected==0U) {
                CHECK(display_handle_action(DISPLAY_ACTION_NEXT));
                CHECK(display_handle_action(DISPLAY_ACTION_NEXT));
                s_injected=1U;
            }
            if(scenario("busy-coalesce") && s_panel.partial==1U && s_injected==0U) {
                s_injected=1U;
                s_live.calendar.minute=32U;
                s_live.calendar.minute=33U;
            }
        } else {
            apply_activation();
            s_panel.pending=0U;
        }
    }
    return 0;
}

static void panel_cs(void *handle,uint8_t value) { (void)handle;s_panel.cs=value; }
static void panel_dc(void *handle,uint8_t value) { (void)handle;s_panel.dc=value; }
static void panel_reset(void *handle,uint8_t value)
{
    (void)handle;
    if(value==0U) {
        s_panel.resets++;
        s_panel.pending=0U;
        s_timeout_active=0U;
    }
}
static void panel_delay(void *handle,uint32_t ms) { (void)handle;s_tick+=ms; }
static uint32_t panel_tick(void *handle) { (void)handle;return s_tick; }

epd_gdey029t94_status_t stm32_gdey029t94_make_io(
    epd_gdey029t94_io_t *io,stm32_gdey029t94_context_t *context)
{
    CHECK(io!=NULL && context!=NULL);
    *io=(epd_gdey029t94_io_t){panel_write,panel_busy,panel_cs,panel_dc,
        panel_reset,panel_delay,panel_tick,&s_panel};
    return EPD_GDEY029T94_OK;
}
epd_gdey029t94_status_t stm32_gdey029t94_prepare(stm32_gdey029t94_context_t *context)
{ CHECK(context!=NULL);return EPD_GDEY029T94_OK; }

osThreadId_t osThreadNew(osThreadFunc_t thread,void *argument,const osThreadAttr_t *attributes)
{
    CHECK(thread!=NULL && attributes!=NULL && attributes->stack_size==2048U);
    s_thread=thread;s_thread_argument=argument;
    return (osThreadId_t)&s_thread;
}
osThreadId_t osThreadGetId(void) { return (osThreadId_t)&s_thread; }
int32_t osThreadSuspend(osThreadId_t thread)
{ CHECK(thread==osThreadGetId());CHECK(0);return 0; }

static void verify_stats(void)
{
    epd_gdey029t94_refresh_stats_t stats;
    epd_bringup_get_refresh_stats(&stats);
    CHECK(stats.full_refreshes==s_panel.full);
    CHECK(stats.partial_refreshes==s_panel.partial-
        (scenario("second-window-timeout")?1U:0U));
    CHECK(stats.fast_refreshes==0U);
}

static void finish(void)
{
    if(strncmp(s_scenario,"pause-",6U)==0 || scenario("buttons-busy")) {
        CHECK(s_panel.sleeps==2U);
    } else if(scenario("date-full") || scenario("format-full") || scenario("weather-full") ||
       scenario("page-full") || scenario("outside-full") ||
       scenario("forecast-day") || scenario("forecast-missing") ||
       scenario("feed-live-data") || scenario("feed-forecast-day")) {
        CHECK(s_panel.sleeps==1U);
    } else if(scenario("fault-full")) { CHECK(s_panel.sleeps==2U); }
    else { CHECK(s_panel.sleeps==0U); }
    verify_stats();
    longjmp(s_stop,1);
}

static void next_change(void)
{
    if(strncmp(s_scenario,"pause-",6U)==0) {
        CHECK(display_handle_action(DISPLAY_ACTION_TOGGLE_CYCLE));
        s_expected_paused=1U;
        expect_frame(s_expected_page,s_expected_page==DISPLAY_PAGE_CLOCK ?
                     CLOCK_MEMORY_IFLASH:CLOCK_MEMORY_SRAM);
    } else if(scenario("buttons-busy")) {
        CHECK(display_handle_action(DISPLAY_ACTION_NEXT));
        expect_frame(DISPLAY_PAGE_WEATHER,CLOCK_MEMORY_IFLASH);
    } else if(scenario("feed-live-data")) {
        CHECK(display_page_command("PAGE DATA"));
        expect_frame(DISPLAY_PAGE_DATA,CLOCK_MEMORY_SRAM);
    } else if(scenario("feed-forecast-day")) {
        clock_calendar_add_seconds(&s_live.calendar,86400UL);
        expect_frame(DISPLAY_PAGE_FORECAST,CLOCK_MEMORY_SRAM);
    } else if(scenario("feed-duplicate")) {
        publish_feed();
    } else if(scenario("forecast-minute")) {
        s_live.calendar.minute++;
        expect_forecast(1U);
    } else if(scenario("forecast-day") || scenario("forecast-missing")) {
        clock_calendar_add_seconds(&s_live.calendar,86400UL *
            (scenario("forecast-missing")?2U:1U));
        expect_forecast((uint8_t)!scenario("forecast-missing"));
    } else if(scenario("forecast-restore")) {
        publish_forecast();
        /* Restoration changes neither selected page nor WIFI sync state. */
        CHECK(s_live.wifi.state==CLOCK_DATA_UNAVAILABLE);
        s_live.calendar.second++;
    } else if(scenario("weather-minute") || scenario("weather-ampm")) {
        s_live.calendar.minute=0U;
        s_live.calendar.hour++;
        expect_frame(DISPLAY_PAGE_WEATHER,CLOCK_MEMORY_SRAM);
    } else if(scenario("clock-ampm")) {
        s_live.calendar.hour=12U;s_live.calendar.minute=0U;
        expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_IFLASH);
    } else if(scenario("narrow-digits")) {
        s_live.calendar.hour=11U;s_live.calendar.minute=11U;
        expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_IFLASH);
    } else if(scenario("date-full")) {
        clock_calendar_add_seconds(&s_live.calendar,60U);
        expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_IFLASH);
    } else if(scenario("format-full")) {
        s_live.format=CLOCK_FORMAT_12;s_live.format_revision++;
        expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_IFLASH);
    } else if(scenario("weather-full")) {
        publish_weather(4U);
        expect_frame(DISPLAY_PAGE_WEATHER,CLOCK_MEMORY_SRAM);
    } else if(scenario("page-full")) {
        publish_weather(2U);
        expect_frame(DISPLAY_PAGE_WEATHER,CLOCK_MEMORY_SRAM);
    } else if(scenario("fault-full")) {
        clock_fault_report(CLOCK_FAULT_IMU,CLOCK_FAULT_IO);
        clock_faults_get_display_snapshot(&s_live.faults);
        expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_IFLASH);
    } else if(scenario("outside-full")) {
        s_live.temperature.state=CLOCK_DATA_READY;s_live.temperature.value=237;
        s_live.calendar.minute++;
        expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_IFLASH);
    } else if(scenario("duplicate")) {
        s_live.calendar.second++;
    } else {
        s_live.calendar.minute++;
        expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_IFLASH);
    }
}

int32_t osDelay(uint32_t ticks)
{
    uint32_t completed;
    CHECK(ticks==200U || ticks==1000U);
    s_tick+=ticks;
    if(ticks==1000U) {
        s_retry_delays++;
        CHECK((scenario("second-window-failure") || scenario("second-window-timeout")) &&
              s_failed_window!=0U);
        CHECK(epd_bringup_get_completed_frames()==1U);
        s_live.calendar.minute=35U;
        expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_IFLASH);
        return 0;
    }
    s_polls++;
    CHECK(s_polls<20U);
    completed=epd_bringup_get_completed_frames();
    if(completed>s_last_completed) {
        CHECK(completed==s_last_completed+1U);
        verify_pixels();
        s_last_completed=completed;
        if(s_phase==0U) {
            CHECK(s_panel.full==1U && s_panel.partial==0U && s_panel.old_writes==1U);
            next_change();
            s_phase=1U;
        } else if(strncmp(s_scenario,"pause-",6U)==0 && s_phase==1U) {
            CHECK(s_panel.full==2U && s_panel.partial==0U);
            if(scenario("pause-feed")) {
                CHECK(display_handle_action(DISPLAY_ACTION_TOGGLE_CYCLE));
                s_expected_paused=0U;
                expect_frame(s_expected_page,CLOCK_MEMORY_SRAM);
                s_phase=3U;
            } else {
                s_live.calendar.minute++;
                expect_frame(s_expected_page,s_expected_page==DISPLAY_PAGE_CLOCK ?
                             CLOCK_MEMORY_W25Q:CLOCK_MEMORY_SRAM);
                s_phase=2U;
            }
        } else if(strncmp(s_scenario,"pause-",6U)==0 && s_phase==2U) {
            CHECK(s_panel.full==2U && s_panel.partial==
                  (scenario("pause-clock")?2U:1U));
            CHECK(display_handle_action(DISPLAY_ACTION_TOGGLE_CYCLE));
            s_expected_paused=0U;
            expect_frame(s_expected_page,CLOCK_MEMORY_SRAM);
            s_phase=3U;
        } else if(scenario("buttons-busy") && s_phase==1U) {
            CHECK(s_injected!=0U && s_panel.full==2U && s_panel.partial==0U);
            expect_frame(DISPLAY_PAGE_DATA,CLOCK_MEMORY_IFLASH);
            s_phase=2U;
        } else if(scenario("busy-coalesce") && s_phase==1U) {
            CHECK(s_injected!=0U && s_panel.full==1U && s_panel.partial==2U);
            expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_W25Q);
            s_phase=2U;
        } else if(scenario("fault-full") && s_phase==1U) {
            CHECK(s_panel.full==2U && s_panel.partial==0U);
            clock_fault_report(CLOCK_FAULT_IMU,0U);
            clock_faults_get_display_snapshot(&s_live.faults);
            expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_IFLASH);
            s_phase=2U;
        } else {
            if(strncmp(s_scenario,"pause-",6U)==0) {
                CHECK(s_panel.full==3U && s_panel.partial==
                    (scenario("pause-clock")?2U:(scenario("pause-weather")?1U:0U)));
            } else if(scenario("buttons-busy")) {
                CHECK(completed==3U && s_panel.full==3U && s_panel.partial==0U);
            } else if(scenario("weather-minute") || scenario("weather-ampm") ||
               scenario("forecast-minute")) {
                CHECK(s_panel.full==1U && s_panel.partial==1U);
            } else if(scenario("date-full") || scenario("format-full") ||
                      scenario("weather-full") || scenario("page-full") ||
                      scenario("outside-full") || scenario("forecast-day") ||
                      scenario("forecast-missing") || scenario("feed-live-data") ||
                      scenario("feed-forecast-day")) {
                CHECK(s_panel.full==2U && s_panel.partial==0U);
            } else if(scenario("fault-full")) {
                CHECK(s_panel.full==3U && s_panel.partial==0U);
            } else if(scenario("second-window-failure")) {
                CHECK(s_panel.full==2U && s_panel.partial==1U && s_retry_delays==1U);
            } else if(scenario("second-window-timeout")) {
                CHECK(s_panel.full==2U && s_panel.partial==2U && s_retry_delays==1U);
            } else if(scenario("busy-coalesce")) {
                CHECK(s_panel.full==1U && s_panel.partial==4U && completed==3U);
            } else {
                CHECK(s_panel.full==1U && s_panel.partial==2U);
            }
            finish();
        }
    } else if((scenario("duplicate") || scenario("forecast-restore") ||
               scenario("feed-duplicate")) && s_polls>=4U) {
        CHECK(completed==1U && s_panel.full==1U && s_panel.partial==0U);
        verify_pixels();
        finish();
    }
    return 0;
}

static void cleanup_contract(void)
{
    epd_gdey029t94_t device;
    epd_gdey029t94_io_t io;
    stm32_gdey029t94_context_t context;
    epd_gdey029t94_rect_t window={0U,0U,8U,1U};
    uint8_t pixel;
    uint32_t i;
    memset(&device,0,sizeof(device));memset(&context,0,sizeof(context));
    s_driver_only=1U;
    CHECK(stm32_gdey029t94_make_io(&io,&context)==EPD_GDEY029T94_OK);
    CHECK(epd_gdey029t94_init_profile(&device,&io,EPD_PROFILE_BW_296)==EPD_GDEY029T94_OK);
    CHECK(epd_clock_view_draw_bw(s_reference,sizeof(s_reference),&s_live));
    CHECK(epd_gdey029t94_set_background(&device,s_reference,sizeof(s_reference))==EPD_GDEY029T94_OK);
    for(i=0U;i<120U;i++) {
        pixel=(uint8_t)(i%2U==0U?0x55U:0xAAU);
        CHECK(epd_gdey029t94_partial_refresh(&device,&window,&pixel,1U)==EPD_GDEY029T94_OK);
    }
    CHECK(device.full_refreshes==2U && device.partial_refreshes==120U);
    CHECK(device.refreshes_since_full==0U && s_panel.full==2U && s_panel.partial==120U);
    CHECK(memcmp(s_panel.visible,device.background_image,FRAME_BYTES)==0);
    CHECK(s_panel.visible[0]==0xAAU);
}

int main(int argc,char **argv)
{
    if(argc!=2) { fprintf(stderr,"scenario required\n");return 2; }
    s_scenario=argv[1];
    memset(&s_panel,0,sizeof(s_panel));
    memset(s_panel.visible,0xFF,FRAME_BYTES);
    memset(&s_live,0,sizeof(s_live));
    clock_faults_init();
    s_live.startup_ready=1U;s_live.calendar_state=CLOCK_DATA_READY;s_live.format=CLOCK_FORMAT_24;
    s_live.calendar=(clock_calendar_t){2026U,10U,28U,3U,16U,30U,0U};
    s_live.memory[0]=(clock_field_t){CLOCK_DATA_READY,37};
    s_live.memory[1]=(clock_field_t){CLOCK_DATA_READY,19};
    s_live.memory[2]=(clock_field_t){CLOCK_DATA_READY,1};
    if(scenario("clock-ampm") || scenario("weather-ampm")) {
        s_live.format=CLOCK_FORMAT_12;s_live.calendar.hour=11U;s_live.calendar.minute=59U;
    }
    if(scenario("date-full")) { s_live.calendar.hour=23U;s_live.calendar.minute=59U; }
    if(scenario("narrow-digits")) { s_live.calendar.hour=18U;s_live.calendar.minute=58U; }
    if(strncmp(s_scenario,"feed-",5U)==0 || scenario("pause-feed") || scenario("buttons-busy")) {
        weather_feed_init(feed_send);weather_feed_set_console(1U);publish_feed();
        if(scenario("feed-clock-minute") || scenario("buttons-busy")) {
            expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_SRAM);
        } else if(scenario("feed-forecast-day")) {
            CHECK(display_page_command("PAGE FORECAST"));
            expect_frame(DISPLAY_PAGE_FORECAST,CLOCK_MEMORY_SRAM);
        } else {
            CHECK(display_page_command("PAGE LIVE"));
            expect_frame(DISPLAY_PAGE_WEATHER,CLOCK_MEMORY_SRAM);
        }
    } else if(scenario("forecast-minute") || scenario("forecast-day") || scenario("forecast-missing")) {
        publish_forecast();
        display_forecast_committed();
        expect_forecast(1U);
    } else if(scenario("weather-minute") || scenario("weather-ampm") || scenario("weather-full") || scenario("pause-weather")) {
        if(scenario("weather-minute"))s_live.calendar.minute=59U;
        publish_weather(2U);
        expect_frame(DISPLAY_PAGE_WEATHER,CLOCK_MEMORY_SRAM);
    } else { expect_frame(DISPLAY_PAGE_CLOCK,CLOCK_MEMORY_SRAM); }
    if(scenario("cleanup-120")) { cleanup_contract(); }
    else {
        CHECK(display_start());CHECK(s_thread!=NULL);
        if(setjmp(s_stop)==0) { s_thread(s_thread_argument);CHECK(0); }
    }
    printf("PASS: real raster/controller %s (%lu checks), full=%lu partial=%lu\n",s_scenario,
        s_checks,(unsigned long)s_panel.full,(unsigned long)s_panel.partial);
    return 0;
}
