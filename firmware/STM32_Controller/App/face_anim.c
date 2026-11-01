#include "face_anim.h"
#include "ff.h"
#include "lcd.h"
#include "ailink.h"
#include "../System/time.h"
#include <string.h>
#include <stdio.h>

/* Metadata only is compiled into Flash. Every pixel stays on the SD card. */
typedef struct { const char *directory; uint16_t frames; } Clip;
static const Clip clips[]={
    {"BLINK",45u},{"SHY",60u},{"HAPPY",42u},{"GLASSES",57u},
    {"ANGRY",42u},{"FUNNY",51u},{"WINK",48u},{"POUT",48u},
    {"LAUGH",66u},{"STARRY",39u},{"RIGHT",54u},{"SLEEP",48u},
    {"DOUBT",45u},{"LOOK",54u},{"SMILE",36u},{"SURPRISE",56u},{"THINK",40u}
};
static FIL stream;
static uint8_t raw_row[160u*3u];
static uint16_t pixel_row[160u];
/* Eight top-down RGB565 rows: five aligned SD sectors, no frame buffer. */
static uint16_t fast_pixels[160u*8u];
static uint8_t fast_format;
static const Clip *clip;
static uint8_t opened, active, error, top_down, enabled=1u;
static uint16_t frame, row, origin_x, origin_y, interval_ms=40u;
static uint32_t offset, due, completed, frame_started;
static uint32_t sample_at, sample_frames;
static uint16_t measured_fps_x10;

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint8_t equal(const char *a,const char *b)
{
    char c,d;
    do {
        c=*a++;d=*b++;
        if(c>='a' && c<='z') c=(char)(c-'a'+'A');
        if(d>='a' && d<='z') d=(char)(d-'a'+'A');
        if(c!=d)return 0u;
    } while(c);
    return 1u;
}
static const Clip *find_clip(const char *name)
{
    uint8_t i;
    if(!name || !*name || equal(name,"normal")) name="BLINK";
    else if(equal(name,"sleepy")) name="SLEEP";
    else if(equal(name,"doubtful")) name="DOUBT";
    else if(equal(name,"eyseclose") || equal(name,"eyesclose")) name="BLINK";
    else if(equal(name,"lookleft") || equal(name,"lookright")) name="LOOK";
    else if(equal(name,"look_right")) name="RIGHT";
    else if(equal(name,"lookup1") || equal(name,"lookup2")) name="STARRY";
    else if(equal(name,"smell")) name="POUT";
    for(i=0u;i<sizeof(clips)/sizeof(clips[0]);i++)
        if(equal(name,clips[i].directory))return &clips[i];
    return 0;
}

void FaceAnim_Stop(void)
{
    if(opened) { (void)f_close(&stream); opened=0u; }
    active=0u;
}
static void failed(uint8_t reason)
{
    error=reason;FaceAnim_Stop();
    printf("[ANIM] %s frame=%u error=%u; static fallback\r\n",
           clip?clip->directory:"NONE",(unsigned)frame,(unsigned)reason);
}
static uint8_t open_frame(void)
{
    uint8_t header[54];
    UINT got;
    uint32_t dib,height;
    char path[32];
    fast_format=0u;
    sprintf(path,"FACE/%s/%04u.R16",clip->directory,(unsigned)frame);
    if(f_open(&stream,path,FA_READ)==FR_OK) {
        opened=1u;
        if(f_read(&stream,header,16u,&got)==FR_OK && got==16u &&
           !memcmp(header,"D565",4u) && read32(header+4)==160u &&
           read32(header+8)==160u && read32(header+12)==51200u &&
           f_size(&stream)==51712u && f_lseek(&stream,512u)==FR_OK) {
            fast_format=1u;row=0u;return 1u;
        }
        /* An absent or stale sidecar never prevents the original BMP playing. */
        (void)f_close(&stream);opened=0u;
    }
    sprintf(path,"FACE/%s/%04u.BMP",clip->directory,(unsigned)frame);
    if(f_open(&stream,path,FA_READ)!=FR_OK) { failed(1u);return 0u; }
    opened=1u;
    if(f_read(&stream,header,sizeof(header),&got)!=FR_OK || got!=sizeof(header) ||
       header[0]!='B' || header[1]!='M') { failed(2u);return 0u; }
    dib=read32(header+14);offset=read32(header+10);height=read32(header+22);
    top_down=(uint8_t)(height==0xFFFFFF60u); /* signed -160 */
    if(dib<40u || dib>124u || offset<14u+dib ||
       read32(header+18)!=160u || (height!=160u && !top_down) ||
       header[26]!=1u || header[27]!=0u || header[28]!=24u || header[29]!=0u ||
       read32(header+30)!=0u || offset>f_size(&stream) ||
       f_size(&stream)-offset<160u*480u) { failed(2u);return 0u; }
    row=0u;return 1u;
}

