#include "../device/epd_gdey029t94.h"
#include "../app/epd_vendor_demo.h"
#include "../gui/GUI_Paint.h"
#include "../resources/epd_vendor_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
typedef struct {uint8_t command; uint32_t length; uint8_t data[4736];} record_t;
typedef struct {
    uint8_t dc;
    uint8_t cs;
    uint8_t busy;
    uint8_t busy_error;
    uint32_t tick;
    uint32_t writes;
    uint32_t fail_write;
    uint32_t resets;
    uint32_t count;
    record_t records[4096];
} fake_t;
static fake_t fake;
static epd_gdey029t94_t device;
static uint8_t frame[4736];
static uint8_t transformed[9472];
static uint8_t restored[9472];
static int32_t fake_write(void *handle,const uint8_t *data,uint32_t n,uint32_t timeout)
{
    fake_t *f=handle;
    record_t *r;
    CHECK(f->cs==0U); CHECK(timeout==1000U);
    f->writes++;
    if (f->writes==f->fail_write) return -1;
    if (f->dc==0U) {
        CHECK(n==1U); CHECK(f->count<4096U);
        r=&f->records[f->count++]; r->command=data[0]; r->length=0U;
    } else {
        CHECK(f->count>0U);
        r=&f->records[f->count-1U]; CHECK(r->length+n<=sizeof(r->data));
        memcpy(r->data+r->length,data,n); r->length+=n;
    }
    return 0;
}
static int32_t fake_busy(void *h,uint8_t *level) {fake_t *f=h;*level=f->busy;return f->busy_error ? -1:0;}
static void fake_cs(void *h,uint8_t v) {((fake_t*)h)->cs=v;}
static void fake_dc(void *h,uint8_t v) {((fake_t*)h)->dc=v;}
static void fake_reset(void *h,uint8_t v) {if(v==0U)((fake_t*)h)->resets++;}
static void fake_delay(void *h,uint32_t n) {((fake_t*)h)->tick+=n;}
static uint32_t fake_tick(void *h) {return ((fake_t*)h)->tick;}
static epd_gdey029t94_io_t io={fake_write,fake_busy,fake_cs,fake_dc,fake_reset,fake_delay,fake_tick,&fake};
static void clear_trace(void) {fake.count=0U;fake.writes=0U;fake.fail_write=0U;fake.resets=0U;}
static void initialize(epd_gdey029t94_profile_t p)
{
    memset(&fake,0,sizeof(fake)); memset(&device,0,sizeof(device));
    CHECK(epd_gdey029t94_init_profile(&device,&io,p)==0);
    CHECK(fake.resets==1U); CHECK(fake.tick==20U);
    CHECK(device.profile==p); CHECK(device.background_valid==0U);
}
static unsigned count_cmd(uint8_t command)
{
    unsigned n=0; uint32_t i;
    for(i=0;i<fake.count;i++) if(fake.records[i].command==command)n++;
    return n;
}
static void expect(unsigned index,uint8_t cmd,const uint8_t *data,uint32_t n)
{
    if(index>=fake.count || fake.records[index].command!=cmd) {
        fprintf(stderr,"Trace index=%u expected=%02X actual=%02X count=%lu\n",index,cmd,
            index<fake.count?fake.records[index].command:0,(unsigned long)fake.count);
    }
    CHECK(index<fake.count); CHECK(fake.records[index].command==cmd);
    CHECK(fake.records[index].length==n);
    if(n!=0U) CHECK(memcmp(fake.records[index].data,data,n)==0);
}
static void profiles(void)
{
    static const uint8_t cmds[]={0x12,0x01,0x11,0x44,0x45,0x3C,0x21,0x18,0x4E,0x4F};
    static const uint8_t lens[]={0,3,1,2,4,1,2,1,1,2};
    static const uint8_t bw[10][4]={{0},{0x27,1,0},{1},{0,15},{0x27,1,0,0},{5},{0,0x80},{0x80},{0},{0x27,1}};
    static const uint8_t rotcmd[]={0x12,0x3C,0x01,0x11,0x44,0x45,0x21,0x18,0x4E,0x4F};
    static const uint8_t rotlens[]={0,1,3,1,2,4,2,1,1,2};
    static const uint8_t rot[10][4]={{0},{5},{0x27,1,0},{2},{15,0},{0,0,0x27,1},{0,0},{0x80},{15},{0,0}};
    static const uint8_t guicmd[]={0x12,0x01,0x11,0x44,0x45,0x3C,0x18,0x21,0x4E,0x4F};
    static const uint8_t guilens[]={0,3,1,2,4,1,1,2,1,2};
    static const uint8_t gui[10][4]={{0},{0x55,1,1},{1},{0,15},{0x55,1,0,0},{5},{0x80},{0,0x80},{0},{0x55,1}};
    static const uint8_t graycmd[]={0x12,0x18,0x22,0x20,0x1A,0x22,0x20};
    static const uint8_t graylens[]={0,1,1,0,2,1,0};
    static const uint8_t gray[7][4]={{0},{0x80},{0xB1},{0},{0x5A,0},{0x91},{0}};
    unsigned i;
    initialize(EPD_PROFILE_BW_296); CHECK(fake.count==10U);
    for(i=0;i<10;i++)expect(i,cmds[i],bw[i],lens[i]);
    initialize(EPD_PROFILE_BW_180); CHECK(fake.count==10U);
    for(i=0;i<10;i++)expect(i,rotcmd[i],rot[i],rotlens[i]);
    initialize(EPD_PROFILE_GUI_342); CHECK(fake.count==10U);
    for(i=0;i<10;i++)expect(i,guicmd[i],gui[i],guilens[i]);
    initialize(EPD_PROFILE_GRAY4); CHECK(fake.count==7U);
    for(i=0;i<7;i++)expect(i,graycmd[i],gray[i],graylens[i]);
    for(i=1;i<4;i++) {
        initialize((epd_gdey029t94_profile_t)i); clear_trace();
        CHECK(epd_gdey029t94_fast_refresh(&device,frame,sizeof(frame))==-4);
        CHECK(epd_gdey029t94_full_refresh(&device,frame,sizeof(frame))==-4);
        CHECK(epd_gdey029t94_set_background(&device,frame,sizeof(frame))==-4);
        CHECK(fake.writes==0U);
        if(i!=3U) {
            CHECK(epd_gdey029t94_profile_refresh(&device,frame,sizeof(frame))==0);
            CHECK(count_cmd(0x24)==1U); CHECK(count_cmd(0x26)==0U);
            CHECK(count_cmd(0x20)==1U);
            for(unsigned k=1;k<10;k++) {
                if(i==1U)expect(k-1U,rotcmd[k],rot[k],rotlens[k]);
                else expect(k-1U,guicmd[k],gui[k],guilens[k]);
            }
            expect(9U,0x24,frame,4736U);
        }
    }
}
static uint8_t oracle(uint16_t pair,uint8_t plane)
{
    uint8_t result=0;
    unsigned pixel;
    for(pixel=0;pixel<8;pixel++) {
        unsigned shade=(pair>>(14-2*pixel))&3;
        result|=(uint8_t)(((shade>>plane)&1)<<(7-pixel));
    }
    return (uint8_t)~result;
}
static void gray_test(void)
{
    uint32_t pair; unsigned p;
    for(pair=0;pair<65536U;pair++)for(p=0;p<2;p++) {
        CHECK(epd_gdey029t94_gray4_encode_pair((uint8_t)(pair>>8),(uint8_t)pair,(uint8_t)p)==oracle((uint16_t)pair,(uint8_t)p));
    }
    initialize(EPD_PROFILE_GRAY4); clear_trace();
    CHECK(epd_gdey029t94_gray4_refresh(&device,gImage_4G1,9471U)==-5);
    CHECK(fake.writes==0U);
    CHECK(epd_gdey029t94_gray4_refresh(&device,gImage_4G1,9472U)==0);
    CHECK(fake.count==4U);
    CHECK(fake.records[0].command==0x24U); CHECK(fake.records[1].command==0x26U);
    for(p=0;p<2;p++) {
        CHECK(fake.records[p].length==4736U);
        for(pair=0;pair<4736;pair++)CHECK(fake.records[p].data[pair]==oracle(
            (uint16_t)(((uint16_t)gImage_4G1[pair*2]<<8)|gImage_4G1[pair*2+1]),(uint8_t)p));
    }
    CHECK(fake.records[2].data[0]==0xC7U);
    CHECK(epd_gdey029t94_init_profile(&device,&io,EPD_PROFILE_BW_296)==0);
    CHECK(device.full_refreshes==1U); CHECK(device.background_valid==0U);
    CHECK(epd_gdey029t94_gray4_refresh(&device,gImage_4G1,9472U)==-4);
}
static void partial_test(void)
{
    epd_gdey029t94_partial_item_t items[5];
    unsigned i; uint32_t activations;
    initialize(EPD_PROFILE_BW_296);
    CHECK(epd_gdey029t94_set_background(&device,gImage_basemap,4736)==0);
    for(i=0;i<5;i++) {
        items[i].window=(epd_gdey029t94_rect_t){32U,(uint16_t)(100U+32U*i),64U,32U};
        items[i].image=Num[i]; items[i].image_length=256U;
    }
    clear_trace(); CHECK(epd_gdey029t94_partial_refresh_batch(&device,items,5U)==0);
    CHECK(count_cmd(0x20)==1U); CHECK(count_cmd(0x24)==5U); CHECK(fake.resets==1U);
    CHECK(device.partial_refreshes==1U); CHECK(device.refreshes_since_full==1U);
    for(i=0;i<5;i++) {
        expect(i*7U+6U,0x24U,Num[i],256U);
        CHECK(fake.records[i*7U+2U].data[0]==4U);
        CHECK(fake.records[i*7U+2U].data[1]==11U);
        CHECK(fake.records[i*7U+3U].data[0]==(uint8_t)(295U-items[i].window.y));
        CHECK(memcmp(device.background_image+(100U+32U*i)*16U+4U,Num[i],8U)==0);
    }
    clear_trace();items[4].image_length=255;
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,items,5U)==-5);CHECK(fake.writes==0);
    items[4].image_length=256;items[4].window=items[0].window;
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,items,5U)==-6);CHECK(fake.writes==0);
    items[4].window.y=228;items[4].image=device.background_image;
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,items,5U)==-1);CHECK(fake.writes==0);
    items[4].image=Num[4];items[4].window.x=33;
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,items,5U)==-7);CHECK(fake.writes==0);
    items[4].window.x=32;items[4].window.y=280;
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,items,5U)==-6);CHECK(fake.writes==0);
    clear_trace();
    items[0].window=(epd_gdey029t94_rect_t){0,0,128,296};items[0].image=gImage_p1;items[0].image_length=4736;
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,items,1)==0);
    CHECK(count_cmd(0x20)==1);CHECK(memcmp(device.background_image,gImage_p1,4736)==0);
    clear_trace(); device.refreshes_since_full=119U;
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,items,1)==0);
    activations=count_cmd(0x20);CHECK(activations==2U); CHECK(device.refreshes_since_full==0U);
}
static void fault_test(void)
{
    uint32_t total; uint32_t failure;
    epd_gdey029t94_profile_t p;
    for(p=EPD_PROFILE_BW_296;p<=EPD_PROFILE_GRAY4;p++) {
        initialize(p);total=fake.writes;
        for(failure=1;failure<=total;failure++) {
            memset(&device,0,sizeof(device));clear_trace();fake.fail_write=failure;
            CHECK(epd_gdey029t94_init_profile(&device,&io,p)==-2);
            CHECK(fake.writes==failure); CHECK(device.initialized==0U); CHECK(fake.cs==1U);
        }
        initialize(p);clear_trace();
        if(p==EPD_PROFILE_GRAY4) CHECK(epd_gdey029t94_gray4_refresh(&device,gImage_4G1,9472)==0);
        else CHECK(epd_gdey029t94_profile_refresh(&device,gImage_1,4736)==0);
        total=fake.writes;
        for(failure=1;failure<=total;failure++) {
            initialize(p);clear_trace();fake.fail_write=failure;
            if(p==EPD_PROFILE_GRAY4)CHECK(epd_gdey029t94_gray4_refresh(&device,gImage_4G1,9472)==-2);
            else CHECK(epd_gdey029t94_profile_refresh(&device,gImage_1,4736)==-2);
            CHECK(fake.writes==failure);CHECK(device.initialized==0U);CHECK(device.background_valid==0U);
        }
    }
    initialize(EPD_PROFILE_BW_296);fake.busy=1;clear_trace();
    CHECK(epd_gdey029t94_profile_refresh(&device,frame,4736)==-3);
    CHECK(device.initialized==0);CHECK(fake.tick>=10020U);
    initialize(EPD_PROFILE_BW_296);fake.busy_error=1;clear_trace();
    CHECK(epd_gdey029t94_profile_refresh(&device,frame,4736)==-2);CHECK(device.initialized==0);
    for(unsigned operation=0;operation<2;operation++) {
        const epd_gdey029t94_rect_t window={32,12,56,68};
        initialize(EPD_PROFILE_BW_296);
        CHECK(epd_gdey029t94_set_background(&device,gImage_1,4736)==0);clear_trace();
        if(operation==0) CHECK(epd_gdey029t94_fast_refresh(&device,frame,4736)==0);
        else CHECK(epd_gdey029t94_partial_refresh(&device,&window,frame,476)==0);
        total=fake.writes;
        for(failure=1;failure<=total;failure++) {
            initialize(EPD_PROFILE_BW_296);
            CHECK(epd_gdey029t94_set_background(&device,gImage_1,4736)==0);clear_trace();
            fake.fail_write=failure;
            if(operation==0) CHECK(epd_gdey029t94_fast_refresh(&device,frame,4736)==-2);
            else CHECK(epd_gdey029t94_partial_refresh(&device,&window,frame,476)==-2);
            CHECK(fake.writes==failure);CHECK(device.initialized==0);CHECK(device.background_valid==0);
            CHECK(epd_gdey029t94_partial_refresh(&device,&window,frame,476)==-4);
            CHECK(fake.writes==failure);
        }
    }
}
static uint8_t pixel(const uint8_t *data,unsigned width,unsigned x,unsigned y,unsigned bpp)
{
    unsigned per=8U/bpp;
    return (uint8_t)((data[y*(width/per)+x/per]>>(8U-bpp-(x%per)*bpp))&((1U<<bpp)-1U));
}
static void transforms(void)
{
    const uint8_t *images[]={gImage_1,gImage_2,gImage_basemap,gImage_p1,gImage_p2,gImage_p3,gImage_p4,gImage_p5,gImage_4G1};
    unsigned i,x,y,bpp,n;
    for(i=0;i<9;i++) {
        bpp=(i==8)?2:1;n=4736*bpp;
        epd_vendor_image_rotate180(transformed,images[i],128,296,(uint8_t)bpp);
        for(y=0;y<296;y++)for(x=0;x<128;x++)CHECK(pixel(transformed,128,x,y,bpp)==pixel(images[i],128,127-x,295-y,bpp));
        epd_vendor_image_rotate180(restored,transformed,128,296,(uint8_t)bpp);
        CHECK(memcmp(restored,images[i],n)==0);
    }
    for(i=0;i<10;i++) {
        epd_vendor_image_rotate180(transformed,Num[i],64,32,1);
        for(y=0;y<32;y++)for(x=0;x<64;x++)CHECK(pixel(transformed,64,x,y,1)==pixel(Num[i],64,63-x,31-y,1));
    }
}
static void cn_test(void)
{
    uint8_t guarded[4738];const cFONT *fonts[]={&Font12CN,&Font24CN};
    unsigned f,i,x,y;unsigned advance,stride;
    const uint8_t unknown[]={0xA1,0xA1,'?',0xC4};
    for(f=0;f<2;f++)for(i=0;i<6;i++) {
        memset(guarded,0xFF,sizeof(guarded));guarded[0]=0xA5;guarded[4737]=0x5A;
        Paint_NewImage(guarded+1,128,296,ROTATE_0,WHITE);
        Paint_DrawString_CN(7,3,fonts[f]->table[i].index,
            fonts[f]->table[i].index[1]?2:1,fonts[f],WHITE,BLACK);
        advance=fonts[f]->table[i].index[1]?fonts[f]->Width:fonts[f]->ASCII_Width;
        stride=fonts[f]->Width/8;
        for(y=0;y<fonts[f]->Height;y++)for(x=0;x<advance;x++) {
            unsigned ink=(fonts[f]->table[i].matrix[y*stride+x/8]>>(7-x%8))&1;
            CHECK(pixel(guarded+1,128,x+7,y+3,1)==!ink);
        }
        CHECK(guarded[0]==0xA5);CHECK(guarded[4737]==0x5A);
        Paint_DrawString_CN(127,295,unknown,sizeof(unknown),fonts[f],WHITE,BLACK);
        CHECK(guarded[0]==0xA5);CHECK(guarded[4737]==0x5A);
    }
    Paint_Clear(WHITE); Paint_DrawString_CN(0,0,unknown,sizeof(unknown),&Font12CN,WHITE,BLACK);
    CHECK(pixel(guarded+1,128,0,0,1)==0);CHECK(pixel(guarded+1,128,15,20,1)==0);
    CHECK(pixel(guarded+1,128,1,1,1)==1);
    Paint_DrawString_CN(0,0,NULL,5,&Font12CN,WHITE,BLACK);
    Paint_DrawString_CN(0,0,unknown,4,NULL,WHITE,BLACK);
    for(i=0;i<6;i++) {
        epd_vendor_gui_page(guarded+1,(uint8_t)i);
        CHECK(Paint.Rotate==ROTATE_270);CHECK(Paint.Mirror==MIRROR_NONE);
        CHECK(guarded[0]==0xA5);CHECK(guarded[4737]==0x5A);
    }
}
static void demo_test(void)
{
    initialize(EPD_PROFILE_BW_296);clear_trace();
    CHECK(epd_vendor_demo_run(&device,&io,frame)==0);
    CHECK(epd_vendor_demo_state.completed==1U);CHECK(epd_vendor_demo_state.stage==14U);
    CHECK(epd_vendor_demo_state.completed_pages==12U);
    CHECK(epd_vendor_demo_state.completed_batches==11U);
    CHECK(epd_vendor_demo_state.cleanup_refreshes==2U);
    CHECK(device.sleeping==1U);CHECK(device.partial_refreshes==11U);
    CHECK(device.full_refreshes==15U);CHECK(device.fast_refreshes==1U);
    CHECK(count_cmd(0x10)==14U);CHECK(count_cmd(0x20)==31U);
    CHECK(epd_gdey029t94_init_profile(&device,&io,EPD_PROFILE_BW_296)==0);
    CHECK(device.refreshes_since_full==0U);CHECK(device.partial_refreshes==11U);
    clear_trace();fake.fail_write=4;
    CHECK(epd_vendor_demo_run(&device,&io,frame)==-2);
    CHECK(epd_vendor_demo_state.completed==0U);CHECK(epd_vendor_demo_state.stage==1U);
    CHECK(fake.writes==4U);
}

