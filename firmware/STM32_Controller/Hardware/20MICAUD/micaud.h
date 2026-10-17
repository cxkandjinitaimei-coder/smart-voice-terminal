/**
 * @file micaud.h
 * 用途：把麦克风 DMA 半块拼成 2048 字节块，经串口带 ACK 发送给 ESP。
 * 使用：Start 发 START 等 READY；Tick 复制半块/发 DATA，OnAck 推进；断句或时长到达后发 END。
 * 时序与边界：环形队列保存尚未 ACK 的原块，确认后才释放；超时有限重传，溢出显式报失败，不伪造完整音频。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __MICAUD_H
#define __MICAUD_H

#include "stm32f4xx.h"

/* 音频：16kHz/16bit/mono，32ms 半块，64ms 数据块。
 * 发送环 12x2048 字节，窗口 1；10 秒量化为 156 块/319488 字节。
 * ACK 表示 ESP 接受该块，不等于云端完成识别；断线后的网络重放由 ESP 缓冲策略处理。 */

/* ---------- 参数 ---------- */
#define MICAUD_CHUNK 2048u       /* 一块 = 1024 个单声道样本 = 64 ms */
#define MICAUD_HALF_BYTES 1024u  /* 半块 = 512 个单声道样本 = 32 ms（颗粒度） */
#define MICAUD_HALF_SAMPLES 512u /* = 半块的样本数（= inmp441.h 的 MIC_HALF_SAMPLES/2） */
#define MICAUD_RING_BLOCKS                                                                         \
    12u                      /* 环 = 12 块 = 24 KB = 768 ms 余量；与 ESP
                                     * 端 16 KB 非阻塞发送队列共同吸收 Wi-Fi/TCP 抖动。 */
#define MICAUD_WINDOW 1u     /*  只能 = 1，理由见文件头 */
#define MICAUD_DMA_HALVES 2u /* 麦克风 DMA 环能装几个半块（= inmp441.h 的 64ms） */

#define MICAUD_RATE 16000u
#define MICAUD_BITS 16u
#define MICAUD_CH 1u

#define MICAUD_SEC_MAX 10u  /* 默认上限：10 秒跑稳（不是服务器上限 30 秒） */
#define MICAUD_ACK_MS 800u  /* 等一个 `0x04` 的上限 */
#define MICAUD_RETRY_MAX 3u /* 同一块最多重传几次，再不行就中止会话 */

/* ---------- 帧 type（音频类型段 0x01~0x0F）---------- */
#define MICAUD_T_START 0x01u /* F407→ESP 音频开始：旧版 10B；VAD 短句版多 flags(1) */
#define MICAUD_T_PCM 0x02u   /* F407→ESP PCM 块：**seq = 块号**，payload = 裸 PCM */
#define MICAUD_T_END 0x03u /* F407→ESP 音频结束（对账用）：sent(2) ok(2) lost_half(2) */
#define MICAUD_T_ACK                                                                               \
    0x04u /* ESP→F407 流控回执：**seq = next_seq**，payload[0] = flags
                                     *   flags 0 = 继续发 next_seq ｜ 1 = 从 next_seq 重传
                                     *    口径：next_seq 就用**信封自己的 seq 字段**，
                                     *     不再在 payload 里塞一遍（免得两处对不上）。 */

/* ---------- 收/拒的原因（给人看 / 给日志看）---------- */
#define MICAUD_EC_NONE 0u
#define MICAUD_EC_BUSY 1u         /* 已经有一段在跑 */
#define MICAUD_EC_IAP 2u          /*  02 正在升级 —— 互斥闸（iap_app.h:12-17） */
#define MICAUD_EC_ARG 3u          /* 秒数算不出整块 */
#define MICAUD_EC_LINK 4u         /* 回执等不到 / 重传超限 ⇒ 中止 */
#define MICAUD_EC_MIC 5u          /* 麦克风没起来（g_mic_ok == 0） */
#define MICAUD_EC_BACKPRESSURE 6u /* ESP/Wi-Fi 回执慢，发送环已满；非麦克风硬件故障 */

/* START 的可选第 11 字节。置此位后，END 可以声明小于 START 上限的实际块数；
 * 它仍然必须零丢半块、每块都 ACK，不能拿来把异常中断伪装成一句完整录音。 */
#define MICAUD_F_VAD_SHORT 0x01u

/* ---------- 接口 ---------- */
/* START 后等待 ESP 的 seq=0/flags=0 就绪回执，再开始采样。flags=2 表示中止。
 * 开一段流式录音。**只置状态、绝不阻塞**（真正的推进在 MicAud_Tick）。
 * 返回 1 = 已开始；0 = 被拒（原因用 MicAud_ErrName() 看，state 回到 idle）。 */
uint8_t MicAud_Start(uint16_t seconds, uint8_t flags);

/* 主循环每圈调一次，**绝不阻塞**（除了发一帧那 22ms 的忙等）。
 * 返回 1 = 本圈干了活（发了/重传了/收尾了）。 */
uint8_t MicAud_Tick(void);

/* VAD 静音断句的正常收尾：停止继续采样，排空已完成的整块，再发合法 END。
 * 这不是 Abort；只有 MICAUD_F_VAD_SHORT 会话允许调用。 */
uint8_t MicAud_RequestSentenceEnd(void);
uint8_t MicAud_IsCapturing(void);
uint16_t MicAud_CapturedBlocks(void);

/* 中止（用户 / 上位机要求，或链接超时兜底）：会尽力把 `0x03` 对账帧发出去。 */
void MicAud_Abort(uint8_t reason);

uint8_t MicAud_IsStreaming(void);
void MicAud_OnAck(uint16_t next_seq, uint8_t flags); /* 由 ailink 的分发调用 */

/* 只算不置状态：秒数 → 块数（= 向下取整到半块再取整块，见文件头）。
 * 返回 0 表示这个秒数连一整块都凑不出来。 */
uint16_t MicAud_TotalBlocks(uint16_t seconds);
uint32_t MicAud_TotalBytes(uint16_t seconds);

/* ---------- 观察量（状态页 / `[MICAUD]` 那行日志 / 验收判据用）---------- */
uint16_t MicAud_SentBlocks(void); /* 已被回执确认的块数 */
uint16_t MicAud_LostHalf(void);   /*  丢的**半块**数（验收判据：必须是 0） */
uint16_t MicAud_Retries(void);    /* 重传次数（>0 是修复成功，不是故障） */
uint32_t MicAud_Frames(void);     /* 一共发出去多少帧（含重传） */
const char *MicAud_StateName(void);
const char *MicAud_ErrName(void);
void MicAud_Diag(void); /* 打一行摘要（主循环每 5 秒调一次） */

#endif /* __MICAUD_H */
