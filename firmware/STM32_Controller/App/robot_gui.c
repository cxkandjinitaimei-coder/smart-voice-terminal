/**
 * @file robot_gui.c
 * @brief F407深色圆角界面：状态卡、参数页及录音局部反馈。
 * 使用：主循环进入页面时调用Gui_Frame；周期调用各页更新与Gui_AudioTick。
 * 绘图走现有FSMC LCD，不运行网页，也不创建全屏帧缓冲。
 * 录音前缓存少量状态字模；采集/排空期间仅用RAM字模与ASCII局部绘制，
 * 不读取SD图片、SPI中文字库，不在中断里绘制。传感器与音频状态均读真实值。
 */
#include "camera_link.h"
#include "robot_gui.h"
#include "app_version.h"
#include "miclink.h"
#include "micaud.h"
#include "inmp441.h"
#include "ailink.h"
#include "esp_uart.h"
#include "dht11.h"
#include "font.h"
#include "font_store.h"
#include "font_upload.h"
#include "iap_app.h"
#include "face_anim.h"
#include "../System/time.h"
#include <stdio.h>
#include <string.h>

static GuiHardware hw;
static char row_cache[10][64], audio_caption[256];
static uint8_t bars_shown[12], link_shown=255u, meter_level, audio_code=255u;
static uint32_t audio_due, last_capture_ms, timer_shown=0xFFFFFFFFu;
/* 40个字模最多1360字节，入页时准备；不保留Font_Glyph16的临时指针。 */
static struct { uint16_t cp; uint8_t bits[32]; } ui_glyph[40];
static uint8_t ui_glyph_count, ui_glyph_ready;

void Gui_SetHardware(const GuiHardware *hardware) { hw = *hardware; }

/* 半开尺寸转换为LCD闭区间。只画顶部/底部圆角扫描线与中间矩形，
 * 无浮点、无动态分配；音量变化只调用很窄的小柱，不重画整张卡片。 */
void Gui_RoundRect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint8_t radius,uint16_t color)
{
    uint16_t i, dx, dy, r=radius;
    if(!w || !h || x>=LCD_W || y>=LCD_H)return;
    if(w>LCD_W-x)w=LCD_W-x;
    if(h>LCD_H-y)h=LCD_H-y;
    if(r>w/2u)r=w/2u;
    if(r>h/2u)r=h/2u;
    if(!r){LCD_Fill(x,y,x+w-1u,y+h-1u,color);return;}
    for(i=0u;i<r;i++) {
        dy=(uint16_t)(r-1u-i); dx=r;
        while(dx && (uint32_t)dx*dx+(uint32_t)dy*dy>(uint32_t)r*r)dx--;
        LCD_Fill(x+r-dx,y+i,x+w-r+dx-1u,y+i,color);
        LCD_Fill(x+r-dx,y+h-1u-i,x+w-r+dx-1u,y+h-1u-i,color);
    }
    if(h>2u*r)LCD_Fill(x,y+r,x+w-1u,y+h-r-1u,color);
}

static uint16_t next_cp(const char **p)
{
    const unsigned char *s=(const unsigned char *)*p;
    uint16_t cp;
    if(s[0]<0x80u){*p+=1;return s[0];}
    if(s[0]>=0xE0u && s[0]<=0xEFu && s[1] && s[2]) {
        cp=(uint16_t)(((s[0]&15u)<<12)|((s[1]&63u)<<6)|(s[2]&63u));*p+=3;return cp;
    }
    *p+=1;return '?';
}

static void cache_audio_glyphs(void)
{
    const char *p="等你开口准备录音正在听等待断句发送中上传收尾监听间隔底噪校准手动就绪";
    uint16_t cp; uint8_t i;
    if(ui_glyph_ready || MicAud_IsStreaming())return;
    ui_glyph_count=0u;
    while(*p) {
        cp=next_cp(&p);
        for(i=0u;i<ui_glyph_count;i++)if(ui_glyph[i].cp==cp)break;
        if(i<ui_glyph_count || ui_glyph_count>=40u)continue;
        if(Font_Glyph16Copy(cp,ui_glyph[ui_glyph_count].bits)) {
            ui_glyph[ui_glyph_count].cp=cp;ui_glyph_count++;
        }
    }
    ui_glyph_ready=1u;
}

