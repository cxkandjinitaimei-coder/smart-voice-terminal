#ifndef MEDIA_H
#define MEDIA_H
#include "stm32f4xx.h"
#include "lcd.h"        /* ★ 下面 FACE_BOX_* / CAPTION_* 要用 LCD_W / LCD_H */

/* 固定160像素表情区，兼容现有SD的R16/BMP，不缩放或改变帧序。
 * 卡片和对话坐标由robot_gui维护；横屏左图右卡，竖屏上图下卡。 */
#define FACE_BOX_SIZE 160u
#define FACE_BOX_X ((LCD_W>LCD_H)?10u:((LCD_W-FACE_BOX_SIZE)/2u))
#define FACE_BOX_Y 40u

/* SD 卡上的表情目录（绝对路径；_FS_RPATH=0 时 FatFs 仍认前导 '/'） */
#define FACE_DIR        "FACE"

/* ★ PAGE_AI 插在 PAGE_COUNT **之前**，所以老页号一个都没变
 *   （0=状态 1=彩条 2=相册 3=文字 4=AI），K-UP 自动变成 5 页循环。 */
/* Keep legacy command page numbers; GUI navigation uses Media_NextPage. */
enum { PAGE_STATUS, PAGE_COLOR, PAGE_PHOTO, PAGE_TEXT, PAGE_AI,
       PAGE_MIC_SETTINGS, PAGE_SERVO_SETTINGS, PAGE_CAMERA, PAGE_SYSTEM_SETTINGS, PAGE_COUNT, PAGE_KEEP = 255 };
uint8_t Media_NextPage(uint8_t page);
void Media_DrawTextBox(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const char *text, uint16_t foreground, uint16_t background);
void Media_Init(uint8_t sd_ok);
uint16_t Media_PhotoCount(void);
void Media_ShowPhoto(void);
void Media_ShowText(void);
/* Updates selection/text. Caller paints the returned page; KEEP=no change. */
uint8_t Media_Command(const char *line);

/* ---------- AI 页（上方表情 + 下方文字）----------
 * ★ 状态和绘制**分开**，因为 ESP 会一次连发 5 帧（AI 决策 = 2 条舵机 +
 *   1 手势 + 1 表情 + 1 文字）。若每帧都立刻重绘，刷一次表情要 150~300ms，
 *   5 帧就是 1 秒以上，期间新帧继续丢。
 *   ⇒ 帧解析器先调 Set* 把状态攒齐（很快），等线上安静了再调一次 DrawAIPage。 */
void Media_ScanFaces(void);                  /* 扫 SD 的 FACE\ 目录，填册子 */
void Media_SetFace(const char *name);        /* 只设状态，不画 */
void Media_SetCaption(const char *text);     /* 只设状态，不画 */
void Media_DrawAIPage(void);                 /* 按当前状态把 AI 页整页画一遍 */
void Media_AnimationTick(uint8_t visible_page,uint32_t now);
void Media_UpdateAIPage(void);               /* Refresh only changed face/caption on an already visible AI page. */
void Media_ClearFace(void);                  /* 表情框填黑 */
const char *Media_GetFace(void);
const char *Media_GetCaption(void);
#endif
