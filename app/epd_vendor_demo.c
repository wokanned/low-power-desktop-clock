#include "epd_vendor_demo.h"
#if EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_GRAY4_HOLD
#include "epd_clock_view.h"
#endif
#include "../gui/GUI_Paint.h"
#include "../resources/epd_vendor_assets.h"
#include <stddef.h>
#include <string.h>

volatile epd_vendor_demo_state_t epd_vendor_demo_state;
#if (EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_FULL) || \
    (EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_GRAY4)
static uint8_t gray_work[EPD_VENDOR_GRAY_SIZE];
#endif
#if (EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF) && \
    (EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_GRAY4_HOLD)
static uint8_t patches[5][EPD_VENDOR_PATCH_SIZE];
#endif

void epd_vendor_image_rotate180(uint8_t *destination, const uint8_t *source,
                                uint16_t width, uint16_t height, uint8_t bpp)
{
    uint32_t length;
    uint32_t index;
    uint8_t value;
    if ((destination == NULL) || (source == NULL) || (destination == source) ||
        (width == 0U) || (height == 0U) || ((width % 8U) != 0U) ||
        ((bpp != 1U) && (bpp != 2U))) return;
    length = (uint32_t)(width / 8U) * height * bpp;
    for (index = 0U; index < length; index++) {
        value = source[length - 1U - index];
        value = (uint8_t)(((value & 0x33U) << 2U) | ((value & 0xCCU) >> 2U));
        value = (uint8_t)((value << 4U) | (value >> 4U));
        if (bpp == 1U) {
            value = (uint8_t)(((value & 0x55U) << 1U) | ((value & 0xAAU) >> 1U));
        }
        destination[index] = value;
    }
}

void epd_vendor_gui_page(uint8_t *frame, uint8_t page)
{
    static const uint8_t chinese[] = {0xC4U, 0xE3U, 0xBAU, 0xC3U, 'A', 'a', 'b', 'c'};
    const sFONT *fonts[5] = {&Font8, &Font12, &Font16, &Font20, &Font24};
    const uint16_t positions[5] = {0U, 10U, 25U, 45U, 80U};
    uint8_t i;
    Paint_NewImage(frame, 128U, 296U, ROTATE_270, WHITE);
    Paint_SetMirroring(MIRROR_NONE);
    Paint_Clear(WHITE);
    if (page == 0U) {
        for (i = 1U; i <= 4U; i++) {
            Paint_DrawPoint(5U, (UWORD)(10U + (i - 1U) * 15U), BLACK,
                             (DOT_PIXEL)i, DOT_STYLE_DFT);
        }
        Paint_DrawLine(20U,10U,70U,60U,BLACK,LINE_STYLE_SOLID,DOT_PIXEL_1X1);
        Paint_DrawLine(70U,10U,20U,60U,BLACK,LINE_STYLE_SOLID,DOT_PIXEL_1X1);
        Paint_DrawRectangle(20U,10U,70U,60U,BLACK,DRAW_FILL_EMPTY,DOT_PIXEL_1X1);
        Paint_DrawRectangle(85U,10U,130U,60U,BLACK,DRAW_FILL_FULL,DOT_PIXEL_1X1);
        Paint_DrawCircle(150U,90U,30U,BLACK,DRAW_FILL_EMPTY,DOT_PIXEL_1X1);
        Paint_DrawCircle(220U,90U,30U,BLACK,DRAW_FILL_FULL,DOT_PIXEL_1X1);
    } else if ((page == 1U) || (page == 2U)) {
        for (i = 0U; i < 5U; i++) {
            if (page == 1U) {
                Paint_DrawString_EN(0U, positions[i], "Good Display", fonts[i], WHITE, BLACK);
            } else {
                Paint_DrawNum(0U, positions[i], 123456789, fonts[i], WHITE, BLACK);
            }
        }
    } else if ((page == 3U) || (page == 4U)) {
        Paint_DrawString_CN(12U, 35U, chinese, sizeof(chinese),
                             (page == 3U) ? &Font12CN : &Font24CN, WHITE, BLACK);
    } else {
        Paint_DrawString_EN(0U,0U,"TL",&Font12,WHITE,BLACK);
        Paint_DrawString_EN(282U,0U,"TR",&Font12,WHITE,BLACK);
        Paint_DrawString_EN(0U,116U,"BL",&Font12,WHITE,BLACK);
        Paint_DrawString_EN(282U,116U,"BR",&Font12,WHITE,BLACK);
        Paint_DrawString_EN(40U,20U,"0123456789",&Font20,WHITE,BLACK);
        Paint_DrawString_EN(40U,75U,"L   TOP   R",&Font16,WHITE,BLACK);
        Paint_DrawLine(70U,65U,210U,65U,BLACK,LINE_STYLE_SOLID,DOT_PIXEL_1X1);
        Paint_DrawLine(190U,55U,210U,65U,BLACK,LINE_STYLE_SOLID,DOT_PIXEL_1X1);
        Paint_DrawLine(190U,75U,210U,65U,BLACK,LINE_STYLE_SOLID,DOT_PIXEL_1X1);
    }
}