/* 录音状态中文只从已缓存字模取，不走可能访问W25Q的通用文本函数。 */
static void audio_text(uint16_t x,uint16_t y,uint16_t w,const char *p,uint16_t color)
{
    uint16_t cp, end=(uint16_t)(x+w); uint8_t i;
    while(*p) {
        cp=next_cp(&p);
        if(x+(cp<128u?8u:16u)>end)break;
        if(cp<128u) {LCD_ShowChar(x,y,(uint8_t)cp,color,GUI_PANEL,LCD_FONT_16,0u);x+=8u;}
        else {
            for(i=0u;i<ui_glyph_count;i++)if(ui_glyph[i].cp==cp)break;
            if(i<ui_glyph_count)LCD_ShowGlyph16(x,y,ui_glyph[i].bits,color,GUI_PANEL,0u);
            else LCD_ShowChar(x,y,'?',color,GUI_PANEL,LCD_FONT_16,0u);
            x+=16u;
        }
    }
}

static void text(uint16_t x,uint16_t y,uint16_t w,const char *s,uint16_t color)
{ Media_DrawTextBox(x,y,w,16u,s,color,GUI_PANEL); }

/* 参数更新以字段为单位比较；稳定数值不写屏，不以刷新页面维持“活着”。 */
static void field(uint8_t index,uint16_t x,uint16_t y,uint16_t w,const char *label,const char *value)
{
    char key[64];
    if(index>=10u)return;
    snprintf(key,sizeof(key),"%s:%s",label,value);
    if(!strcmp(row_cache[index],key))return;
    strcpy(row_cache[index],key);
    LCD_Fill(x,y,x+w-1u,y+15u,GUI_PANEL);
    text(x,y,w,label,GUI_MUTED);
    LCD_ShowString((uint16_t)(x+w-strlen(value)*6u),y+4u,value,GUI_INK,GUI_PANEL,LCD_FONT_8,0u);
}

static void page_title(const char *title,const char *counter)
{
    Media_DrawTextBox(12u,38u,LCD_W-82u,18u,title,GUI_INK,GUI_BG);
    LCD_ShowString(LCD_W-64u,44u,counter,GUI_MUTED,GUI_BG,LCD_FONT_8,0u);
}

void Gui_StatusTick(uint32_t now,uint8_t force)
{
    uint8_t online=AILink_LinkAlive(now),i;
    uint16_t x=LCD_W-91u;
    if(CameraLink_Busy() || FontUpload_Active() || (!force && (MicAud_IsStreaming() || IAP_IsBusy() || AILink_OtaPending())))return;
    if(!force && online==link_shown)return;
    link_shown=online;
    LCD_Fill(x,10u,LCD_W-10u,25u,GUI_BG);
    for(i=0u;i<3u;i++)Gui_RoundRect(x+i*5u,22u-i*4u,3u,3u+i*4u,1u,online?GUI_ACCENT:GUI_MUTED);
    /* 只表示ESP链路，不表示互联网或识别服务健康。 */
    LCD_ShowString(x+22u,15u,online?"ESP ON":"ESP OFF",online?GUI_ACCENT:GUI_WARN,GUI_BG,LCD_FONT_8,0u);
}

