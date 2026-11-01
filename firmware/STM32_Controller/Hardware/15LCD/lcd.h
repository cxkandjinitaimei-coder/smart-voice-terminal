#ifndef __LCD_H
#define __LCD_H

#include "stm32f4xx.h"

/* ==================================================================
 * TFT LCD 并口屏驱动（ILI9341，240×320 竖屏，8080 模式走 FSMC NE1）
 * ------------------------------------------------------------------
 * 移植：换板子/换屏只改本文件的宏，lcd.c 不用动。
 *
 * 【总线】FSMC Bank1 NOR/SRAM1，16 位数据宽度，异步模式 B
 *   CS = PD7  → FSMC_NE1   片选
 *   RS = PD13 → FSMC_A18   0 = 写命令/寄存器   1 = 读写数据
 *   WR = PD5  → FSMC_NWE   写选通
 *   RD = PD4  → FSMC_NOE   读选通（读 ID / 读显存用；不接也能点亮，但读 ID 会是 0xFFFF）
 *   地址换算：16 位总线下 HADDR[25:1]↔FSMC_A[24:0]，
 *             所以 A18 ↔ HADDR 位 19 → 偏移 1<<19 = 0x80000
 *             命令地址 0x60000000    数据地址 0x60080000
 *   ⚠️ 这个偏移成立的前提是 FSMC_MemoryDataWidth_16b。
 *      哪天改成 8 位宽度，偏移要跟着改成 1<<18 = 0x40000。
 *
 * 【数据线】本板是照 FSMC 标准引出的，丝印与 AF 一一对应：
 *   D0=PD14  D1=PD15  D2=PD0   D3=PD1   D4=PE7   D5=PE8   D6=PE9
 *   D7=PE10  D8=PE11  D9=PE12  D10=PE13 D11=PE14 D12=PE15
 *   D13=PD8  D14=PD9  D15=PD10
 *
 * 【复位】RST 接在板级复位上（上电自动复位），驱动不控制 RST。
 *
 * 【背光】★★ 2026-09-17 **已由实物定案：PB1 高电平 = 亮**（`LCD_BL_ACTIVE_HIGH = 1`）
 *   两条独立证据，都很好复现：
 *     ① **按住板上的 RST 不放 ⇒ 背光亮**。按住 RST 时 MCU 在复位态、PB1 是
 *        **悬空输入**，而老代码把 PB1 推挽拉低时会灭 ⇒ **亮 = 高电平**。
 *     ② **开机时"背光闪两下白"**（`lcd.c::lcd_bl_selftest` 的两个亮相位，
 *        都是 `LCD_Backlight(1)` = PB1 高）。
 *   ⇒ 屏的背光电由 J1 的 VCC 供给，**不需要额外往第 28 针喂 5V**。
 *
 * 【屏是 32 针，直插 J1】和板上 J1（2×16）天生配套，不用飞线、不用额外接线。
 *   针脚定义看 `ST32/AIAssistant/板载TFT.png` 那张 J1 表（唯一依据）。
 *
 * ⚠️ PE13/PE14 是 LCD 的 D10/D11，**别拿来当普通 GPIO**（历史上有 LED 类模块用过）。
 * ⚠️ 触摸（TCS/TSCK/MISO/MOSI/IRQ = PB12/PB13/PB14/PB15/PC5）本驱动一律不用。
 *
 * 【字库】文本显示用的字模在 lcd_font.h 里，**只由 lcd.c 包含**。
 *    那 6 个汉字是 重/庆/粤/嵌/科/技（no = 0~5）。想显示别的字得先补字模。
 * ================================================================== */

/* ---------- 屏幕分辨率 ----------
 * 若图片右边/下边被裁掉或出现错位花边，说明实际分辨率不同：
 *   320×480 的屏（ILI9486/ST7796 常见）改成 320 / 480
 *   480×320、240×240 同理。换分辨率的同时记得看 LCD_Init 里的 MADCTL。 */
#define LCD_MAX_WIDTH 320u
#define LCD_MAX_HEIGHT 320u
extern uint16_t lcd_width, lcd_height;
#define LCD_W lcd_width
#define LCD_H lcd_height
enum { LCD_PORTRAIT_0, LCD_LANDSCAPE_90, LCD_PORTRAIT_180, LCD_LANDSCAPE_270 };
void LCD_SetOrientation(uint8_t orientation);
uint8_t LCD_GetOrientation(void);

/* ---------- 背光极性 ----------
 * ★ 2026-09-17 **实测定案 = 1**（PB1 高电平点亮）。判据见文件头的【背光】那一节。
 * 若哪天换屏/改线后上电全黑，把这个宏在 1↔0 之间翻一次再烧 ——
 * `LCD_Init()` 开头的背光自检会把两种状态各亮一次，**肉眼直接能看出哪种是亮的**。 */
#define LCD_BL_ACTIVE_HIGH   1

