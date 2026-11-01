#ifndef __MICAUD_H
#define __MICAUD_H

#include "stm32f4xx.h"

/* ==================================================================
 * 20MICAUD —— 音频**流式上行**（F407 → ESP → PC 的 /api/audio/stream）
 * ------------------------------------------------------------------
 * 这是 P3 的核心模块。链路：
 *   麦克风 DMA（64ms 环）→ 本模块的 16KB 环 → `0x02` 二进制帧 → ESP
 *   → 一条流式 HTTP POST（Content-Length 裸 PCM）→ PC 落 wav → 识别
 *
 * ★★ 为什么是"发一块、等一个回执"（窗口 = 1）—— 这是正确性要求：
 *   块 = 2048 B，而 **ESP 的串口 RX 环为 4 KB**，且它读串口和写 socket 是
 *   同一根线程。F407 若不等回执连发两块，第二块到达时环里还压着第一块
 *   ⇒ 溢出、静默丢字节。⇒ **MICAUD_WINDOW 只能 = 1。**
 *
 * ★★ 颗粒度 32ms ⇒ `total` 必须**量化**（会让整段录音永远不被识别的坑）：
 *   PC 侧 `audio_rx.py` 判完整用的是 `received == total`，对不上就
 *   **"收不齐 ⇒ 不送识别"**（不报错、不写日志）。而数据的颗粒度是**半块
 *   32ms = 1024 B**：10 秒 = 312.5 个半块，凑不齐整数。
 *   ⇒ 本模块只发**整块**（2048 B = 2 个半块），`total_blocks` 向下取整：
 *         halfblocks  = seconds * 1000 / 32          （向下取整）
 *         total_blocks = halfblocks / 2              （向下取整）
 *         total_bytes  = total_blocks * 2048
 *     10 秒 → 312 半块 → 156 块 → **319488 B = 9.984 秒**
 *   ⚠️ 写成 `seconds * 32000` = 320000 ⇒ PC 侧**永远判"收不齐"、永远不识别**。
 *
 * ★ 没有断点续传，而且**不可能有**：F407 只有一个 16KB 环，音频推走之后
 *   原数据就不存在了 ⇒ 中断 = 用户重说一遍。别"顺手"加续传。
 *
 * ★ 麦克风**由 main.c 上电自检起好**（INMP441_Init/Start/Calibrate），
 *   本模块只管读 —— 和 19MICREC 的做法一致，别在这儿重复起停。
 * ================================================================== */

/* ---------- 参数 ---------- */
#define MICAUD_CHUNK        2048u   /* 一块 = 1024 个单声道样本 = 64 ms */
#define MICAUD_HALF_BYTES   1024u   /* 半块 = 512 个单声道样本 = 32 ms（颗粒度） */
#define MICAUD_HALF_SAMPLES 512u    /* = 半块的样本数（= inmp441.h 的 MIC_HALF_SAMPLES/2） */
#define MICAUD_RING_BLOCKS  12u     /* 环 = 12 块 = 24 KB = 768 ms 余量；与 ESP
                                     * 端 16 KB 非阻塞发送队列共同吸收 Wi-Fi/TCP 抖动。 */
#define MICAUD_WINDOW       1u      /* ★ 只能 = 1，理由见文件头 */
#define MICAUD_DMA_HALVES   2u      /* 麦克风 DMA 环能装几个半块（= inmp441.h 的 64ms） */

#define MICAUD_RATE         16000u
#define MICAUD_BITS         16u
#define MICAUD_CH           1u

#define MICAUD_SEC_MAX      10u     /* 本轮目标：10 秒跑稳（不是服务器上限 30 秒） */
#define MICAUD_ACK_MS       800u    /* 等一个 `0x04` 的上限 */
#define MICAUD_RETRY_MAX    3u      /* 同一块最多重传几次，再不行就中止会话 */