void Gui_Frame(const char *title,uint8_t tab)
{
    static const char *tabs[3]={"陪伴","设备","设置"};
    uint8_t i, selected=(tab==0u?0u:(tab==1u || tab==4u?1u:2u));
    uint16_t slot=(LCD_W-24u)/3u,x;
    (void)title;
    LCD_Clear(GUI_BG);
    Gui_RoundRect(12u,10u,16u,16u,5u,GUI_SELECT);
    Gui_RoundRect(17u,15u,6u,6u,3u,GUI_ACCENT);
    Media_DrawTextBox(35u,10u,40u,18u,"小D",GUI_INK,GUI_BG);
    Gui_StatusTick(GetTick_ms(),1u);
    Gui_RoundRect(12u,LCD_H-30u,LCD_W-24u,22u,11u,GUI_DOCK);
    for(i=0u;i<3u;i++) {
        x=(uint16_t)(12u+i*slot);
        if(i==selected)Gui_RoundRect(x+2u,LCD_H-28u,slot-4u,18u,9u,GUI_SELECT);
        Media_DrawTextBox(x+(slot-32u)/2u,LCD_H-27u,34u,16u,tabs[i],
                         i==selected?GUI_ACCENT:GUI_MUTED,i==selected?GUI_SELECT:GUI_DOCK);
    }
    if(tab==0u) {
        Gui_RoundRect(LCD_W>LCD_H?8u:12u,36u,LCD_W>LCD_H?164u:216u,168u,14u,GUI_FACE_BG);
        Gui_RoundRect(GUI_AUDIO_X,GUI_AUDIO_Y,GUI_AUDIO_W,GUI_AUDIO_H,13u,GUI_PANEL);
        cache_audio_glyphs();
    }
    memset(row_cache,0,sizeof(row_cache));memset(bars_shown,255,sizeof(bars_shown));
    audio_caption[0]=0;audio_code=255u;audio_due=0u;timer_shown=0xFFFFFFFFu;
}

/* 简单7段数字用于温湿度主数值，避免引入大字体或额外图像资源。 */
static void big_number(uint16_t x,uint16_t y,const char *p)
{
    static const uint8_t segments[10]={0x3Fu,6u,0x5Bu,0x4Fu,0x66u,0x6Du,0x7Du,7u,0x7Fu,0x6Fu};
    static const uint8_t rect[7][4]={{2,0,10,2},{12,2,2,9},{12,13,2,9},{2,22,10,2},{0,13,2,9},{0,2,2,9},{2,11,10,2}};
    uint8_t i,mask;
    while(*p) {
        if(*p=='.'){Gui_RoundRect(x,y+21u,3u,3u,1u,GUI_INK);x+=6u;}
        else if(*p>='0' && *p<='9') {
            mask=segments[*p-'0'];
            for(i=0u;i<7u;i++)if(mask&(1u<<i))Gui_RoundRect(x+rect[i][0],y+rect[i][1],rect[i][2],rect[i][3],1u,GUI_INK);
            x+=18u;
        } else {LCD_ShowChar(x,y+8u,(uint8_t)*p,GUI_INK,GUI_PANEL,LCD_FONT_16,0u);x+=8u;}
        p++;
    }
}

void Gui_ShowDevice(uint32_t uptime)
{
    uint8_t result=DHT11_LastResult(),i,wide=(uint8_t)(LCD_W>LCD_H);
    uint16_t cw=(LCD_W-32u)/2u,x,y;
    char value[40];
    if(!row_cache[9][0]) {
        page_title("设备状态","STATUS");
        Gui_RoundRect(12u,66u,cw,62u,12u,GUI_PANEL);
        Gui_RoundRect(20u+cw,66u,cw,62u,12u,GUI_PANEL);
        text(22u,73u,cw-20u,"温度",GUI_MUTED);text(30u+cw,73u,cw-20u,"湿度",GUI_MUTED);
        Gui_RoundRect(12u,138u,LCD_W-24u,wide?62u:132u,12u,GUI_PANEL);
        strcpy(row_cache[9],"layout");
    }
    for(i=0u;i<2u;i++) {
        if(DHT11_InitOK() && result==0u)snprintf(value,sizeof(value),"%u.%u",i?dht11.humi_int:dht11.temp_int,i?dht11.humi_dec:dht11.temp_dec);
        else strcpy(value,"--.-");
        if(strcmp(row_cache[i],value)) {
            strcpy(row_cache[i],value);x=(uint16_t)(22u+i*(cw+8u));
            LCD_Fill(x,96u,x+cw-21u,121u,GUI_PANEL);big_number(x,96u,value);
            LCD_ShowString(x+cw-28u,108u,i?"%":"C",GUI_ACCENT,GUI_PANEL,LCD_FONT_8,0u);
        }
    }
    for(i=0u;i<4u;i++) {
        static const char *labels[4]={"麦克风","舵机驱动","SD素材","DHT11"};
        x=(uint16_t)(24u+(wide?(i%2u)*148u:0u));y=(uint16_t)(147u+(wide?i/2u:i)*29u);
        if(i==0u)snprintf(value,sizeof(value),"%s",hw.mic_ok && !hw.mic_pin_error?"READY":"ERROR");
        else if(i==1u)strcpy(value,hw.pca_ok?"READY":"NO ACK");
        else if(i==2u)snprintf(value,sizeof(value),"%lu MB",(unsigned long)hw.sd_mb);
        else strcpy(value,result==0u?"READ OK":(result==255u?"WAIT":"READ ERROR"));
        field(i+2u,x,y,wide?124u:192u,labels[i],value);
    }
    (void)uptime;
}

