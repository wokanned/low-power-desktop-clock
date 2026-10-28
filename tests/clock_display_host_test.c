/* Real display/bringup/schedule; mock only RTOS, data, raster and transport.
 * Each scenario is a fresh process so production private statics stay private. */
#include "../app/display.h"
#include "../app/epd_bringup.h"
#include "../app/epd_clock_view.h"
#include "../app/epd_weather_view.h"
#include "../app/clock_service.h"
#include "../app/clock_faults.h"
#include "../app/weather.h"
#include "../platform/stm32_gdey029t94.h"
#include "stubs/display/FreeRTOS.h"
#include "stubs/display/cmsis_os.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 32U
#define STOP_COMPLETE 1
#define STOP_SUSPENDED 2
#if EPD_CLOCK_USE_GRAY4
#define TEST_FRAME_BYTES EPD_CLOCK_VIEW_GRAY4_BYTES
#define TEST_PROFILE EPD_PROFILE_GRAY4
#define TEST_DRAW epd_clock_view_draw_gray4
#define TEST_REFRESH epd_gdey029t94_gray4_refresh
#define COMPLETED_SEQUENCE_SLEEPS(count) (count)
#else
#define TEST_FRAME_BYTES EPD_CLOCK_VIEW_BW_BYTES
#define TEST_PROFILE EPD_PROFILE_BW_296
#define TEST_DRAW epd_clock_view_draw_bw
#define TEST_REFRESH epd_gdey029t94_full_refresh
#define COMPLETED_SEQUENCE_SLEEPS(count) ((count)-1U)
#endif
#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr,"FAIL %s:%d (%s): %s\n",__FILE__,__LINE__,s_scenario,#condition); \
        exit(1); \
    } \
} while (0)

static const char *s_scenario;
static unsigned long s_checks;
static jmp_buf s_stop;
static clock_view_model_t s_live;
static clock_view_model_t s_render_models[MAX_EVENTS];
static clock_view_model_t s_committed_models[MAX_EVENTS];
static uint32_t s_frame_hashes[MAX_EVENTS];
static uint32_t s_frame_ticks[MAX_EVENTS];
static uint32_t s_delay_ticks[MAX_EVENTS];
static uint32_t s_tick;
static uint32_t s_renders;
static uint32_t s_refreshes;
static uint32_t s_committed;
static uint32_t s_sleeps;
static uint32_t s_init_calls;
static uint32_t s_bind_calls;
static uint32_t s_prepare_calls;
static uint32_t s_retry_delays;
static uint32_t s_poll_delays;
static uint32_t s_model_reads;
static uint32_t s_thread_creates;
static uint32_t s_suspends;
static uint8_t s_sleep_error_observed;
static uint8_t s_weather_published;
static uint32_t s_sync_successes;
static osThreadFunc_t s_thread_function;
static void *s_thread_argument;

static int scenario(const char *p_name)
{
    return strcmp(s_scenario,p_name)==0;
}

static int debug_tx_scenario(void)
{
    return scenario("debug-tx") || scenario("debug-tx-weather");
}

static uint32_t frame_hash(const uint8_t *p_data, uint32_t length)
{
    uint32_t hash=2166136261UL;
    uint32_t i;
    for(i=0U;i<length;i++) { hash=(hash^p_data[i])*16777619UL; }
    return hash;
}

static void change_live(uint32_t revision, uint8_t active)
{
    s_live.faults.revision=revision;
    s_live.faults.active_categories=active;
    s_live.format_revision=revision;
    s_live.format=revision%2U==0U?CLOCK_FORMAT_24:CLOCK_FORMAT_12;
}

static void publish_weather(uint32_t now_tick, uint8_t condition)
{
    weather_data_t weather;
    CHECK(weather_parse_line(
        "WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,2,235,180,270,65,2,35,2026-10-26T10:30",
        &weather));
    weather.condition = condition;
    CHECK(display_publish_weather(&weather, now_tick));
    s_weather_published++;
}

void clock_service_get_model(clock_view_model_t *p_model)
{
    s_model_reads++;
    *p_model=s_live;
    if(debug_tx_scenario()) {
        clock_faults_get_display_snapshot(&p_model->faults);
    }
}

void clock_service_note_sync_success(void)
{
    s_sync_successes++;
    s_live.wifi.state=CLOCK_DATA_READY;
    s_live.wifi.value=0;
}

uint8_t TEST_DRAW(uint8_t *p_frame,size_t length,
                  const clock_view_model_t *p_model)
{
    CHECK(s_renders<MAX_EVENTS);
    CHECK(length==TEST_FRAME_BYTES);
    CHECK(p_model->startup_ready==1U);
    s_render_models[s_renders++]=*p_model;
    memset(p_frame,0x5A,length);
    memcpy(p_frame,&p_model->calendar,sizeof(p_model->calendar));
    memcpy(p_frame+16U,&p_model->faults.revision,sizeof(uint32_t));
    memcpy(p_frame+20U,&p_model->format_revision,sizeof(uint32_t));
    p_frame[24U]=(uint8_t)p_model->memory_mode;
    p_frame[25U]=(uint8_t)p_model->calendar_state;
    p_frame[26U]=p_model->faults.active_categories;
    memcpy(p_frame+28U,&p_model->wifi,sizeof(p_model->wifi));
    return 1U;
}