#if (EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF) && \
    (EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_GRAY4_HOLD)
static epd_gdey029t94_status_t demo_pause(epd_gdey029t94_t *device)
{
    epd_gdey029t94_status_t status = epd_gdey029t94_sleep(device);
    if (status == EPD_GDEY029T94_OK) device->io.delay_ms(device->io.handle, 2000U);
    return status;
}

static epd_gdey029t94_status_t demo_image(epd_gdey029t94_t *device,
    const epd_gdey029t94_io_t *io, uint8_t *frame, const uint8_t *image,
    epd_gdey029t94_profile_t profile, uint8_t fast)
{
    epd_gdey029t94_status_t status;
    status = epd_gdey029t94_init_profile(device, io, profile);
    if (status != EPD_GDEY029T94_OK) return status;
    epd_vendor_image_rotate180(frame, image, 128U, 296U, 1U);
    status = fast ? epd_gdey029t94_fast_refresh(device, frame, EPD_VENDOR_BW_SIZE) :
                    epd_gdey029t94_profile_refresh(device, frame, EPD_VENDOR_BW_SIZE);
    if (status != EPD_GDEY029T94_OK) return status;
    epd_vendor_demo_state.completed_pages++;
    return demo_pause(device);
}

static epd_gdey029t94_status_t demo_time(epd_gdey029t94_t *device,
    const epd_gdey029t94_io_t *io, uint8_t *frame)
{
    epd_gdey029t94_partial_item_t items[5];
    const uint8_t *sources[5];
    uint8_t i;
    uint8_t j;
    epd_gdey029t94_status_t status;
    status = epd_gdey029t94_init_profile(device, io, EPD_PROFILE_BW_296);
    if (status != EPD_GDEY029T94_OK) return status;
    epd_vendor_image_rotate180(frame,gImage_basemap,128U,296U,1U);
    status = epd_gdey029t94_set_background(device,frame,EPD_VENDOR_BW_SIZE);
    if (status != EPD_GDEY029T94_OK) return status;
    for (i = 0U; i < 6U; i++) {
        sources[0]=Num[i]; sources[1]=Num[0]; sources[2]=gImage_numdot;
        sources[3]=Num[0]; sources[4]=Num[1];
        for (j = 0U; j < 5U; j++) {
            epd_vendor_image_rotate180(patches[j],sources[j],64U,32U,1U);
            /* Apply the same two-axis transform to vendor window positions. */
            items[j].window.x=32U;
            items[j].window.y=(uint16_t)(296U - (100U + 32U*j) - 32U);
            items[j].window.width=64U; items[j].window.height=32U;
            items[j].image=patches[j]; items[j].image_length=EPD_VENDOR_PATCH_SIZE;
        }
        status = epd_gdey029t94_partial_refresh_batch(device,items,5U);
        if (status != EPD_GDEY029T94_OK) return status;
        epd_vendor_demo_state.completed_batches++;
        device->io.delay_ms(device->io.handle,1000U);
        if (i == 4U) {
            status = epd_gdey029t94_full_refresh(device,device->background_image,EPD_VENDOR_BW_SIZE);
            if (status != EPD_GDEY029T94_OK) return status;
            epd_vendor_demo_state.cleanup_refreshes++;
        }
    }
    return demo_pause(device);
}