void Gui_ShowMicSettings(void)
{
    MicLink_VadInfo v;char value[40];uint8_t i,wide=(uint8_t)(LCD_W>LCD_H);
    static const char *labels[6]={"自动监听","起录下限","底噪增量","连续有声","静音断句","监听间隔"};
    uint16_t x,y,w;
    MicLink_GetVadInfo(&v);
    if(!row_cache[9][0]) {
        page_title("语音设置","01 / 03");
        for(i=0u;i<6u;i++) {
            x=wide?(uint16_t)(12u+(i/3u)*152u):12u;y=wide?66u:(uint16_t)(66u+i*32u);
            if(!wide || i%3u==0u)Gui_RoundRect(x,y,wide?144u:216u,wide?133u:28u,10u,GUI_PANEL);
        }
        strcpy(row_cache[9],"layout");
    }
    for(i=0u;i<6u;i++) {
        if(i==0u)strcpy(value,v.enabled?"ON":"OFF");
        else if(i==1u)snprintf(value,sizeof(value),"%u RMS",v.minimum);
        else if(i==2u)snprintf(value,sizeof(value),"%u RMS",v.margin);
        else snprintf(value,sizeof(value),"%u ms",i==3u?v.onset_ms:(i==4u?v.silence_ms:MICLINK_VAD_REARM_MS));
        x=wide?(uint16_t)(22u+(i/3u)*152u):22u;y=wide?(uint16_t)(77u+(i%3u)*40u):(uint16_t)(72u+i*32u);w=wide?124u:196u;
        if(wide) {
            if(strcmp(row_cache[i],value)) {
                strcpy(row_cache[i],value);LCD_Fill(x,y,x+w-1u,y+31u,GUI_PANEL);text(x,y,w,labels[i],GUI_MUTED);
                LCD_ShowString(x,y+19u,value,GUI_ACCENT,GUI_PANEL,LCD_FONT_8,0u);
            }
        } else field(i,x,y,w,labels[i],value);
    }
}

void Gui_ShowServoSettings(void)
{
    uint8_t id,wide=(uint8_t)(LCD_W>LCD_H);uint16_t x,y,w;
    AILink_ServoInfo s;char value[64];
    static const char *names[3]={"底座左右","俯仰上下","屏幕偏头"};
    if(!row_cache[9][0]) {
        page_title("动作设置","02 / 03");
        for(id=0u;id<3u;id++)Gui_RoundRect(wide?(uint16_t)(12u+id*101u):12u,wide?66u:(uint16_t)(66u+id*68u),wide?94u:216u,wide?133u:62u,11u,GUI_PANEL);
        strcpy(row_cache[9],"layout");
    }
    for(id=1u;id<=3u;id++) {
        (void)AILink_GetServoInfo(id,&s);x=wide?(uint16_t)(22u+(id-1u)*101u):22u;y=wide?76u:(uint16_t)(74u+(id-1u)*68u);w=wide?74u:196u;
        snprintf(value,sizeof(value),"%u-%u / %u / %u / %u / %u",s.minimum,s.maximum,s.front,s.target,s.speed,s.configured);
        if(!strcmp(row_cache[id-1u],value))continue;
        strcpy(row_cache[id-1u],value);LCD_Fill(x,y,x+w-1u,y+(wide?106u:45u),GUI_PANEL);
        text(x,y,w,names[id-1u],GUI_INK);
        snprintf(value,sizeof(value),"%u-%u deg",s.minimum,s.maximum);
        LCD_ShowString(wide?x:x+112u,y+(wide?26u:3u),value,GUI_ACCENT,GUI_PANEL,LCD_FONT_8,0u);
        snprintf(value,sizeof(value),"%u / %u",s.front,s.target);
        LCD_ShowString(x,y+(wide?52u:27u),value,GUI_MUTED,GUI_PANEL,LCD_FONT_8,0u);
        snprintf(value,sizeof(value),s.configured?"%u deg/s":"WAIT CFG",s.speed);
        LCD_ShowString(wide?x:x+112u,y+(wide?77u:27u),value,GUI_INK,GUI_PANEL,LCD_FONT_8,0u);
    }
}