/* ---------- 帧 type（01 的段 0x01~0x0F）---------- */
#define MICAUD_T_START      0x01u   /* F407→ESP 音频开始：旧版 10B；VAD 短句版多 flags(1) */
#define MICAUD_T_PCM        0x02u   /* F407→ESP PCM 块：**seq = 块号**，payload = 裸 PCM */
#define MICAUD_T_END        0x03u   /* F407→ESP 音频结束（对账用）：sent(2) ok(2) lost_half(2) */
#define MICAUD_T_ACK        0x04u   /* ESP→F407 流控回执：**seq = next_seq**，payload[0] = flags
                                     *   flags 0 = 继续发 next_seq ｜ 1 = 从 next_seq 重传
                                     *   ⚠️ 口径：next_seq 就用**信封自己的 seq 字段**，
                                     *     不再在 payload 里塞一遍（免得两处对不上）。 */

/* ---------- 收/拒的原因（给人看 / 给日志看）---------- */
#define MICAUD_EC_NONE      0u
#define MICAUD_EC_BUSY      1u   /* 已经有一段在跑 */
#define MICAUD_EC_IAP       2u   /* ★ 02 正在升级 —— 互斥闸（iap_app.h:12-17） */
#define MICAUD_EC_ARG       3u   /* 秒数算不出整块 */
#define MICAUD_EC_LINK      4u   /* 回执等不到 / 重传超限 ⇒ 中止 */
#define MICAUD_EC_MIC       5u   /* 麦克风没起来（g_mic_ok == 0） */
#define MICAUD_EC_BACKPRESSURE 6u /* ESP/Wi-Fi 回执慢，发送环已满；非麦克风硬件故障 */

/* START 的可选第 11 字节。置此位后，END 可以声明小于 START 上限的实际块数；
 * 它仍然必须零丢半块、每块都 ACK，不能拿来把异常中断伪装成一句完整录音。 */
#define MICAUD_F_VAD_SHORT  0x01u

/* ---------- 接口 ---------- */
/* START 后等待 ESP 的 seq=0/flags=0 就绪回执，再开始采样。flags=2 表示中止。
 * 开一段流式录音。**只置状态、绝不阻塞**（真正的推进在 MicAud_Tick）。
 * 返回 1 = 已开始；0 = 被拒（原因用 MicAud_ErrName() 看，state 回到 idle）。 */
uint8_t     MicAud_Start(uint16_t seconds, uint8_t flags);

/* 主循环每圈调一次，**绝不阻塞**（除了发一帧那 22ms 的忙等）。
 * 返回 1 = 本圈干了活（发了/重传了/收尾了）。 */
uint8_t     MicAud_Tick(void);

/* VAD 静音断句的正常收尾：停止继续采样，排空已完成的整块，再发合法 END。
 * 这不是 Abort；只有 MICAUD_F_VAD_SHORT 会话允许调用。 */
uint8_t     MicAud_RequestSentenceEnd(void);
uint8_t     MicAud_IsCapturing(void);
uint16_t    MicAud_CapturedBlocks(void);

/* 中止（用户 / 上位机要求，或链接超时兜底）：会尽力把 `0x03` 对账帧发出去。 */
void        MicAud_Abort(uint8_t reason);

uint8_t     MicAud_IsStreaming(void);
void        MicAud_OnAck(uint16_t next_seq, uint8_t flags);   /* 由 ailink 的分发调用 */

/* 只算不置状态：秒数 → 块数（= 向下取整到半块再取整块，见文件头）。
 * 返回 0 表示这个秒数连一整块都凑不出来。 */
uint16_t    MicAud_TotalBlocks(uint16_t seconds);
uint32_t    MicAud_TotalBytes(uint16_t seconds);

/* ---------- 观察量（状态页 / `[MICAUD]` 那行日志 / 验收判据用）---------- */
uint16_t    MicAud_SentBlocks(void);    /* 已被回执确认的块数 */
uint16_t    MicAud_LostHalf(void);      /* ★ 丢的**半块**数（验收判据：必须是 0） */
uint16_t    MicAud_Retries(void);       /* 重传次数（>0 是修复成功，不是故障） */
uint32_t    MicAud_Frames(void);        /* 一共发出去多少帧（含重传） */
const char *MicAud_StateName(void);
const char *MicAud_ErrName(void);
void        MicAud_Diag(void);          /* 打一行摘要（主循环每 5 秒调一次） */

#endif /* __MICAUD_H */
