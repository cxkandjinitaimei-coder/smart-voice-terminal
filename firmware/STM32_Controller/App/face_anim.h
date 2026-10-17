/**
 * @file face_anim.h
 * 用途：从 SD 卡读取 RGB565 序列帧，在表情区域播放动画。
 * 使用：由媒体层选择表情目录，Tick 按毫秒时基推进；显示方向由屏幕状态决定。
 * 时序与边界：不把整套动画放进片内 Flash；文件读取和 LCD 写入占前台时间，录音期按调度条件暂停。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef FACE_ANIM_H
#define FACE_ANIM_H
#include <stdint.h>

typedef struct
{
    const char *directory;
    uint16_t frames, frame, rows, interval_ms;
    uint32_t completed;
    uint8_t active, error;
    uint16_t measured_fps_x10; /* Last >=1 s sample; zero means not sampled. */
    uint8_t rgb565;
} FaceAnim_Info;
/* Prefer aligned SD RGB565 (R16); fall back to BMP, never buffer a full frame. */
uint8_t FaceAnim_Start(const char *name, uint16_t x, uint16_t y, uint32_t now);
void FaceAnim_Stop(void);
/* allow=0 performs no SD operation and no LCD draw, including file close. */
void FaceAnim_Tick(uint32_t now, uint8_t allow);
void FaceAnim_GetInfo(FaceAnim_Info *info);
uint8_t FaceAnim_SetFPS(uint8_t fps);
uint8_t FaceAnim_SetEnabled(uint8_t enabled);
uint8_t FaceAnim_IsEnabled(void);
#endif