void Gui_ShowSystem(void)
{
    char value[40];FaceAnim_Info a;
    uint8_t wide=(uint8_t)(LCD_W>LCD_H);
    if(!row_cache[9][0]) {
        page_title("系统信息","03 / 03");
        Gui_RoundRect(12u,66u,LCD_W-24u,58u,12u,GUI_PANEL);
        text(24u,76u,LCD_W-48u,"F407当前固件",GUI_MUTED);
        LCD_ShowString(24u,100u,APP_FW_VERSION,GUI_ACCENT,GUI_PANEL,LCD_FONT_16,0u);
        Gui_RoundRect(12u,134u,LCD_W-24u,wide?67u:136u,12u,GUI_PANEL);
        strcpy(row_cache[9],"layout");
    }
    FaceAnim_GetInfo(&a);
    snprintf(value,sizeof(value),"%ux%u / %s",LCD_W,LCD_H,FontStore_Ready()?"W25Q":"FLASH");
    field(0u,24u,144u,LCD_W-48u,"屏幕字库",value);
    snprintf(value,sizeof(value),"%u.%u FPS",a.measured_fps_x10/10u,a.measured_fps_x10%10u);
    field(1u,24u,174u,LCD_W-48u,"动画实测",value);
    if(!wide) {
        field(2u,24u,204u,LCD_W-48u,"按键","K-UP / PE3");
        snprintf(value,sizeof(value),"RX %u DROP %u",ailink_ok_cnt,ailink_drop_cnt);
        field(3u,24u,234u,LCD_W-48u,"通信",value);
    }
}

void Gui_ShowCamera(void)
{
    char value[64];uint8_t i,wide=(uint8_t)(LCD_W>LCD_H);
    uint16_t y;
    static const char *labels[4]={"相机类型","传输方式","本机采集","识别状态"};
    if(!row_cache[9][0]){page_title("摄像头","CAMERA");Gui_RoundRect(12u,66u,LCD_W-24u,wide?135u:202u,12u,GUI_PANEL);strcpy(row_cache[9],"layout");}
    for(i=0u;i<4u;i++) {
        y=(uint16_t)(78u+i*(wide?29u:45u));
        if(i==0u)strcpy(value,"ESP32-S3");
        else if(i==1u)strcpy(value,"Wi-Fi -> PC");
        else if(i==2u)strcpy(value,"EXTERNAL CAMERA");
        else strcpy(value,"SEE PC WEB");
        field(i,24u,y,LCD_W-48u,labels[i],value);
    }
}

void Gui_UpdateParameters(uint8_t page)
{
    if(page==PAGE_MIC_SETTINGS)Gui_ShowMicSettings();
    else if(page==PAGE_SERVO_SETTINGS)Gui_ShowServoSettings();
    else if(page==PAGE_SYSTEM_SETTINGS)Gui_ShowSystem();
    else if(page==PAGE_CAMERA)Gui_ShowCamera();
}

uint8_t Gui_IsListening(void) { return MicLink_IsRecording(); }

/* 每50ms仅写有变化的窄区域。中文状态已缓存，长AI正文只在空闲时绘制。
 * 计时来自已采集64ms整块，ACK数仅表示F407->ESP确认，不能充当云端百分比。 */
