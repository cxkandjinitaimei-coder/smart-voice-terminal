/**
 * @file robot_gui.h
 * 用途：表情页和参数页的布局、顶部状态栏、录音反馈及方向适配。
 * 使用：LCD 与媒体层初始化后由主循环绘制；状态变动触发小区域更新，设置页显示版本。
 * 时序与边界：UI 读取模块快照，不自行启动录音/升级；录音时优先更新状态条，避免 SD 动画阻塞 DMA 消费。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef ROBOT_GUI_H
#define ROBOT_GUI_H
#include "lcd.h"
#include "media.h"

#define GUI_BG LCD_RGB(240, 229, 198)
#define GUI_INK LCD_RGB(60, 72, 56)
#define GUI_MUTED LCD_RGB(125, 128, 96)
#define GUI_LINE LCD_RGB(196, 191, 155)
#define GUI_PANEL LCD_RGB(225, 216, 182)

typedef struct
{
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
void Gui_UpdateParameters(uint8_t page);
uint8_t Gui_OrientationTick(uint32_t now);
/* SD-only BMP assets, drawn once before capture; no full-frame RAM buffer. */
#define GUI_HEAD_NORMAL_BMP "FACE/GUINORM.BMP"
#define GUI_HEAD_LISTEN_BMP "FACE/GUILIST.BMP"
void Gui_DrawHead(uint8_t listening);
void Gui_AudioTick(uint8_t visible_page, uint32_t now, uint8_t force);
uint8_t Gui_IsListening(void);
#endif
