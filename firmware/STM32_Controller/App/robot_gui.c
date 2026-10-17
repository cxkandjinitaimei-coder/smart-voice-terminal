/**
 * @file robot_gui.c
 * 用途：表情页和参数页的布局、顶部状态栏、录音反馈及方向适配。
 * 使用：LCD 与媒体层初始化后由主循环绘制；状态变动触发小区域更新，设置页显示版本。
 * 时序与边界：UI 读取模块快照，不自行启动录音/升级；录音时优先更新状态条，避免 SD 动画阻塞 DMA 消费。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "camera_link.h"
#include "robot_gui.h"
#include "product_config.h"
#include "app_version.h"
#include "miclink.h"
#include "micaud.h"
#include "inmp441.h"
#include "ailink.h"
#include "esp_uart.h"
#include "dht11.h"
#include "font_store.h"
#include "font_upload.h"
#include "iap_app.h"
#include "camera_ov7725.h"
#include "face_anim.h"
#include "../System/time.h"
#include <stdio.h>
#include <string.h>

static GuiHardware hw;
static char row_cache[10][80];
static char audio_shown[32];
static uint8_t bars_shown[12];
static uint32_t audio_due;
static uint8_t link_shown = 255u, meter_level;

void Gui_SetHardware(const GuiHardware *hardware)
{
    hw = *hardware;
}

static void row(uint8_t index, const char *text, uint16_t color)
{
    uint8_t landscape = (uint8_t)(LCD_W > LCD_H);
    uint16_t y = (uint16_t)((landscape ? 36u : 46u) + index * (landscape ? 17u : 24u));
    if (index >= 10u || !strcmp(row_cache[index], text))
        return;
    strncpy(row_cache[index], text, 79u);
    row_cache[index][79] = 0;
    LCD_Fill(10u, y, LCD_W - 11u, y + (landscape ? 15u : 19u), GUI_BG);
    Media_DrawTextBox(12u, y + (landscape ? 0u : 1u), LCD_W - 24u, landscape ? 16u : 18u, text,
                      color, GUI_BG);
    LCD_DrawLine(10u, y + (landscape ? 16u : 21u), LCD_W - 11u, y + (landscape ? 16u : 21u),
                 GUI_LINE);
}

void Gui_StatusTick(uint32_t now, uint8_t force)
{
    uint8_t online = AILink_LinkAlive(now), i;
    uint16_t x = (uint16_t)(LCD_W - 96u);
    if (CameraLink_Busy() || FontUpload_Active() ||
        (!force && (MicAud_IsStreaming() || IAP_IsBusy() || AILink_OtaPending())))
        return;
    if (!force && online == link_shown)
        return;
    link_shown = online;
    LCD_Fill(x, 5u, LCD_W - 7u, 24u, GUI_INK);
    /* A link indication, not a claim that the cloud service is reachable. */
    for (i = 0u; i < 3u; i++)
        LCD_Fill(x + i * 5u, 20u - i * 4u, x + i * 5u + 2u, 22u, online ? GUI_BG : GUI_MUTED);
    LCD_ShowString(x + 20u, 10u, online ? "ESP ON" : "ESP OFF", GUI_BG, GUI_INK, LCD_FONT_8,
                   LCD_MODE_NORMAL);
}

void Gui_Frame(const char *title, uint8_t tab)
{
    static const char *tabs[5] = {"FACE", "DEVICE", "MIC", "SERVO", "CAM"};
    uint8_t i;
    LCD_Clear(GUI_BG);
    LCD_Fill(0u, 0u, LCD_W - 1u, 29u, GUI_INK);
    Media_DrawTextBox(10u, 7u, 40u, 18u, ROBOT_DISPLAY_NAME, GUI_BG, GUI_INK);
    if (tab)
        LCD_ShowString(54u, 10u, tabs[tab < 5u ? tab : 0u], GUI_BG, GUI_INK, LCD_FONT_8,
                       LCD_MODE_NORMAL);
    (void)title; /* Detailed content is shown by the selected page. */
    Gui_StatusTick(GetTick_ms(), 1u);
    if (tab == 2u || tab == 3u)
        LCD_ShowString(8u, LCD_H - 28u, "FW " APP_FW_VERSION, GUI_MUTED, GUI_BG, LCD_FONT_8,
                       LCD_MODE_NORMAL);
    LCD_DrawLine(8u, LCD_H - 16u, LCD_W - 9u, LCD_H - 16u, GUI_LINE);
    for (i = 0u; i < 5u; i++)
    {
        LCD_ShowString((uint16_t)(8u + i * (LCD_W - 10u) / 5u), LCD_H - 11u, tabs[i],
                       i == tab ? GUI_INK : GUI_MUTED, GUI_BG, LCD_FONT_8, LCD_MODE_NORMAL);
        if (i == tab)
            LCD_Fill((uint16_t)(8u + i * (LCD_W - 10u) / 5u), LCD_H - 17u,
                     (uint16_t)(37u + i * (LCD_W - 10u) / 5u), LCD_H - 16u, GUI_INK);
    }
    memset(row_cache, 0, sizeof(row_cache));
    memset(bars_shown, 255, sizeof(bars_shown));
    audio_shown[0] = 0;
    audio_due = 0u;
}