void Gui_AudioTick(uint8_t visible_page,uint32_t now,uint8_t force)
{
    const char *state=MicLink_AudioStateName(),*title,*hint;
    const char *caption=Media_GetCaption();MicLink_UiInfo info;
    uint8_t old_height, max_height, code=0u,i,height,wide=(uint8_t)(LCD_W>LCD_H),target,changed;
    uint16_t x=GUI_AUDIO_X+8u,y=GUI_AUDIO_Y,w=GUI_AUDIO_W-16u,by=wide?156u:240u;
    uint16_t step=wide?9u:16u,bw=wide?5u:10u;
    uint32_t rms,visual,ms;char line[32];
    if(!strncmp(caption,"OTA",3u))return; /* OTA文字覆盖层不可被音量卡片覆盖。 */
    if(visible_page!=PAGE_AI || FontUpload_Active() || IAP_IsBusy() || AILink_OtaPending())return;
    if(!force && (int32_t)(now-audio_due)<0)return;
    audio_due=now+50u;MicLink_GetUiInfo(&info);
    if(strstr(state,"ARMED") || !strcmp(state,"WAIT ESP READY"))code=1u;
    else if(MicAud_IsCapturing())code=(info.automatic && info.quiet_ms>=96u)?3u:2u;
    else if(MicAud_IsStreaming())code=4u;
    else if(!strcmp(state,"UPLOADING"))code=5u;
    else if(!strcmp(state,"QUIET GAP"))code=6u;
    else if(!strcmp(state,"CALIBRATING"))code=7u;
    else if(!strcmp(state,"MANUAL READY"))code=8u;
    changed=(uint8_t)(audio_code!=code);
    if(force || changed) {
        /* 只清卡片内部，不破坏外缘圆角与导航。 */
        if(MicAud_IsStreaming()) {
            LCD_Fill(x,y+8u,x+95u,y+23u,GUI_PANEL);
            LCD_Fill(x,wide?188u:266u,x+111u,wide?195u:273u,GUI_PANEL);
        } else LCD_Fill(x,y+7u,x+w-1u,y+GUI_AUDIO_H-8u,GUI_PANEL);
        audio_code=code;if(!MicAud_IsStreaming())memset(bars_shown,255,sizeof(bars_shown));timer_shown=0xFFFFFFFFu;
        title=code==1u?"录音准备":(code==2u?"正在听":(code==3u?"等待断句":
              (code==4u?"录音收尾":(code==5u?"正在上传":(code==6u?"监听间隔":
              (code==7u?"底噪校准":(code==8u?"手动就绪":"等你开口")))))));
        audio_text(x,y+8u,w,title,code==2u || code==3u?GUI_ACCENT:GUI_INK);
        if(code==1u)last_capture_ms=0u;
        if(code!=2u && code!=3u && code!=4u) {
            /* 此分支不在采集/排空期间；正文可读常规中文字库。 */
            Media_DrawTextBox(x,y+(wide?40u:30u),w,wide?100u:GUI_AUDIO_H-38u,
                             caption[0]?caption:"你好呀，我在这里。",GUI_INK,GUI_PANEL);
            strncpy(audio_caption,caption,sizeof(audio_caption)-1u);audio_caption[sizeof(audio_caption)-1u]=0;
        }
    }
    if(code!=2u && code!=3u && code!=4u && strcmp(audio_caption,caption) && !MicAud_IsStreaming()) {
        LCD_Fill(x,y+(wide?40u:30u),x+w-1u,y+GUI_AUDIO_H-8u,GUI_PANEL);
        Media_DrawTextBox(x,y+(wide?40u:30u),w,wide?100u:GUI_AUDIO_H-38u,
                         caption,GUI_INK,GUI_PANEL);
        strncpy(audio_caption,caption,sizeof(audio_caption)-1u);audio_caption[sizeof(audio_caption)-1u]=0;
    }
    if(code==0u || code==6u || code==8u)return;
    if(code==2u || code==3u)last_capture_ms=(uint32_t)MicAud_CapturedBlocks()*64u;
    ms=last_capture_ms/100u;
    if(timer_shown!=ms || force || changed) {
        timer_shown=ms;
        snprintf(line,sizeof(line),"%02lu.%lus",(unsigned long)(ms/10u),(unsigned long)(ms%10u));
        LCD_Fill(wide?x:x+w-42u,wide?y+34u:y+11u,wide?x+w-1u:x+w-1u,wide?y+43u:y+20u,GUI_PANEL);
        LCD_ShowString(wide?x:x+w-42u,wide?y+34u:y+11u,line,GUI_ACCENT,GUI_PANEL,LCD_FONT_8,0u);
    }
    /* 状态文字只用内置ASCII；采音时无SD或SPI字库访问。 */
    if(force || changed) {
        hint=code==1u?"WAIT ESP READY":(code==4u?"DRAIN TO ESP":(code==5u?"WAIT UPLOAD ACK":
             (code==7u?"NOISE CALIBRATION":(info.automatic?"AUTO END ON PAUSE":"FIXED DURATION"))));
        LCD_ShowString(x,wide?188u:266u,hint,GUI_MUTED,GUI_PANEL,LCD_FONT_8,0u);
    }
    rms=INMP441_Rms();if(rms>32768u)rms=32768u;
    visual=rms>20u?rms-20u:0u;target=(uint8_t)(visual*100u/(visual+160u));
    if(code!=2u && code!=3u)meter_level=0u;
    else if(target>=meter_level)meter_level=target;
    else meter_level=(uint8_t)((meter_level*2u+target)/3u);
    for(i=0u;i<12u;i++) {
        height=(uint8_t)((uint16_t)meter_level*(18u+(i%4u)*2u)/100u);if(height<3u)height=3u;
        if(height==bars_shown[i] && !changed)continue;
        old_height=bars_shown[i]==255u?24u:bars_shown[i];
        max_height=old_height>height?old_height:height;
        LCD_Fill(x+i*step,by+(24u-max_height)/2u,x+i*step+bw-1u,by+(24u-max_height)/2u+max_height-1u,GUI_PANEL);
        Gui_RoundRect(x+i*step,by+(24u-height)/2u,bw,height,2u,code==2u?GUI_ACCENT:GUI_MUTED);
        bars_shown[i]=height;
    }
    if(code==3u || code==4u) {
        if(code==3u)snprintf(line,sizeof(line),"QUIET %4u/%4ums",info.quiet_ms,info.quiet_limit_ms);
        else snprintf(line,sizeof(line),"ACK %4u BLK",MicAud_SentBlocks());
        LCD_Fill(x,wide?136u:266u,x+111u,wide?143u:273u,GUI_PANEL);
        LCD_ShowString(x,wide?136u:266u,line,GUI_MUTED,GUI_PANEL,LCD_FONT_8,0u);
    }
}