static epd_gdey029t94_status_t demo_full_partial(epd_gdey029t94_t *device,
    const epd_gdey029t94_io_t *io, uint8_t *frame)
{
    const uint8_t *images[5]={gImage_p1,gImage_p2,gImage_p3,gImage_p4,gImage_p5};
    const epd_gdey029t94_rect_t window={0U,0U,128U,296U};
    epd_gdey029t94_status_t status;
    uint8_t i;
    status = epd_gdey029t94_init_profile(device,io,EPD_PROFILE_BW_296);
    if (status != EPD_GDEY029T94_OK) return status;
    epd_vendor_image_rotate180(frame,gImage_p1,128U,296U,1U);
    status = epd_gdey029t94_set_background(device,frame,EPD_VENDOR_BW_SIZE);
    if (status != EPD_GDEY029T94_OK) return status;
    for (i=0U;i<5U;i++) {
        epd_vendor_image_rotate180(frame,images[i],128U,296U,1U);
        status = epd_gdey029t94_partial_refresh(device,&window,frame,EPD_VENDOR_BW_SIZE);
        if (status != EPD_GDEY029T94_OK) return status;
        epd_vendor_demo_state.completed_batches++;
        device->io.delay_ms(device->io.handle,1000U);
    }
    status = epd_gdey029t94_full_refresh(device,frame,EPD_VENDOR_BW_SIZE);
    if (status != EPD_GDEY029T94_OK) return status;
    epd_vendor_demo_state.cleanup_refreshes++;
    return demo_pause(device);
}
#endif