void Gui_ShowDevice(uint32_t uptime)
{
    char line[80];
    uint8_t online = AILink_LinkAlive(GetTick_ms());
    uint8_t result = DHT11_LastResult();
    sprintf(line, "ESP通信 %s", online ? "OK" : "LOST");
    row(0u, line, online ? GUI_INK : LCD_BROWN);
    sprintf(line, "麦克风 %s",
            !hw.mic_ok ? "INIT FAIL" : (hw.mic_pin_error ? "PIN FAIL" : "INIT OK"));
    row(1u, line, hw.mic_ok && !hw.mic_pin_error ? GUI_INK : LCD_RED);
    sprintf(line, "DMA %lu RMS %lu", (unsigned long)INMP441_Blocks(), (unsigned long)INMP441_Rms());
    row(2u, line, GUI_INK);
    sprintf(
        line, "DHT11 %s",
        !DHT11_InitOK()
            ? "INIT FAIL"
            : (result == 255u
                   ? "WAIT"
                   : (result == 0u ? "READ OK" : (result == 1u ? "NO RESPONSE" : "READ ERROR"))));
    row(3u, line, result == 0u ? GUI_INK : LCD_BROWN);
    if (DHT11_InitOK() && result == 0u)
        sprintf(line, "温湿度 %u.%uC / %u.%u%%", (unsigned)dht11.temp_int, (unsigned)dht11.temp_dec,
                (unsigned)dht11.humi_int, (unsigned)dht11.humi_dec);
    else
        strcpy(line, "温湿度 --.-C / --.-%");
    row(4u, line, GUI_INK);
    sprintf(line, "舵机PWM %s", hw.pca_ok ? "PCA9685 OK" : "PCA9685 FAIL");
    row(5u, line, GUI_INK);
    sprintf(line, "SD %s %luMB", hw.sd_ok ? "OK" : "FAIL", (unsigned long)hw.sd_mb);
    row(6u, line, GUI_INK);
    sprintf(line, "字库 %s LCD %04X", FontStore_Ready() ? "W25Q" : "FLASH", hw.lcd_id);
    row(7u, line, GUI_INK);
    sprintf(line, "UP %lus RX %u DROP %u", (unsigned long)uptime, (unsigned)ailink_ok_cnt,
            (unsigned)ailink_drop_cnt);
    row(8u, line, GUI_MUTED);
    row(9u, "K-UP: 下一页 / NEXT", GUI_MUTED);
}

void Gui_ShowMicSettings(void)
{
    MicLink_VadInfo v;
    char line[80];
    MicLink_GetVadInfo(&v);
    sprintf(line, "自动监听 %s", v.enabled ? "ON" : "OFF");
    row(0u, line, GUI_INK);
    sprintf(line, "起录下限 %u RMS", v.minimum);
    row(1u, line, GUI_INK);
    sprintf(line, "底噪增量 %u RMS", v.margin);
    row(2u, line, GUI_INK);
    sprintf(line, "连续有声 %u ms", v.onset_ms);
    row(3u, line, GUI_INK);
    sprintf(line, "静音结束 %u ms", v.silence_ms);
    row(4u, line, GUI_INK);
    sprintf(line, "单句上限 %u s", MICLINK_VAD_MAX_SEC);
    row(5u, line, GUI_INK);
    sprintf(line, "监听间隔 %u ms", MICLINK_VAD_REARM_MS);
    row(6u, line, GUI_INK);
    sprintf(line, "当前门限 %lu RMS", (unsigned long)v.threshold);
    row(7u, line, GUI_MUTED);
    {
        FaceAnim_Info anim;
        FaceAnim_GetInfo(&anim);
        if (anim.measured_fps_x10)
            sprintf(line, "动画 %u/%u.%u FPS", (unsigned)(1000u / anim.interval_ms),
                    (unsigned)(anim.measured_fps_x10 / 10u),
                    (unsigned)(anim.measured_fps_x10 % 10u));
        else
            sprintf(line, "动画 %u/-- FPS", (unsigned)(1000u / anim.interval_ms));
        row(8u, line, GUI_MUTED);
    }
    row(9u, "网页修改 / K-UP下一页", GUI_MUTED);
}