/* Nearest right angle, with 10-degree hysteresis and 150ms stability.
 * Only use acknowledged PWM estimates; unknown hardware holds landscape.
 * 90=landscape, 0/180=the two portrait directions. No redraw during audio/OTA. */
uint8_t Gui_OrientationTick(uint32_t now)
{
    static uint8_t candidate=255u;
    static uint32_t candidate_at;
    uint8_t current=LCD_GetOrientation(), wanted=current;
    uint16_t angle;
    if(CameraLink_Busy() || MicLink_IsRecording() || MicAud_IsStreaming() || IAP_IsBusy() || AILink_OtaPending()) {
        candidate=255u; return 0u;
    }
    if(!AILink_GetScreenAngle(&angle)) { candidate=255u; return 0u; }
    if(current==LCD_PORTRAIT_0) {
        if(angle>=145u) wanted=LCD_PORTRAIT_180;
        else if(angle>=55u) wanted=LCD_LANDSCAPE_90;
    } else if(current==LCD_PORTRAIT_180) {
        if(angle<=35u) wanted=LCD_PORTRAIT_0;
        else if(angle<=125u) wanted=LCD_LANDSCAPE_90;
    } else {
        if(angle<=35u) wanted=LCD_PORTRAIT_0;
        else if(angle>=145u) wanted=LCD_PORTRAIT_180;
    }
    if(wanted==current) { candidate=255u; return 0u; }
    if(wanted!=candidate) { candidate=wanted; candidate_at=now; return 0u; }
    if((int32_t)(now-candidate_at)<150) return 0u;
    LCD_SetOrientation(wanted); candidate=255u;
    printf("[GUI] orientation=%u %ux%u roll=%u\r\n",(unsigned)wanted,
           (unsigned)LCD_W,(unsigned)LCD_H,(unsigned)angle);
    return 1u;
}