/* ---------- RGB565 常用色 ---------- */
#define LCD_RGB(r,g,b)  ((uint16_t)((((uint16_t)(r) & 0xF8) << 8) | \
                                    (((uint16_t)(g) & 0xFC) << 3) | \
                                    ((uint16_t)(b) >> 3)))
#define LCD_BLACK    0x0000
#define LCD_WHITE    0xFFFF
#define LCD_RED      0xF800
#define LCD_GREEN    0x07E0
#define LCD_BLUE     0x001F
#define LCD_YELLOW   0xFFE0
#define LCD_CYAN     0x07FF
#define LCD_MAGENTA  0xF81F
#define LCD_GRAY     0x8430
#define LCD_ORANGE   0xFD20
#define LCD_PURPLE   0x781F
#define LCD_BROWN    0xBC40
#define LCD_PINK     0xFC18
#define LCD_DARKBLUE 0x019F

/* ---------- 字模尺寸（传给 LCD_ShowChar/String/Num 的 size 参数） ---------- */
#define LCD_FONT_16     16      /* 8×16 ASCII（LCD_F8X16） */
#define LCD_FONT_8      8       /* 6×8  小字模（LCD_F6x8） */

/* ---------- 显示模式（字符/汉字） ---------- */
#define LCD_MODE_NORMAL 0       /* 叠加：笔画画 fc，背景填 bc */
#define LCD_MODE_TRANS  1       /* 透明：只画笔画，不动背景 */

/* ---------- 函数 ---------- */
void     LCD_Init(void);        /* GPIO + FSMC + ILI9341 上电序列 + 点亮背光 + 白闪自检 */
/* Main-loop cooperative hook; must never access LCD or dispatch commands. */
void     LCD_SetServiceHook(void (*hook)(void));
void     LCD_Backlight(uint8_t on);   /* 背光开(1)/关(0) */
uint16_t LCD_ReadID(void);      /* 读屏 ID 并把原始值 printf 出来；RD 没接会得 0xFFFF/0 */
void     LCD_Clear(uint16_t color);   /* 全屏填一色 */
void     LCD_Fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void     LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void     LCD_ColorBar(void);    /* 8 条竖向彩条：验"能出图"+"颜色顺序有没有反" */

/* ---------- 显存窗口 / 诊断 ---------- */
/* 设窗口并进入"写显存"状态：之后连续写的就是这一块里的像素 */
void     LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
/* 诊断用：写命令 reg 后连续读回 len 个 16 位数据（判断读通道/面板是否应答） */
void     LCD_ReadReg(uint16_t reg, uint16_t *buf, uint8_t len);
/* 读回一个像素（调试/取色用；读显存需先空读一次，已封装） */
uint16_t LCD_ReadPoint(uint16_t x, uint16_t y);
/* 整块刷位图：把 RAM 里的 w×h 个 RGB565 像素写到 (x,y)（图片显示用，越界自动裁） */
void     LCD_ShowBuf(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *buf);

/* ---------- 线条 / 图形 ---------- */
void     LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void     LCD_DrawRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void     LCD_DrawCircle(uint16_t xc, uint16_t yc, uint16_t r, uint16_t color);

/* ---------- 文本 ---------- */
/* chr  : ASCII 可见字符（< ' ' 会被当成空格）
 * size : LCD_FONT_16（8×16）或 LCD_FONT_8（6×8）
 * mode : LCD_MODE_NORMAL 带背景 / LCD_MODE_TRANS 透明 */
void     LCD_ShowChar(uint16_t x, uint16_t y, uint8_t chr,
                      uint16_t fc, uint16_t bc, uint8_t size, uint8_t mode);
/* 超出行宽自动换行（换到 x=0、下一行） */
void     LCD_ShowString(uint16_t x, uint16_t y, const char *p,
                        uint16_t fc, uint16_t bc, uint8_t size, uint8_t mode);
/* 定长显示，前导 0 显示成空格 */
void     LCD_ShowNum(uint16_t x, uint16_t y, uint32_t num, uint8_t len,
                     uint16_t fc, uint16_t bc, uint8_t size);
/* 16×16 汉字，**no 合法范围 0~5**（重/庆/粤/嵌/科/技），越界会被夹住
   —— 走 lcd_font.h 的 LCD_Hzk（列优先格式），只有这 6 个字，够用别加 */
void     LCD_ShowChinese(uint16_t x, uint16_t y, uint8_t no,
                         uint16_t fc, uint16_t bc, uint8_t mode);

/* 16×16 汉字，字模由调用方给：g = 32 字节，**逐行、每行 2 字节、高位在左**。
   g == 0 时什么都不画（查不到字的正常路径，不用先判断）。
   ★ 格式与上面 LCD_ShowChinese 用的 LCD_Hzk **不是一套**，详见 lcd.c 里的注释。
   汉字从哪来：App/font.c 的 Font_Glyph16() 查 App/font_data.h。 */
void     LCD_ShowGlyph16(uint16_t x, uint16_t y, const uint8_t *g,
                         uint16_t fc, uint16_t bc, uint8_t mode);

#endif