void Gui_ShowServoSettings(void)
{
    uint8_t id;
    AILink_ServoInfo s;
    char line[80];
    for (id = 1u; id <= 3u; id++)
    {
        (void)AILink_GetServoInfo(id, &s);
        sprintf(line, "轴%u 限位 %u - %u", id, s.minimum, s.maximum);
        row((uint8_t)((id - 1u) * 3u), line, GUI_INK);
        sprintf(line, "正面 %u 目标 %u", s.front, s.target);
        row((uint8_t)((id - 1u) * 3u + 1u), line, GUI_INK);
        if (s.configured)
            sprintf(line, "速度 %u deg/s", s.speed);
        else
            strcpy(line, "配置 WAIT ESP PROFILE");
        row((uint8_t)((id - 1u) * 3u + 2u), line, GUI_MUTED);
    }
    {
        uint16_t angle;
        if (AILink_GetScreenAngle(&angle))
            sprintf(line, "S3 PWM %u %s", angle, LCD_W > LCD_H ? "LANDSCAPE" : "PORTRAIT");
        else
            strcpy(line, "S3 PWM WAIT / NO ACK");
        row(9u, line, GUI_MUTED);
    }
}

void Gui_ShowCamera(void)
{
    const Camera_OV7725_Info *c = Camera_OV7725_GetInfo();
    char line[80];
    row(0u, "OV7725 + FIFO", GUI_INK);
    row(1u, c->initialized ? "驱动 INITIALIZED" : "驱动 DISABLED", GUI_INK);
    if (c->initialized)
        sprintf(line, "SCCB MID %04X PID %04X", c->manufacturer_id, c->product_id);
    else
        strcpy(line, "SCCB NOT TESTED");
    row(2u, line, GUI_MUTED);
    row(3u, "QVGA 320x240 RGB565", GUI_INK);
    row(4u, c->frame_ready ? "FIFO FRAME READY" : "FIFO WAITING", GUI_INK);
    row(5u, "RCLK PD6 / ESP PC6", GUI_MUTED);
    row(6u, "分块读取 / NO FRAME RAM", GUI_INK);
    sprintf(line, "%s %lu/153600", CameraLink_State(), (unsigned long)CameraLink_Bytes());
    row(7u, line, GUI_INK);
    row(8u, "网页请求单张拍照", GUI_MUTED);
    row(9u, "RGB565 -> ESP -> SERVER", GUI_MUTED);
}

void Gui_UpdateParameters(uint8_t page)
{
    if (page == PAGE_MIC_SETTINGS)
        Gui_ShowMicSettings();
    else if (page == PAGE_SERVO_SETTINGS)
        Gui_ShowServoSettings();
    else if (page == PAGE_CAMERA)
        Gui_ShowCamera();
}

uint8_t Gui_IsListening(void)
{
    return MicLink_IsRecording();
}