uint8_t FaceAnim_Start(const char *name,uint16_t x,uint16_t y,uint32_t now)
{
    FaceAnim_Stop();clip=find_clip(name);error=0u;completed=0u;frame=0u;row=0u;
    if(!enabled || !clip || x+160u>LCD_W || y+160u>LCD_H) return 0u;
    origin_x=x;origin_y=y;due=frame_started=now;active=1u;
    sample_at=now;sample_frames=0u;measured_fps_x10=0u;
    if(!open_frame())return 0u;
    printf("[ANIM] %s frames=%u interval=%ums format=%s SD-only\r\n",clip->directory,
           (unsigned)clip->frames,(unsigned)interval_ms,fast_format?"R16":"BMP");
    return 1u;
}

void FaceAnim_Tick(uint32_t now,uint8_t allow)
{
    uint8_t slice=0u;
    uint16_t col;
    UINT got;
    uint32_t started;
    if(!allow || !active)return;
    if(!opened) {
        if((int32_t)(now-due)<0)return;
        frame_started=now;
        if(!open_frame())return;
    }
    started=GetTick_ms();
    if(fast_format) {
        /* One sequential block per loop, then give motion/UART their turn. */
        if(f_read(&stream,fast_pixels,sizeof(fast_pixels),&got)!=FR_OK ||
           got!=sizeof(fast_pixels)) { failed(3u);return; }
        LCD_ShowBuf(origin_x,(uint16_t)(origin_y+row),160u,8u,fast_pixels);
        row+=8u;AILink_ServiceMotion();
    }
    /* BMP compatibility: at most four rows or two ms per main-loop slice. A single
     * card read can take longer; never claim this is a hardware time bound. */
    while(!fast_format && row<160u && slice<4u) {
        if(f_lseek(&stream,offset+(uint32_t)row*480u)!=FR_OK ||
           f_read(&stream,raw_row,sizeof(raw_row),&got)!=FR_OK || got!=sizeof(raw_row)) {
            failed(3u);return;
        }
        for(col=0u;col<160u;col++)pixel_row[col]=LCD_RGB(raw_row[col*3u+2u],raw_row[col*3u+1u],raw_row[col*3u]);
        LCD_ShowBuf(origin_x,(uint16_t)(origin_y+(top_down?row:159u-row)),160u,1u,pixel_row);
        row++;slice++;AILink_ServiceMotion();
        if(GetTick_ms()-started>=2u)break;
    }
    if(row==160u) {
        (void)f_close(&stream);opened=0u;completed++;
        frame=(uint16_t)((frame+1u)%clip->frames);
        now=GetTick_ms();due=frame_started+interval_ms;
        if(completed==1u)sample_at=now;
        else sample_frames++;
        if(now-sample_at>=1000u) {
            measured_fps_x10=(uint16_t)(sample_frames*10000u/(now-sample_at));
            sample_at=now;sample_frames=0u;
        }
        /* Never skip frames or emit an unbounded catch-up burst after a stall. */
        if((int32_t)(now-due)>=0)due=now+1u;
    }
}
void FaceAnim_GetInfo(FaceAnim_Info *info)
{
    if(!info)return;
    info->directory=clip?clip->directory:"NONE";info->frames=clip?clip->frames:0u;
    info->frame=frame;info->rows=row;info->interval_ms=interval_ms;
    info->completed=completed;info->active=active;info->error=error;
    info->measured_fps_x10=measured_fps_x10;info->rgb565=fast_format;
}
uint8_t FaceAnim_SetFPS(uint8_t fps)
{
    if(fps<1u || fps>25u)return 0u;
    interval_ms=(uint16_t)((1000u+fps-1u)/fps);return 1u;
}
uint8_t FaceAnim_SetEnabled(uint8_t value)
{
    if(value>1u)return 0u;
    enabled=value;if(!enabled)FaceAnim_Stop();return 1u;
}
uint8_t FaceAnim_IsEnabled(void) {return enabled;}