epd_gdey029t94_status_t epd_vendor_demo_run(epd_gdey029t94_t *device,
    const epd_gdey029t94_io_t *io, uint8_t *frame)
{
#if EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_GRAY4_HOLD
    epd_gdey029t94_status_t status;
    memset((void *)&epd_vendor_demo_state, 0, sizeof(epd_vendor_demo_state));
    epd_vendor_demo_state.stage = EPD_VENDOR_STAGE_GRAY4_DRAW;
    if ((device == NULL) || (io == NULL) || (frame == NULL)) {
        status = EPD_GDEY029T94_ERROR_ARGUMENT;
        goto failure;
    }
    if (epd_clock_view_draw_gray4_diagnostic(frame,
            EPD_GDEY029T94_GRAY4_SIZE) == 0U) {
        status = EPD_GDEY029T94_ERROR_ARGUMENT;
        goto failure;
    }
    epd_vendor_demo_state.stage = EPD_VENDOR_STAGE_GRAY4_INIT;
    status = epd_gdey029t94_init_profile(device, io, EPD_PROFILE_GRAY4);
    if (status != EPD_GDEY029T94_OK) { goto failure; }
    epd_vendor_demo_state.stage = EPD_VENDOR_STAGE_GRAY4_REFRESH;
    status = epd_gdey029t94_gray4_refresh(device, frame,
        EPD_GDEY029T94_GRAY4_SIZE);
    if (status != EPD_GDEY029T94_OK) { goto failure; }
    epd_vendor_demo_state.completed_pages = 1U;
    epd_vendor_demo_state.stage = EPD_VENDOR_STAGE_GRAY4_SLEEP;
    status = epd_gdey029t94_sleep(device);
    if (status != EPD_GDEY029T94_OK) { goto failure; }
    epd_vendor_demo_state.stage = EPD_VENDOR_STAGE_GRAY4_HELD;
    epd_vendor_demo_state.completed = 1U;
    return EPD_GDEY029T94_OK;
failure:
    epd_vendor_demo_state.status = status;
    return status;
#elif EPD_VENDOR_DEMO_MODE != EPD_VENDOR_DEMO_OFF
    epd_gdey029t94_status_t status;
    uint8_t page;
    uint32_t offset;
    uint32_t block;
    const uint8_t full=(EPD_VENDOR_DEMO_MODE==EPD_VENDOR_DEMO_FULL);
    if ((device==NULL)||(io==NULL)||(frame==NULL)) return EPD_GDEY029T94_ERROR_ARGUMENT;
    memset((void *)&epd_vendor_demo_state,0,sizeof(epd_vendor_demo_state));
    if (full) {
        epd_vendor_demo_state.stage=1U;
        status=demo_image(device,io,frame,gImage_1,EPD_PROFILE_BW_296,0U);
        if(status!=EPD_GDEY029T94_OK) goto failure;
        epd_vendor_demo_state.stage=2U;
        status=demo_image(device,io,frame,gImage_2,EPD_PROFILE_BW_296,1U);
        if(status!=EPD_GDEY029T94_OK) goto failure;
    }
#if (EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_FULL) || (EPD_VENDOR_DEMO_MODE == EPD_VENDOR_DEMO_GRAY4)
    epd_vendor_demo_state.stage=3U;
    status=epd_gdey029t94_init_profile(device,io,EPD_PROFILE_GRAY4);
    if(status!=EPD_GDEY029T94_OK) goto failure;
    epd_vendor_image_rotate180(gray_work,gImage_4G1,128U,296U,2U);
    status=epd_gdey029t94_gray4_refresh(device,gray_work,sizeof(gray_work));
    if(status!=EPD_GDEY029T94_OK) goto failure;
    epd_vendor_demo_state.completed_pages++;
    status=demo_pause(device);
    if(status!=EPD_GDEY029T94_OK) goto failure;
#endif
    if(full || (EPD_VENDOR_DEMO_MODE==EPD_VENDOR_DEMO_PARTIAL_TIME)) {
        epd_vendor_demo_state.stage=4U;
        status=demo_time(device,io,frame);
        if(status!=EPD_GDEY029T94_OK) goto failure;
    }
    if(full || (EPD_VENDOR_DEMO_MODE==EPD_VENDOR_DEMO_PARTIAL_FULL)) {
        epd_vendor_demo_state.stage=5U;
        status=demo_full_partial(device,io,frame);
        if(status!=EPD_GDEY029T94_OK) goto failure;
    }
    if(full || (EPD_VENDOR_DEMO_MODE==EPD_VENDOR_DEMO_ROTATE_180)) {
        epd_vendor_demo_state.stage=6U;
        status=demo_image(device,io,frame,gImage_1,EPD_PROFILE_BW_180,0U);
        if(status!=EPD_GDEY029T94_OK) goto failure;
    }
    if(full || (EPD_VENDOR_DEMO_MODE==EPD_VENDOR_DEMO_GUI_342)) {
        for(page=0U;page<6U;page++) {
            epd_vendor_demo_state.stage=7U+page;
            status=epd_gdey029t94_init_profile(device,io,EPD_PROFILE_GUI_342);
            if(status!=EPD_GDEY029T94_OK) goto failure;
            epd_vendor_gui_page(frame,page);
            status=epd_gdey029t94_profile_refresh(device,frame,EPD_VENDOR_BW_SIZE);
            if(status!=EPD_GDEY029T94_OK) goto failure;
            epd_vendor_demo_state.completed_pages++;
            status=demo_pause(device);
            if(status!=EPD_GDEY029T94_OK) goto failure;
        }
    }
    epd_vendor_demo_state.stage=13U;
    status=epd_gdey029t94_init_profile(device,io,EPD_PROFILE_BW_296);
    if(status!=EPD_GDEY029T94_OK) goto failure;
    if(full) {
        memset(frame,BLACK,EPD_VENDOR_BW_SIZE);
        status=epd_gdey029t94_full_refresh(device,frame,EPD_VENDOR_BW_SIZE);
        if(status!=EPD_GDEY029T94_OK) goto failure;
        epd_vendor_demo_state.completed_pages++;
        status=demo_pause(device);
        if(status!=EPD_GDEY029T94_OK) goto failure;
        status=epd_gdey029t94_init_profile(device,io,EPD_PROFILE_BW_296);
        if(status!=EPD_GDEY029T94_OK) goto failure;
    }
    for (offset=0U;offset<EPD_VENDOR_BW_SIZE;offset+=EPD_VENDOR_PATCH_SIZE) {
        block=EPD_VENDOR_BW_SIZE-offset;
        if(block>EPD_VENDOR_PATCH_SIZE) block=EPD_VENDOR_PATCH_SIZE;
        memcpy(frame+offset,gImage_white,block);
    }
    status=epd_gdey029t94_full_refresh(device,frame,EPD_VENDOR_BW_SIZE);
    if(status!=EPD_GDEY029T94_OK) goto failure;
    epd_vendor_demo_state.completed_pages++;
    status=epd_gdey029t94_sleep(device);
    if(status!=EPD_GDEY029T94_OK) goto failure;
    epd_vendor_demo_state.stage=14U;
    epd_vendor_demo_state.completed=1U;
    return EPD_GDEY029T94_OK;
failure:
    epd_vendor_demo_state.status=status;
    return status;
#else
    (void)device; (void)io; (void)frame;
    return EPD_GDEY029T94_OK;
#endif
}