epd_gdey029t94_status_t stm32_gdey029t94_make_io(
    epd_gdey029t94_io_t *p_io,stm32_gdey029t94_context_t *p_context)
{
    CHECK(p_io!=NULL && p_context!=NULL);
    s_bind_calls++;
    memset(p_io,0,sizeof(*p_io));
    p_io->handle=p_context;
    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t stm32_gdey029t94_prepare(stm32_gdey029t94_context_t *p_context)
{
    CHECK(p_context!=NULL);
    s_prepare_calls++;
    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t epd_gdey029t94_init_profile(
    epd_gdey029t94_t *p_device,const epd_gdey029t94_io_t *p_io,
    epd_gdey029t94_profile_t profile)
{
    CHECK(p_device!=NULL && p_io!=NULL);
    CHECK(profile==TEST_PROFILE);
    CHECK(s_live.startup_ready==1U);
    CHECK(p_device->initialized==0U || p_device->sleeping!=0U);
    s_init_calls++;
    p_device->io=*p_io;
    p_device->profile=profile;
    p_device->initialized=0U;
    p_device->sleeping=0U;
    p_device->background_valid=0U;
    p_device->fast_initialized=0U;
    p_device->refreshes_since_full=0U;
    if(scenario("init-retry") && s_init_calls<3U) { return EPD_GDEY029T94_ERROR_IO; }
    p_device->initialized=1U;
    return EPD_GDEY029T94_OK;
}

static epd_gdey029t94_status_t mock_refresh(
    epd_gdey029t94_t *p_device,const uint8_t *p_frame,uint32_t length,
    uint8_t partial)
{
    uint8_t fail=0U;
    uint32_t before=epd_bringup_get_completed_frames();
    CHECK(s_refreshes<MAX_EVENTS);
    CHECK(p_device->initialized==1U && p_device->sleeping==0U);
    CHECK(p_device->profile==TEST_PROFILE);
    CHECK(length==TEST_FRAME_BYTES);
    CHECK(before==s_committed);
    s_frame_hashes[s_refreshes]=frame_hash(p_frame,length);
    s_frame_ticks[s_refreshes]=s_tick;
    s_refreshes++;
    if(scenario("wifi-coalesce") && s_refreshes==1U) {
        /* Multiple successes while BUSY merge into one latest target. */
        clock_service_note_sync_success();
        s_live.wifi.value=1;
        clock_service_note_sync_success();
        CHECK(frame_hash(p_frame,length)==s_frame_hashes[0]);
        CHECK(epd_bringup_get_completed_frames()==before);
    } else if(scenario("wifi-revert") && s_refreshes==1U) {
        s_live.wifi.value=1;
        clock_service_note_sync_success();
        CHECK(frame_hash(p_frame,length)==s_frame_hashes[0]);
    } else if(debug_tx_scenario()) {
        /* A TX failure/recovery during BUSY must not request another frame. */
        if(s_refreshes==1U) {
            clock_fault_report(CLOCK_FAULT_DEBUG,CLOCK_FAULT_TX);
            clock_fault_report(CLOCK_FAULT_DEBUG,0U);
        } else {
            clock_fault_report(CLOCK_FAULT_DEBUG,0U);
            clock_fault_report(CLOCK_FAULT_DEBUG,CLOCK_FAULT_TX);
        }
    } else if(scenario("cycle-busy")) {
        if(s_refreshes==1U) {
            /* A packet at the old deadline wins while the old frame is BUSY. */
            s_tick=180000U;
            publish_weather(s_tick, 4U);
            publish_weather(s_tick, 5U);
        } else if(s_refreshes==2U) {
            s_tick=360000U;
        }
    } else if(scenario("retry-limit")) {
        fail=1U;
    } else if(scenario("retry-success") && s_refreshes<3U) {
        fail=1U;
        change_live(s_refreshes,1U);
    } else if(scenario("failed-fault-coalesce") || scenario("failed-recovery-coalesce")) {
        fail=(uint8_t)(s_refreshes<3U);
    } else if(scenario("retry-budget")) {
        fail=(uint8_t)(s_refreshes!=3U);
    } else if(scenario("coalesce")) {
        if(s_refreshes==1U) {
            s_tick+=1000U;change_live(1U,1U);
            s_tick+=1000U;change_live(2U,2U);
            s_live.calendar.minute=31U;
        } else if(s_refreshes==2U) {
            s_tick+=1000U;change_live(3U,0U);
            s_live.calendar.minute=32U;
        }
        /* Updates during the blocking call cannot commit the pending frame. */
        CHECK(epd_bringup_get_completed_frames()==before);
        CHECK(frame_hash(p_frame,length)==s_frame_hashes[s_refreshes-1U]);
    } else if(scenario("page-switch") && s_refreshes==1U) {
        CHECK(display_select_page(DISPLAY_PAGE_WEATHER));
        publish_weather(s_tick, 2U);
    } else if((scenario("weather-duplicate") || scenario("weather-minute") ||
               scenario("memory-phase-weather")) && s_refreshes==1U) {
        CHECK(display_select_page(DISPLAY_PAGE_WEATHER));
        publish_weather(s_tick, 2U);
    } else if(scenario("weather-duplicate") && s_refreshes==2U) {
        publish_weather(s_tick, 2U);
    } else if(scenario("memory-phase-weather") && s_refreshes==2U) {
        CHECK(display_select_page(DISPLAY_PAGE_CLOCK));
    } else if(scenario("fault-weather") && s_refreshes==1U) {
        CHECK(display_select_page(DISPLAY_PAGE_WEATHER));
        publish_weather(s_tick, 4U);
        change_live(1U, 1U);
    } else if(scenario("fault-weather") && s_refreshes==2U) {
        change_live(2U, 0U);
    } else if(scenario("page-revert") && s_refreshes==1U) {
        CHECK(display_select_page(DISPLAY_PAGE_WEATHER));
        CHECK(display_select_page(DISPLAY_PAGE_CLOCK));
    } else if(scenario("weather-revert") && s_refreshes==1U) {
        publish_weather(s_tick, 4U);
        publish_weather(s_tick, 2U);
    } else if(scenario("weather-coalesce") && s_refreshes==1U) {
        publish_weather(s_tick, 4U);
        publish_weather(s_tick, 5U);
    } else if(scenario("weather-retry")) {
        fail=(uint8_t)(s_refreshes<3U);
        if(fail!=0U) { publish_weather(s_tick, (uint8_t)(s_refreshes+3U)); }
    }
    CHECK(epd_bringup_get_completed_frames()==before);
    if(fail) {
        p_device->initialized=0U;
        p_device->background_valid=0U;
        p_device->fast_initialized=0U;
        return EPD_GDEY029T94_ERROR_BUSY_TIMEOUT;
    }
    if(s_renders!=0U) {
        s_committed_models[s_committed]=s_render_models[s_renders-1U];
    }
    s_committed++;
    if(partial!=0U) {
        p_device->partial_refreshes++;
        p_device->refreshes_since_full++;
    } else {
        p_device->full_refreshes++;
        p_device->refreshes_since_full=0U;
    }
#if !EPD_CLOCK_USE_GRAY4
    memcpy(p_device->background_image,p_frame,length);
    p_device->background_valid=1U;
#endif
    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t TEST_REFRESH(
    epd_gdey029t94_t *p_device,const uint8_t *p_frame,uint32_t length)
{
    return mock_refresh(p_device,p_frame,length,0U);
}

epd_gdey029t94_status_t epd_gdey029t94_partial_refresh(
    epd_gdey029t94_t *p_device,const epd_gdey029t94_rect_t *p_window,
    const uint8_t *p_image,uint32_t image_length)
{
    uint8_t frame[EPD_GDEY029T94_FRAME_BUFFER_SIZE];
    uint32_t row;
    uint32_t row_bytes;
    CHECK(p_device!=NULL && p_window!=NULL && p_image!=NULL);
    CHECK(p_device->profile==EPD_PROFILE_BW_296);
    CHECK(p_device->initialized==1U && p_device->sleeping==0U);
    CHECK(p_device->background_valid==1U);
    CHECK(p_window->width!=0U && p_window->height!=0U);
    CHECK(p_window->x%8U==0U && p_window->width%8U==0U);
    CHECK((uint32_t)p_window->x+p_window->width<=EPD_GDEY029T94_WIDTH_PIXELS);
    CHECK((uint32_t)p_window->y+p_window->height<=EPD_GDEY029T94_HEIGHT_PIXELS);
    row_bytes=p_window->width/8U;
    CHECK(image_length==row_bytes*p_window->height);
    memcpy(frame,p_device->background_image,sizeof(frame));
    for(row=0U;row<p_window->height;row++) {
        memcpy(frame+((uint32_t)p_window->y+row)*EPD_GDEY029T94_BYTES_PER_ROW+
                   p_window->x/8U,p_image+row*row_bytes,row_bytes);
    }
    return mock_refresh(p_device,frame,sizeof(frame),1U);
}

epd_gdey029t94_status_t epd_gdey029t94_get_refresh_stats(
    const epd_gdey029t94_t *p_device,epd_gdey029t94_refresh_stats_t *p_stats)
{
    CHECK(p_device!=NULL && p_stats!=NULL);
    p_stats->full_refreshes=p_device->full_refreshes;
    p_stats->fast_refreshes=p_device->fast_refreshes;
    p_stats->partial_refreshes=p_device->partial_refreshes;
    p_stats->refreshes_since_full=p_device->refreshes_since_full;
    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t epd_gdey029t94_sleep(epd_gdey029t94_t *p_device)
{
    CHECK(p_device!=NULL);
    CHECK(p_device->initialized==1U && p_device->sleeping==0U);
    s_sleeps++;
    CHECK(epd_bringup_get_completed_frames()==s_committed);
    CHECK(s_sleeps==s_committed);
    if(scenario("sleep-failure") && s_sleeps==1U) {
        p_device->initialized=0U;
        p_device->background_valid=0U;
        return EPD_GDEY029T94_ERROR_IO;
    }
    p_device->sleeping=1U;
    return EPD_GDEY029T94_OK;
}

void Debug_Log(const char *p_message)
{
    CHECK(p_message!=NULL);
}

TickType_t xTaskGetTickCount(void) { return s_tick; }

osThreadId_t osThreadNew(osThreadFunc_t function,void *p_argument,
                         const osThreadAttr_t *p_attributes)
{
    CHECK(function!=NULL && p_attributes!=NULL);
    CHECK(p_attributes->cb_mem!=NULL && p_attributes->cb_size>0U);
    CHECK(p_attributes->stack_mem!=NULL && p_attributes->stack_size==2048U);
    CHECK(p_attributes->priority==osPriorityNormal);
    CHECK(strcmp(p_attributes->name,"displayTask")==0);
    s_thread_creates++;
    s_thread_function=function;s_thread_argument=p_argument;
    return (osThreadId_t)&s_thread_function;
}

osThreadId_t osThreadGetId(void) { return (osThreadId_t)&s_thread_function; }

int32_t osThreadSuspend(osThreadId_t thread)
{
    CHECK(thread==osThreadGetId());
    s_suspends++;
    longjmp(s_stop,STOP_SUSPENDED);
    return 0;
}

int32_t osDelay(uint32_t ticks)
{
    CHECK(ticks==200U || ticks==1000U);
    if(ticks==1000U) {
        CHECK(s_retry_delays<MAX_EVENTS);
        s_delay_ticks[s_retry_delays++]=s_tick;
    } else {
        s_poll_delays++;
    }
    s_tick+=ticks;
    if(scenario("wifi-visible")) {
        if(s_poll_delays==1U || s_poll_delays==2U ||
           s_poll_delays==4U || s_poll_delays==5U) {
            clock_service_note_sync_success();
        } else if(s_poll_delays==3U) {
            CHECK(s_refreshes==2U);
            s_live.wifi.value=1;
        } else if(s_poll_delays==6U) {
            CHECK(s_refreshes==4U);
            s_live.wifi.state=CLOCK_DATA_ERROR;
        } else if(s_poll_delays==7U) {
            s_live.wifi.state=CLOCK_DATA_UNAVAILABLE;
        } else if(s_poll_delays==8U) {
            CHECK(s_refreshes==5U);
            longjmp(s_stop,STOP_COMPLETE);
        }
    } else if(scenario("wifi-saturated")) {
        if(s_poll_delays==1U) { s_live.wifi.value=100; }
        else if(s_poll_delays==2U) { s_live.wifi.value=101; }
        else if(s_poll_delays==3U) {
            CHECK(s_refreshes==2U);
            s_live.wifi.value=100;
        } else if(s_poll_delays==4U) {
            CHECK(s_refreshes==2U);
            clock_service_note_sync_success();
        } else if(s_poll_delays==5U) {
            longjmp(s_stop,STOP_COMPLETE);
        }
    } else if(scenario("wifi-weather-hidden") || scenario("wifi-fault-hidden")) {
        if(s_poll_delays<=4U) { CHECK(s_refreshes==1U); }
        if(s_poll_delays==1U || s_poll_delays==3U) {
            clock_service_note_sync_success();
        } else if(s_poll_delays==2U) {
            s_live.wifi.value=100;
        } else if(s_poll_delays==4U) {
            if(scenario("wifi-weather-hidden")) {
                CHECK(display_select_page(DISPLAY_PAGE_CLOCK));
            } else {
                s_live.faults.active_categories=0U;
                s_live.faults.revision++;
            }
        } else if(s_poll_delays==5U) {
            longjmp(s_stop,STOP_COMPLETE);
        }
    } else if(scenario("wifi-coalesce") || scenario("wifi-revert")) {
        CHECK(s_refreshes==(scenario("wifi-coalesce") ?
            (s_poll_delays==1U ? 1U : 2U) : 1U));
        if(s_poll_delays==4U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(debug_tx_scenario()) {
        CHECK(s_refreshes==(s_poll_delays<=4U ? 1U : 2U));
        if(s_poll_delays==1U || s_poll_delays==3U) {
            clock_fault_report(CLOCK_FAULT_DEBUG,CLOCK_FAULT_TX);
        } else if(s_poll_delays==2U || s_poll_delays==5U) {
            clock_fault_report(CLOCK_FAULT_DEBUG,0U);
        } else if(s_poll_delays==4U) {
            /* TX remains active across the normal minute refresh. */
            s_live.calendar.minute=31U;
        } else if(s_poll_delays==6U) {
            clock_fault_report(CLOCK_FAULT_DEBUG,CLOCK_FAULT_TX);
            clock_fault_report(CLOCK_FAULT_DEBUG,0U);
        } else if(s_poll_delays==8U) {
            longjmp(s_stop,STOP_COMPLETE);
        }
    } else if(scenario("startup")) {
        if(s_poll_delays==5U) {
            CHECK(s_refreshes==0U && s_renders==0U && s_init_calls==0U);
            s_live.startup_ready=1U;
        }
        if(s_committed==1U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("minute")) {
        s_live.calendar.second=(uint8_t)((s_tick/1000U)%60U);
        if(s_tick==10000U) { s_live.temperature.value=100; }
        if(s_tick>=60000U) { s_live.calendar.minute=31U; }
        if(s_committed==2U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("time-jump") || scenario("weather-time-jump")) {
        if(s_poll_delays==1U) { s_live.calendar.second=59U; }
        if(s_poll_delays==2U) {
            CHECK(s_refreshes==1U);
            s_live.calendar=(clock_calendar_t){2027U,1U,1U,5U,21U,30U,0U};
        }
        if(s_poll_delays==3U) {
            CHECK(s_committed==2U);
            s_live.calendar=(clock_calendar_t){2000U,1U,1U,6U,21U,30U,20U};
        }
        if(s_poll_delays==4U) {
            CHECK(s_committed==3U);
            longjmp(s_stop,STOP_COMPLETE);
        }
    } else if(scenario("rtc-error")) {
        if(s_tick>=2000U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("retry-success")) {
        if(s_committed==1U && s_tick>=4000U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("failed-fault-coalesce")) {
        if(ticks==1000U && s_retry_delays==1U) {
            change_live(1U,1U);change_live(2U,2U);change_live(3U,1U);
        } else if(ticks==1000U && s_retry_delays==2U) {
            change_live(4U,2U);change_live(5U,0U);change_live(6U,2U);
        }
        if(s_committed==1U && s_tick>=4000U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("failed-recovery-coalesce")) {
        if(ticks==1000U && s_retry_delays==1U) {
            change_live(2U,2U);change_live(3U,0U);
        } else if(ticks==1000U && s_retry_delays==2U) {
            change_live(4U,1U);change_live(5U,0U);
        }
        if(s_committed==1U && s_tick>=4000U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("init-retry")) {
        if(s_committed==1U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("retry-budget")) {
        if(s_refreshes==3U && s_committed==1U) { change_live(1U,0U); }
    } else if(scenario("sleep-failure")) {
        if(s_committed==1U) {
            CHECK(display_get_last_status()==EPD_GDEY029T94_ERROR_IO);
            s_sleep_error_observed=1U;
            s_live.calendar.minute=31U;
        }
        if(s_committed==2U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("coalesce")) {
        if(s_committed==3U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("page-switch")) {
        if(s_committed==2U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("weather-duplicate")) {
        if(s_poll_delays>=2U) { CHECK(s_refreshes==2U); }
        if(s_poll_delays==5U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("weather-minute")) {
        if(s_poll_delays==2U) { s_live.calendar.minute=31U; }
        if(s_committed==3U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("memory-phase-weather")) {
        if(s_committed==3U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("weather-auto-page")) {
        if(s_poll_delays==1U) {
            publish_weather(s_tick, 1U);
            CHECK(s_refreshes==1U);
        }
        if(s_committed==2U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("cycle-busy")) {
        if(s_committed==3U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("cycle-fault")) {
        display_snapshot_t snapshot;
        if(s_poll_delays<=4U) { CHECK(s_refreshes==1U); }
        if(s_poll_delays==1U) {
            s_tick=180000U;
            display_get_snapshot(s_tick, &snapshot);
            CHECK(snapshot.page==DISPLAY_PAGE_WEATHER);
        } else if(s_poll_delays==2U) {
            s_tick=360000U;
            display_get_snapshot(s_tick, &snapshot);
            CHECK(snapshot.page==DISPLAY_PAGE_CLOCK);
        } else if(s_poll_delays==3U) {
            s_tick=540000U;
            display_get_snapshot(s_tick, &snapshot);
            CHECK(snapshot.page==DISPLAY_PAGE_WEATHER);
        } else if(s_poll_delays==4U) {
            s_live.faults.revision++;
            s_live.faults.active_categories=0U;
        }
        if(s_committed==2U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("fault-weather")) {
        if(s_committed==3U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("fault-page-hidden")) {
        if(s_poll_delays<=4U) { CHECK(s_refreshes==1U); }
        if(s_poll_delays==1U) {
            CHECK(display_select_page(DISPLAY_PAGE_WEATHER));
            publish_weather(s_tick, 2U);
        } else if(s_poll_delays==2U) {
            CHECK(display_select_page(DISPLAY_PAGE_CLOCK));
            CHECK(display_select_page(DISPLAY_PAGE_WEATHER));
        } else if(s_poll_delays==4U) {
            s_live.faults.revision++;
            s_live.faults.active_categories=0U;
        }
        if(s_committed==2U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("page-revert") || scenario("weather-revert")) {
        CHECK(s_refreshes==1U);
        if(s_poll_delays==5U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("weather-coalesce")) {
        if(s_poll_delays>=2U) { CHECK(s_refreshes==2U); }
        if(s_poll_delays==5U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("weather-retry")) {
        if(s_committed==1U && s_tick>=2400U) { longjmp(s_stop,STOP_COMPLETE); }
    } else if(scenario("weather-expiry")) {
        if(s_poll_delays<=2U) { CHECK(s_refreshes==1U); }
        if(s_poll_delays==1U) {
            s_tick=10800000UL-1U;
            /* Isolate the expiry redraw from the independent page timer. */
            CHECK(display_select_page(DISPLAY_PAGE_WEATHER));
        }
        if(s_poll_delays==2U) { s_tick=10800000UL; }
        if(s_poll_delays==4U) {
            weather_snapshot_t snapshot;
            weather_get_snapshot(s_tick, &snapshot);
            CHECK(snapshot.available==1U && snapshot.expired==1U);
            CHECK(s_refreshes==2U);
            longjmp(s_stop,STOP_COMPLETE);
        }
    }
    CHECK(s_tick<(scenario("weather-expiry") ? 10802000UL :
        ((scenario("cycle-busy") || scenario("cycle-fault")) ? 600000UL : 70000UL)));
    return 0;
}

static void init_model(void)
{
    clock_faults_init();
    memset(&s_live,0,sizeof(s_live));
    s_live.startup_ready=1U;
    s_live.format=CLOCK_FORMAT_24;
    s_live.calendar_state=CLOCK_DATA_READY;
    s_live.calendar=(clock_calendar_t){2026U,10U,18U,7U,21U,30U,0U};
    if(scenario("wifi-saturated") || scenario("wifi-revert")) {
        s_live.wifi.state=CLOCK_DATA_READY;
        s_live.wifi.value=scenario("wifi-saturated") ? 99 : 0;
    }
    if(scenario("startup")) { s_live.startup_ready=0U; }
    if(scenario("rtc-error")) {
        s_live.calendar_state=CLOCK_DATA_ERROR;
        s_live.faults.revision=1U;
        s_live.faults.active_categories=1U;
    }
    if(scenario("failed-recovery-coalesce")) { change_live(1U,1U); }
    if(scenario("fault-page-hidden") || scenario("cycle-fault") ||
       scenario("wifi-fault-hidden")) {
        s_live.faults.revision=1U;
        s_live.faults.active_categories=1U;
    }
    if(scenario("weather-revert") || scenario("weather-coalesce") ||
       scenario("weather-retry") || scenario("weather-expiry") ||
       scenario("weather-time-jump") || scenario("debug-tx-weather") ||
       scenario("wifi-weather-hidden")) {
        CHECK(display_select_page(DISPLAY_PAGE_WEATHER));
        publish_weather(s_tick, 2U);
    }
}

static void check_retry_intervals(uint32_t expected)
{
    uint32_t i;
    CHECK(s_retry_delays==expected);
    for(i=0U;i<expected;i++) {
        CHECK(s_delay_ticks[i]%200U==0U);
    }
}

static void check_weather_frame(uint32_t index, uint8_t condition, uint8_t expired)
{
    weather_snapshot_t weather;
    uint8_t expected_frame[TEST_FRAME_BYTES];
    weather_get_snapshot(s_tick, &weather);
    CHECK(weather.data.condition==condition && weather.expired==expired);
#if EPD_CLOCK_USE_GRAY4
    CHECK(epd_weather_view_draw_gray4(expected_frame, sizeof(expected_frame),
                                      &weather, &s_live));
#else
    CHECK(epd_weather_view_draw_bw(expected_frame, sizeof(expected_frame),
                                   &weather, &s_live));
#endif
    CHECK(s_frame_hashes[index]==frame_hash(expected_frame, sizeof(expected_frame)));
}

static void verify(int stopped)
{
    uint32_t i;
    CHECK(s_thread_creates==1U);
    CHECK(s_bind_calls==s_prepare_calls);
    CHECK(epd_bringup_get_completed_frames()==s_committed);
    if(scenario("wifi-visible")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==5U && s_committed==5U);
        CHECK(s_sync_successes==4U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U &&
              s_frame_ticks[2]==600U && s_frame_ticks[3]==800U &&
              s_frame_ticks[4]==1200U);
        CHECK(s_render_models[0].wifi.state==CLOCK_DATA_UNAVAILABLE);
        CHECK(s_render_models[1].wifi.state==CLOCK_DATA_READY &&
              s_render_models[1].wifi.value==0);
        CHECK(s_render_models[2].wifi.value==1);
        CHECK(s_render_models[3].wifi.value==0);
        CHECK(s_render_models[4].wifi.state==CLOCK_DATA_ERROR);
        check_retry_intervals(0U);
    } else if(scenario("wifi-saturated")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==3U && s_committed==3U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U && s_frame_ticks[2]==800U);
        CHECK(s_render_models[0].wifi.value==99 && s_render_models[1].wifi.value==100);
        CHECK(s_render_models[2].wifi.value==0);
        check_retry_intervals(0U);
    } else if(scenario("wifi-weather-hidden") || scenario("wifi-fault-hidden")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==2U && s_committed==2U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==800U);
        CHECK(s_render_models[s_renders-1U].wifi.state==CLOCK_DATA_READY);
        CHECK(s_render_models[s_renders-1U].wifi.value==0);
        CHECK(s_renders==(scenario("wifi-weather-hidden") ? 1U : 2U));
        check_retry_intervals(0U);
    } else if(scenario("wifi-coalesce")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==2U && s_committed==2U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U);
        CHECK(s_render_models[0].wifi.state==CLOCK_DATA_UNAVAILABLE);
        CHECK(s_committed_models[0].wifi.state==CLOCK_DATA_UNAVAILABLE);
        CHECK(s_render_models[1].wifi.state==CLOCK_DATA_READY &&
              s_render_models[1].wifi.value==0);
        CHECK(s_sync_successes==2U);
        check_retry_intervals(0U);
    } else if(scenario("wifi-revert")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==1U && s_committed==1U);
        CHECK(s_render_models[0].wifi.value==0 && s_live.wifi.value==0);
        CHECK(s_sync_successes==1U);
        check_retry_intervals(0U);
    } else if(debug_tx_scenario()) {
        clock_fault_snapshot_t diagnostics;
        display_snapshot_t snapshot;
        CHECK(stopped==STOP_COMPLETE && s_refreshes==2U && s_committed==2U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==800U);
        clock_faults_get_snapshot(&diagnostics);
        CHECK(diagnostics.revision==10U && diagnostics.active_categories==0U);
        CHECK(diagnostics.source[CLOCK_FAULT_DEBUG].first_mask==CLOCK_FAULT_TX);
        display_get_snapshot(s_tick,&snapshot);
        CHECK(snapshot.clock.faults.revision==0U);
        CHECK(snapshot.clock.faults.active_categories==0U);
        if(scenario("debug-tx-weather")) {
            CHECK(snapshot.page==DISPLAY_PAGE_WEATHER);
            CHECK(s_renders==0U && s_weather_published==1U);
            check_weather_frame(1U,2U,0U);
        } else {
            CHECK(snapshot.page==DISPLAY_PAGE_CLOCK && s_renders==2U);
            for(i=0U;i<2U;i++) {
                CHECK(s_render_models[i].faults.revision==0U);
                CHECK(s_render_models[i].faults.active_categories==0U);
            }
            CHECK(s_render_models[1].memory_mode==CLOCK_MEMORY_IFLASH);
        }
        check_retry_intervals(0U);
    } else if(scenario("startup")) {
        CHECK(stopped==STOP_COMPLETE);CHECK(s_init_calls==1U && s_renders==1U);
        CHECK(s_frame_ticks[0]==1000U);check_retry_intervals(0U);
    } else if(scenario("minute")) {
        CHECK(stopped==STOP_COMPLETE);CHECK(s_refreshes==2U && s_renders==2U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==60000U);
        CHECK(s_render_models[0].calendar.second==0U);
        CHECK(s_render_models[1].calendar.minute==31U);
        CHECK(s_render_models[1].memory_mode==CLOCK_MEMORY_IFLASH);
        check_retry_intervals(0U);
    } else if(scenario("time-jump") || scenario("weather-time-jump")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==3U && s_committed==3U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==400U && s_frame_ticks[2]==600U);
        if(scenario("time-jump")) {
            CHECK(s_renders==3U);
            CHECK(s_render_models[1].calendar.year==2027U);
            CHECK(s_render_models[2].calendar.year==2000U);
            CHECK(s_render_models[2].calendar.second==20U);
        } else {
            weather_snapshot_t weather;
            CHECK(s_weather_published==1U && s_renders==0U);
            weather_get_snapshot(s_tick, &weather);
            CHECK(weather.available==1U && weather.expired==0U);
            CHECK(weather.data.condition==2U);
        }
        check_retry_intervals(0U);
    } else if(scenario("rtc-error")) {
        CHECK(stopped==STOP_COMPLETE);CHECK(s_refreshes==1U && s_renders==1U);
        CHECK(s_render_models[0].calendar_state==CLOCK_DATA_ERROR);
        CHECK(s_init_calls==1U && s_bind_calls==1U);check_retry_intervals(0U);
    } else if(scenario("retry-limit")) {
        CHECK(stopped==STOP_SUSPENDED);CHECK(s_suspends==1U);
        CHECK(s_refreshes==3U && s_init_calls==3U);
        CHECK(s_committed==0U && s_sleeps==0U && s_renders==3U);
        CHECK(s_frame_hashes[0]==s_frame_hashes[1] && s_frame_hashes[1]==s_frame_hashes[2]);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==1000U && s_frame_ticks[2]==2000U);
        CHECK(display_get_last_status()==EPD_GDEY029T94_ERROR_BUSY_TIMEOUT);
        check_retry_intervals(2U);
    } else if(scenario("retry-success")) {
        CHECK(stopped==STOP_COMPLETE);CHECK(s_refreshes==3U && s_renders==3U);
        CHECK(s_committed==1U && s_sleeps==1U);
        CHECK(s_frame_hashes[0]!=s_frame_hashes[1] && s_frame_hashes[1]!=s_frame_hashes[2]);
        CHECK(s_render_models[0].faults.revision==0U);
        CHECK(s_render_models[1].faults.revision==1U);
        CHECK(s_render_models[2].faults.revision==2U);
        CHECK(s_committed_models[0].faults.revision==2U);
        CHECK(s_committed_models[0].faults.active_categories==1U);
        CHECK(s_committed_models[0].format_revision==2U);
        for(i=0U;i<3U;i++) { CHECK(s_render_models[i].memory_mode==CLOCK_MEMORY_SRAM); }
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==1000U && s_frame_ticks[2]==2000U);
        check_retry_intervals(2U);
    } else if(scenario("failed-fault-coalesce") || scenario("failed-recovery-coalesce")) {
        CHECK(stopped==STOP_COMPLETE);CHECK(s_refreshes==3U && s_renders==3U);
        CHECK(s_committed==1U);
        CHECK(s_sleeps==(scenario("failed-fault-coalesce") ? 1U :
            COMPLETED_SEQUENCE_SLEEPS(1U)));
        CHECK(s_frame_hashes[0]!=s_frame_hashes[1] && s_frame_hashes[1]!=s_frame_hashes[2]);
        CHECK(s_render_models[1].faults.revision==3U);
        if(scenario("failed-fault-coalesce")) {
            CHECK(s_render_models[0].faults.revision==0U);
            CHECK(s_committed_models[0].faults.revision==6U);
            CHECK(s_committed_models[0].format_revision==6U);
            CHECK(s_committed_models[0].faults.active_categories==2U);
        } else {
            CHECK(s_render_models[0].faults.revision==1U);
            CHECK(s_committed_models[0].faults.revision==5U);
            CHECK(s_committed_models[0].format_revision==5U);
            CHECK(s_committed_models[0].faults.active_categories==0U);
            CHECK(s_committed_models[0].format==CLOCK_FORMAT_12);
        }
        for(i=0U;i<3U;i++) { CHECK(s_render_models[i].memory_mode==CLOCK_MEMORY_SRAM); }
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==1000U && s_frame_ticks[2]==2000U);
        check_retry_intervals(2U);
    } else if(scenario("init-retry")) {
        CHECK(stopped==STOP_COMPLETE);CHECK(s_init_calls==3U && s_refreshes==1U);
        CHECK(s_renders==3U && s_committed==1U);CHECK(s_frame_ticks[0]==2000U);
        check_retry_intervals(2U);
    } else if(scenario("retry-budget")) {
        CHECK(stopped==STOP_SUSPENDED);CHECK(s_refreshes==6U && s_renders==6U);
        CHECK(s_committed==1U && s_sleeps==1U);
        for(i=1U;i<3U;i++) { CHECK(s_frame_hashes[0]==s_frame_hashes[i]); }
        for(i=4U;i<6U;i++) { CHECK(s_frame_hashes[3]==s_frame_hashes[i]); }
        for(i=0U;i<3U;i++) { CHECK(s_render_models[i].memory_mode==CLOCK_MEMORY_SRAM); }
        for(i=3U;i<6U;i++) { CHECK(s_render_models[i].memory_mode==CLOCK_MEMORY_IFLASH); }
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==1000U && s_frame_ticks[2]==2000U);
        CHECK(s_frame_ticks[3]==2200U && s_frame_ticks[4]==3200U && s_frame_ticks[5]==4200U);
        check_retry_intervals(4U);
    } else if(scenario("sleep-failure")) {
        CHECK(stopped==STOP_COMPLETE);CHECK(s_refreshes==2U && s_committed==2U);
        CHECK(s_sleep_error_observed==1U);CHECK(s_init_calls==2U && s_bind_calls==1U);
        CHECK(s_render_models[1].memory_mode==CLOCK_MEMORY_IFLASH);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U);check_retry_intervals(0U);
    } else if(scenario("coalesce")) {
        CHECK(stopped==STOP_COMPLETE);CHECK(s_refreshes==3U && s_renders==3U);
        CHECK(s_committed==3U && s_sleeps==COMPLETED_SEQUENCE_SLEEPS(3U));
        CHECK(s_render_models[0].faults.revision==0U);
        CHECK(s_render_models[1].faults.revision==2U && s_render_models[1].format_revision==2U);
        CHECK(s_render_models[2].faults.revision==3U && s_render_models[2].format_revision==3U);
        CHECK(s_render_models[1].faults.active_categories==2U);
        CHECK(s_render_models[2].faults.active_categories==0U);
        CHECK(s_render_models[0].memory_mode==CLOCK_MEMORY_SRAM);
        CHECK(s_render_models[1].memory_mode==CLOCK_MEMORY_IFLASH);
        CHECK(s_render_models[2].memory_mode==CLOCK_MEMORY_IFLASH);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==2200U && s_frame_ticks[2]==3400U);
        check_retry_intervals(0U);
    } else if(scenario("page-switch")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==2U && s_committed==2U);
        CHECK(s_weather_published==1U);
        CHECK(s_frame_hashes[0]!=s_frame_hashes[1]);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U);
        check_retry_intervals(0U);
    } else if(scenario("weather-duplicate")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==2U && s_committed==2U);
        CHECK(s_weather_published==2U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U);
        CHECK(s_frame_hashes[0]!=s_frame_hashes[1]);
        check_retry_intervals(0U);
    } else if(scenario("weather-minute")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==3U && s_committed==3U);
        CHECK(s_weather_published==1U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U && s_frame_ticks[2]==400U);
        CHECK(s_frame_hashes[1]!=s_frame_hashes[2]);
        check_retry_intervals(0U);
    } else if(scenario("memory-phase-weather")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==3U && s_committed==3U);
        CHECK(s_weather_published==1U && s_renders==2U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U && s_frame_ticks[2]==400U);
        CHECK(s_render_models[1].memory_mode==CLOCK_MEMORY_IFLASH);
        check_retry_intervals(0U);
    } else if(scenario("weather-auto-page")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==2U && s_committed==2U);
        CHECK(s_weather_published==1U);
        CHECK(s_frame_hashes[0]!=s_frame_hashes[1]);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U);
        check_weather_frame(1U, 1U, 0U);
        check_retry_intervals(0U);
    } else if(scenario("cycle-busy")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==3U && s_committed==3U);
        CHECK(s_weather_published==2U && s_renders==2U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==180200U &&
              s_frame_ticks[2]==360200U);
        check_weather_frame(1U, 5U, 0U);
        check_retry_intervals(0U);
    } else if(scenario("cycle-fault")) {
        display_snapshot_t snapshot;
        CHECK(stopped==STOP_COMPLETE && s_refreshes==2U && s_committed==2U);
        CHECK(s_renders==1U && s_weather_published==0U);
        CHECK(s_render_models[0].faults.active_categories==1U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==540200U);
        display_get_snapshot(s_tick, &snapshot);
        CHECK(snapshot.page==DISPLAY_PAGE_WEATHER && snapshot.weather.available==0U);
        check_retry_intervals(0U);
    } else if(scenario("fault-weather")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==3U && s_committed==3U);
        CHECK(s_weather_published==1U && s_renders==2U);
        CHECK(s_frame_hashes[0]!=s_frame_hashes[1]);
        CHECK(s_frame_hashes[1]!=s_frame_hashes[2]);
        CHECK(s_render_models[1].faults.active_categories==1U);
        CHECK(s_render_models[1].faults.revision==1U);
        check_retry_intervals(0U);
    } else if(scenario("fault-page-hidden")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==2U && s_committed==2U);
        CHECK(s_renders==1U && s_weather_published==1U);
        CHECK(s_render_models[0].faults.active_categories==1U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==800U);
        CHECK(s_frame_hashes[0]!=s_frame_hashes[1]);
        check_weather_frame(1U, 2U, 0U);
        check_retry_intervals(0U);
    } else if(scenario("page-revert") || scenario("weather-revert")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==1U && s_committed==1U);
        CHECK(s_weather_published==(scenario("weather-revert") ? 3U : 0U));
        CHECK(s_frame_ticks[0]==0U);
        if(scenario("weather-revert")) { check_weather_frame(0U, 2U, 0U); }
        check_retry_intervals(0U);
    } else if(scenario("weather-coalesce")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==2U && s_committed==2U);
        CHECK(s_weather_published==3U && s_renders==0U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U);
        CHECK(s_frame_hashes[0]!=s_frame_hashes[1]);
        check_weather_frame(1U, 5U, 0U);
        check_retry_intervals(0U);
    } else if(scenario("weather-retry")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==3U && s_committed==1U);
        CHECK(s_weather_published==3U && s_renders==0U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==1000U && s_frame_ticks[2]==2000U);
        CHECK(s_frame_hashes[0]!=s_frame_hashes[1] && s_frame_hashes[1]!=s_frame_hashes[2]);
        check_weather_frame(2U, 5U, 0U);
        check_retry_intervals(2U);
    } else if(scenario("weather-expiry")) {
        CHECK(stopped==STOP_COMPLETE && s_refreshes==2U && s_committed==2U);
        CHECK(s_weather_published==1U && s_renders==0U);
        CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==10800000UL);
        CHECK(s_frame_hashes[0]!=s_frame_hashes[1]);
        check_weather_frame(1U, 2U, 1U);
        check_retry_intervals(0U);
    } else {
        CHECK(0);
    }
}