void Gui_AudioTick(uint8_t visible_page, uint32_t now, uint8_t force)
{
    const char *state = MicLink_AudioStateName();
    uint8_t i, height, recording;
    uint16_t x;
    uint8_t landscape = (uint8_t)(LCD_W > LCD_H);
    uint16_t ax = landscape ? 180u : 12u, ay = landscape ? 164u : 208u;
    uint16_t bottom = landscape ? 199u : 247u;
    uint32_t rms, visual;
    uint8_t target;
    if (CameraLink_Busy())
        return;
    if (visible_page != PAGE_AI || IAP_IsBusy() || AILink_OtaPending())
        return;
    if (!force && (int32_t)(now - audio_due) < 0)
        return;
    audio_due = now + 50u;
    if (force || strcmp(audio_shown, state))
    {
        strncpy(audio_shown, state, sizeof(audio_shown) - 1u);
        audio_shown[sizeof(audio_shown) - 1u] = 0;
        LCD_Fill(ax, ay, LCD_W - 9u, ay + (landscape ? 7u : 15u), GUI_BG);
        LCD_ShowString(ax, ay, state, GUI_INK, GUI_BG, landscape ? LCD_FONT_8 : LCD_FONT_16,
                       LCD_MODE_NORMAL);
    }
    recording = MicAud_IsCapturing();
    /* Visual gain/compression only: PCM and VAD remain untouched.
     * RMS 20 -> rest, 100 -> 33%, 300 -> 64%, 1000 -> 85%. */
    rms = INMP441_Rms();
    if (rms > 32768u)
        rms = 32768u;
    visual = rms > 20u ? rms - 20u : 0u;
    target = (uint8_t)(visual * 100u / (visual + 160u));
    if (!recording)
        meter_level = 0u;
    else if (target >= meter_level)
        meter_level = target;
    else
        meter_level = (uint8_t)((meter_level * 2u + target) / 3u);
    for (i = 0u; i < 12u; i++)
    {
        /* Each segment has a different fixed sensitivity to the same measured
         * RMS level. This is a volume meter, not a spectrum or fake progress. */
        height = recording ? (uint8_t)((uint16_t)meter_level * (18u + (i % 4u) * 2u) / 100u) : 2u;
        if (height < 2u)
            height = 2u;
        if (force || height != bars_shown[i])
        {
            x = (uint16_t)(ax + 2u + i * (landscape ? 10u : 18u));
            if (force || bars_shown[i] == 255u)
            {
                LCD_Fill(x, bottom - 23u, x + (landscape ? 5u : 11u), bottom, GUI_PANEL);
                LCD_Fill(x, (uint16_t)(bottom + 1u - height), x + (landscape ? 5u : 11u), bottom,
                         GUI_INK);
            }
            else if (height > bars_shown[i])
                LCD_Fill(x, (uint16_t)(bottom + 1u - height), x + (landscape ? 5u : 11u),
                         (uint16_t)(bottom - bars_shown[i]), GUI_INK);
            else
                LCD_Fill(x, (uint16_t)(bottom + 1u - bars_shown[i]), x + (landscape ? 5u : 11u),
                         (uint16_t)(bottom - height), GUI_PANEL);
            bars_shown[i] = height;
        }
    }
}

/* Nearest right angle, with 10-degree hysteresis and 150ms stability.
 * Only use acknowledged PWM estimates; unknown hardware holds landscape.
 * 90=landscape, 0/180=the two portrait directions. No redraw during audio/OTA. */
uint8_t Gui_OrientationTick(uint32_t now)
{
    static uint8_t candidate = 255u;
    static uint32_t candidate_at;
    uint8_t current = LCD_GetOrientation(), wanted = current;
    uint16_t angle;
    if (CameraLink_Busy() || MicLink_IsRecording() || MicAud_IsStreaming() || IAP_IsBusy() ||
        AILink_OtaPending())
    {
        candidate = 255u;
        return 0u;
    }
    if (!AILink_GetScreenAngle(&angle))
    {
        candidate = 255u;
        return 0u;
    }
    if (current == LCD_PORTRAIT_0)
    {
        if (angle >= 145u)
            wanted = LCD_PORTRAIT_180;
        else if (angle >= 55u)
            wanted = LCD_LANDSCAPE_90;
    }
    else if (current == LCD_PORTRAIT_180)
    {
        if (angle <= 35u)
            wanted = LCD_PORTRAIT_0;
        else if (angle <= 125u)
            wanted = LCD_LANDSCAPE_90;
    }
    else
    {
        if (angle <= 35u)
            wanted = LCD_PORTRAIT_0;
        else if (angle >= 145u)
            wanted = LCD_PORTRAIT_180;
    }
    if (wanted == current)
    {
        candidate = 255u;
        return 0u;
    }
    if (wanted != candidate)
    {
        candidate = wanted;
        candidate_at = now;
        return 0u;
    }
    if ((int32_t)(now - candidate_at) < 150)
        return 0u;
    LCD_SetOrientation(wanted);
    candidate = 255u;
    printf("[GUI] orientation=%u %ux%u roll=%u\r\n", (unsigned)wanted, (unsigned)LCD_W,
           (unsigned)LCD_H, (unsigned)angle);
    return 1u;
}
