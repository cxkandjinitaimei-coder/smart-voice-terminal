/**
 * @file robot_gui.h
 * @brief 深色圆角GUI对外接口与RGB565调色板。
 * 使用：主循环先设置硬件观测值，进入页面时画框架，之后按状态做局部更新。
 * 本接口不修改VAD/舵机参数；表情像素仍来自SD，不分配全屏缓存。
 */
#ifndef ROBOT_GUI_H
#define ROBOT_GUI_H
#include "lcd.h"
#include "media.h"

/* 深色圆角UI调色板，RGB565；无需全屏缓存。 */
#define GUI_BG LCD_RGB(12,18,29)
#define GUI_INK LCD_RGB(238,245,248)
#define GUI_MUTED LCD_RGB(144,161,183)
#define GUI_LINE LCD_RGB(38,54,74)
#define GUI_PANEL LCD_RGB(21,32,49)
#define GUI_ACCENT LCD_RGB(105,226,194)
#define GUI_WARN LCD_RGB(240,199,127)
#define GUI_DOCK LCD_RGB(17,27,41)
#define GUI_SELECT LCD_RGB(41,67,62)
#define GUI_FACE_BG LCD_RGB(16,27,44)
#define GUI_AUDIO_X ((LCD_W>LCD_H)?182u:12u)
#define GUI_AUDIO_Y ((LCD_W>LCD_H)?36u:212u)
#define GUI_AUDIO_W ((LCD_W>LCD_H)?130u:216u)
#define GUI_AUDIO_H ((LCD_W>LCD_H)?168u:70u)

typedef struct {
    uint16_t lcd_id;
    uint8_t sd_ok, pca_ok, mic_ok, mic_pin_error;
    uint32_t sd_mb;
} GuiHardware;

void Gui_SetHardware(const GuiHardware *hardware);
void Gui_StatusTick(uint32_t now, uint8_t force);
void Gui_Frame(const char *title, uint8_t tab);
void Gui_ShowDevice(uint32_t uptime);
void Gui_ShowMicSettings(void);
void Gui_ShowServoSettings(void);
void Gui_ShowCamera(void);
void Gui_ShowSystem(void);
void Gui_RoundRect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint8_t radius,uint16_t color);
void Gui_UpdateParameters(uint8_t page);
uint8_t Gui_OrientationTick(uint32_t now);
/* SD-only BMP assets, drawn once before capture; no full-frame RAM buffer. */
#define GUI_HEAD_NORMAL_BMP "FACE/GUINORM.BMP"
#define GUI_HEAD_LISTEN_BMP "FACE/GUILIST.BMP"
void Gui_DrawHead(uint8_t listening);
void Gui_AudioTick(uint8_t visible_page, uint32_t now, uint8_t force);
uint8_t Gui_IsListening(void);
#endif