static void contracts(void)
{
    epd_gdey029t94_partial_item_t item={{0,0,128,296},frame,4736};
    initialize(EPD_PROFILE_BW_296);clear_trace();
    CHECK(epd_gdey029t94_profile_refresh(&device,frame,4735)==-5);
    CHECK(epd_gdey029t94_profile_refresh(&device,NULL,4736)==-1);
    CHECK(epd_gdey029t94_gray4_refresh(&device,NULL,9472)==-1);
    CHECK(epd_gdey029t94_init_profile(&device,&io,(epd_gdey029t94_profile_t)-1)==-1);
    CHECK(epd_gdey029t94_init_profile(&device,&io,(epd_gdey029t94_profile_t)4)==-1);
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,&item,0)==-1);
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,&item,6)==-1);
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,&item,1)==-4);
    CHECK(fake.writes==0);
    CHECK(epd_gdey029t94_set_background(&device,gImage_1,4736)==0);clear_trace();
    item.window.x=120;item.window.width=8;item.window.y=295;item.window.height=1;
    item.image_length=1; CHECK(epd_gdey029t94_partial_refresh_batch(&device,&item,1)==0);
    CHECK(count_cmd(0x20)==1);CHECK(fake.records[3].data[0]==0);
    CHECK(device.background_image[4735]==frame[0]);
    clear_trace();CHECK(epd_gdey029t94_sleep(&device)==0);
    CHECK(count_cmd(0x10)==1);CHECK(fake.records[0].data[0]==1);
    clear_trace();CHECK(epd_gdey029t94_profile_refresh(&device,frame,4736)==-4);
    CHECK(epd_gdey029t94_partial_refresh_batch(&device,&item,1)==-4);CHECK(fake.writes==0);
    initialize(EPD_PROFILE_BW_296);CHECK(epd_gdey029t94_set_background(&device,gImage_1,4736)==0);
    clear_trace();fake.fail_write=2;
    CHECK(epd_gdey029t94_sleep(&device)==-2);CHECK(device.initialized==0);CHECK(fake.writes==2);
}
static void export_buffers(const char *directory)
{
    char path[512];
    FILE *file;
    unsigned page;
    for(page=0;page<6;page++) {
        epd_vendor_gui_page(frame,(uint8_t)page);
        CHECK(snprintf(path,sizeof(path),"%s/gui-%u.bin",directory,page)>0);
        file=fopen(path,"wb");CHECK(file!=NULL);
        CHECK(fwrite(frame,1,sizeof(frame),file)==sizeof(frame));CHECK(fclose(file)==0);
    }
    epd_vendor_image_rotate180(frame,gImage_1,128,296,1);
    CHECK(snprintf(path,sizeof(path),"%s/image-1.bin",directory)>0);
    file=fopen(path,"wb");CHECK(file!=NULL);
    CHECK(fwrite(frame,1,sizeof(frame),file)==sizeof(frame));CHECK(fclose(file)==0);
    epd_vendor_image_rotate180(transformed,gImage_4G1,128,296,2);
    CHECK(snprintf(path,sizeof(path),"%s/gray4.bin",directory)>0);
    file=fopen(path,"wb");CHECK(file!=NULL);
    CHECK(fwrite(transformed,1,sizeof(transformed),file)==sizeof(transformed));CHECK(fclose(file)==0);
}
int main(int argc,char **argv)
{
    profiles();gray_test();partial_test();fault_test();transforms();cn_test();demo_test();contracts();
    if(argc>1)export_buffers(argv[1]);
    printf("PASS: %lu checks: profiles, planes, faults, transforms, CN, batch and complete demo.\n",checks);
    return 0;
}
