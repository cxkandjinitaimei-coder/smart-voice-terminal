/**
 * @file ailink.h
 * 用途：ESP 下行命令入口，协调舵机、表情、时间线、待机动作及升级回执。
 * 使用：main 初始化后反复调用 AILink_Poll/Tick；文本按白名单分派，二进制帧按类型路由。
 * 时序与边界：解析只武装耗时任务；主循环推进动作。参数修订号和板端回执用于同步，姿态是估计值。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __AILINK_H
#define __AILINK_H

#include "stm32f4xx.h"
#include "media.h" /* 要 PAGE_AI / PAGE_KEEP */

/* 一次连发的收尾判定：这条线上安静这么久就算这一批帧到齐了，可以重绘了。
 * 50ms 的来历：115200 下一次 5 帧连发约 150 字节，上线只要 13ms，
 * 50ms 留了约 4 倍余量；而人眼对 50ms 的延迟基本无感。 */
#define AILINK_SETTLE_MS 50u

/* Maximum complete frame including NUL. Longer input is rejected, never truncated. */
#define AILINK_LINE_MAX 192u

void AILink_Init(void);
/* Physical key/debug/voice activity cancels autonomous idle tasks. */
void AILink_UserActivity(void);

/* 主循环每圈调一次。返回**要切到的页**（PAGE_KEEP = 不用切）：
 * 静默窗到点时返回 PAGE_AI，由 main.c 统一重绘一次。
 *  它自己**不画屏** —— 自己画一遍再让 main.c 画一遍 = 白刷一次屏
 *   （150~300ms），见 ailink.c 里 AILink_Poll 的注释。 */
uint8_t AILink_Poll(void);

/* Pending interpolated PWM targets (not measured shaft position). */
uint8_t AILink_ServoBusy(void);
/* OTA screen has priority even while the request is waiting for K-UP. */
uint8_t AILink_OtaPending(void);
uint8_t AILink_TimelineNeedsPaint(void);
void AILink_TimelinePainted(void);
/* Foreground BMP row hook: continue smooth PWM without recursively parsing UART. */
void AILink_ServiceMotion(void);
typedef struct
{
    uint8_t minimum, maximum, front, configured;
    uint16_t target, speed;
} AILink_ServoInfo;
/* Current command speed is distinct from the web's per-axis defaults. */
uint8_t AILink_GetServoInfo(uint8_t id, AILink_ServoInfo *info);
/* Last acknowledged/interpolated PWM command estimate, not shaft feedback. */
uint8_t AILink_GetScreenAngle(uint16_t *degrees);

/* 解析一行 ESP 下行帧。返回 1 = 已消费（含"认出来但参数越界被拒不执行"），
 * 0 = 不认识/格式错，已计入 ailink_drop_cnt。 */
/* Valid ESP heartbeat within 8 s. This indicates UART link, not cloud reachability. */
uint8_t AILink_LinkAlive(uint32_t now);
uint8_t handle_esp_line(const char *line);

/* Command counters; heartbeat does not increment ok. RX diagnostics also report
 * byte/overflow/UART-error counts every 5 seconds on the debug port. */
extern volatile uint16_t ailink_ok_cnt;
extern volatile uint16_t ailink_drop_cnt;

#endif /* __AILINK_H */