static int cycle_contract_scenario(void)
{
    return scenario("cycle-default") || scenario("cycle-weather") ||
        scenario("cycle-manual") || scenario("cycle-wrap") ||
        scenario("cycle-time") || scenario("cycle-expired") ||
        scenario("cycle-late");
}

static void check_selected_page(uint32_t tick, display_page_t expected)
{
    display_snapshot_t snapshot;
    s_tick=tick;
    display_get_snapshot(tick, &snapshot);
    CHECK(snapshot.page==expected);
}

static void verify_cycle_contract(void)
{
    display_snapshot_t snapshot;
    uint32_t initial=s_tick;

    check_selected_page(initial, DISPLAY_PAGE_CLOCK);
    if(scenario("cycle-default") || scenario("cycle-wrap")) {
        check_selected_page(initial+179999U, DISPLAY_PAGE_CLOCK);
        check_selected_page(initial+180000U, DISPLAY_PAGE_WEATHER);
        display_get_snapshot(s_tick, &snapshot);
        CHECK(snapshot.weather.available==0U);
        check_selected_page(initial+359999U, DISPLAY_PAGE_WEATHER);
        check_selected_page(initial+360000U, DISPLAY_PAGE_CLOCK);
    } else if(scenario("cycle-weather")) {
        s_tick=1000U;
        publish_weather(s_tick, 2U);
        check_selected_page(1000U, DISPLAY_PAGE_WEATHER);
        check_selected_page(180000U, DISPLAY_PAGE_WEATHER);
        check_selected_page(180999U, DISPLAY_PAGE_WEATHER);
        check_selected_page(181000U, DISPLAY_PAGE_CLOCK);
        s_tick=181001U;
        publish_weather(s_tick, 5U);
        check_selected_page(181001U, DISPLAY_PAGE_WEATHER);
        s_tick=350000U;
        publish_weather(s_tick, 5U);
        check_selected_page(361001U, DISPLAY_PAGE_WEATHER);
        check_selected_page(529999U, DISPLAY_PAGE_WEATHER);
        CHECK(!display_publish_weather(NULL, s_tick));
        check_selected_page(530000U, DISPLAY_PAGE_CLOCK);
        display_get_snapshot(s_tick, &snapshot);
        CHECK(snapshot.weather.available==1U && snapshot.weather.expired==0U);
        CHECK(snapshot.weather.data.condition==5U && s_weather_published==3U);
    } else if(scenario("cycle-manual")) {
        s_tick=179999U;
        CHECK(display_select_page(DISPLAY_PAGE_CLOCK));
        check_selected_page(180000U, DISPLAY_PAGE_CLOCK);
        check_selected_page(359998U, DISPLAY_PAGE_CLOCK);
        check_selected_page(359999U, DISPLAY_PAGE_WEATHER);
        s_tick=360000U;
        CHECK(display_select_page(DISPLAY_PAGE_WEATHER));
        check_selected_page(539999U, DISPLAY_PAGE_WEATHER);
        CHECK(!display_select_page((display_page_t)2U));
        check_selected_page(540000U, DISPLAY_PAGE_CLOCK);
    } else if(scenario("cycle-time")) {
        s_live.calendar=(clock_calendar_t){2099U,12U,31U,4U,23U,59U,59U};
        check_selected_page(90000U, DISPLAY_PAGE_CLOCK);
        s_live.calendar=(clock_calendar_t){2000U,1U,1U,6U,0U,0U,0U};
        check_selected_page(179999U, DISPLAY_PAGE_CLOCK);
        check_selected_page(180000U, DISPLAY_PAGE_WEATHER);
        s_live.calendar=(clock_calendar_t){2026U,10U,28U,3U,15U,0U,0U};
        check_selected_page(359999U, DISPLAY_PAGE_WEATHER);
        check_selected_page(360000U, DISPLAY_PAGE_CLOCK);
    } else if(scenario("cycle-expired")) {
        publish_weather(0U, 2U);
        s_tick=10800000U;
        CHECK(display_select_page(DISPLAY_PAGE_WEATHER));
        check_selected_page(s_tick, DISPLAY_PAGE_WEATHER);
        display_get_snapshot(s_tick, &snapshot);
        CHECK(snapshot.weather.available==1U && snapshot.weather.expired==1U);
        check_selected_page(10980000U, DISPLAY_PAGE_CLOCK);
        check_selected_page(11160000U, DISPLAY_PAGE_WEATHER);
        display_get_snapshot(s_tick, &snapshot);
        CHECK(snapshot.weather.available==1U && snapshot.weather.expired==1U);
    } else if(scenario("cycle-late")) {
        /* One delayed poll produces one switch, then a full new interval. */
        check_selected_page(900000U, DISPLAY_PAGE_WEATHER);
        check_selected_page(1079999U, DISPLAY_PAGE_WEATHER);
        check_selected_page(1080000U, DISPLAY_PAGE_CLOCK);
        /* A captured tick older than the current anchor cannot undo it. */
        check_selected_page(0U, DISPLAY_PAGE_CLOCK);
        check_selected_page(1259999U, DISPLAY_PAGE_CLOCK);
        check_selected_page(1260000U, DISPLAY_PAGE_WEATHER);
    } else {
        CHECK(0);
    }
    CHECK(s_thread_creates==1U && s_refreshes==0U);
    printf("PASS: page rotation gray4=%u scenario %s (%lu checks)\n",
        (unsigned int)EPD_CLOCK_USE_GRAY4,s_scenario,s_checks);
}

static void verify_weather_sync_contract(void)
{
    weather_data_t weather;
    display_snapshot_t before;
    display_snapshot_t after;
    CHECK(!display_publish_weather(NULL, s_tick));
    CHECK(s_sync_successes==0U);
    CHECK(weather_parse_line(
        "WEATHER \xE6\xB7\xB1\xE5\x9C\xB3,2,235,180,270,65,2,35,2026-10-28T14:30",
        &weather));
    CHECK(display_publish_weather(&weather, s_tick));
    display_get_snapshot(s_tick, &before);
    CHECK(s_sync_successes==1U);
    CHECK(before.page==DISPLAY_PAGE_WEATHER && before.weather.available==1U);
    CHECK(before.clock.wifi.state==CLOCK_DATA_READY && before.clock.wifi.value==0);
    s_tick=1000U;
    CHECK(display_publish_weather(&weather, s_tick));
    display_get_snapshot(s_tick, &after);
    CHECK(s_sync_successes==2U);
    CHECK(after.weather.revision==before.weather.revision);
    CHECK(after.clock.wifi.state==CLOCK_DATA_READY && after.clock.wifi.value==0);
    CHECK(display_select_page(DISPLAY_PAGE_CLOCK));
    weather.humidity=101U;
    CHECK(!display_publish_weather(&weather, s_tick));
    display_get_snapshot(s_tick, &after);
    CHECK(s_sync_successes==2U && after.page==DISPLAY_PAGE_CLOCK);
    CHECK(after.weather.data.humidity==65U);
    CHECK(s_thread_creates==1U && s_refreshes==0U);
    printf("PASS: WEATHER sync commit gray4=%u (%lu checks)\n",
        (unsigned int)EPD_CLOCK_USE_GRAY4,s_checks);
}

#if !EPD_CLOCK_USE_GRAY4
static void verify_run_once_sleep_failure(void)
{
    CHECK(epd_bringup_run_once()==EPD_GDEY029T94_OK);
    CHECK(epd_bringup_get_completed_frames()==1U);
    CHECK(epd_bringup_get_last_status()==EPD_GDEY029T94_ERROR_IO);
    CHECK(s_refreshes==1U && s_committed==1U && s_sleeps==1U);
    CHECK(s_render_models[0].memory_mode==CLOCK_MEMORY_SRAM);
    s_tick=200U;
    s_live.calendar.minute=31U;
    CHECK(epd_bringup_run_once()==EPD_GDEY029T94_OK);
    CHECK(epd_bringup_get_completed_frames()==2U);
    CHECK(epd_bringup_get_last_status()==EPD_GDEY029T94_OK);
    CHECK(s_refreshes==2U && s_committed==2U && s_sleeps==2U);
    CHECK(s_init_calls==2U && s_bind_calls==2U && s_prepare_calls==2U);
    CHECK(s_renders==2U && s_retry_delays==0U && s_poll_delays==0U);
    CHECK(s_render_models[1].memory_mode==CLOCK_MEMORY_IFLASH);
    CHECK(s_frame_ticks[0]==0U && s_frame_ticks[1]==200U);
    printf("PASS: BW run_once commits before sleep failure and recovers (%lu checks)\n",
        s_checks);
}
#endif

int main(int argc,char **argv)
{
    int stopped;
    if(argc!=2) { fprintf(stderr,"A scenario argument is required\n");return 2; }
    s_scenario=argv[1];
    if(scenario("cycle-wrap")) { s_tick=UINT32_MAX-89999U; }
    init_model();
    CHECK(display_start());CHECK(display_start());
    CHECK(s_thread_function!=NULL);
#if !EPD_CLOCK_USE_GRAY4
    if(scenario("sleep-failure")) { verify_run_once_sleep_failure();return 0; }
#endif
    if(scenario("weather-sync-contract")) { verify_weather_sync_contract();return 0; }
    if(cycle_contract_scenario()) { verify_cycle_contract();return 0; }
    stopped=setjmp(s_stop);
    if(stopped==0) {
        s_thread_function(s_thread_argument);
        CHECK(0);
    }
    verify(stopped);
    printf("PASS: production display gray4=%u scenario %s (%lu checks), %lu renders, %lu commits\n",
        (unsigned int)EPD_CLOCK_USE_GRAY4,s_scenario,s_checks,
        (unsigned long)s_renders,(unsigned long)s_committed);
    return 0;
}
