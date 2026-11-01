/**
 * @file    micaud.c
 * @brief   20MICAUD —— 音频流式上行（麦克风 → 16KB 环 → 0x02 帧 → ESP → 流式 POST）
 *
 * 【一圈里干的三件事，顺序不能变】
 *   ① 生产：`INMP441_Blocks()` 累计计数增加 1 ⇒ `INMP441_Read()` 搬进环。
 *      ★ **每圈都要做**，包括"正在等回执"的时候 —— 麦克风 DMA 环只有 64ms，
 *        等回执期间不读它就真的丢了。
 *   ② 发送：窗口空着（没有未回执的块）且环里有整块 ⇒ 发一块，然后**立刻返回**。
 *   ③ 超时：等回执超了 `MICAUD_ACK_MS` ⇒ 重传；重传也超限 ⇒ 中止会话（如实计数）。
 *
 * 【★ 环满怎么办】
 *   环满 = 发送侧被卡住超过 512ms（正常不会：窗口=1 时每 64ms 就该走一块）。
 *   此时**扔新来的**（计丢半块并中止，不补入后续声音）而不是扔排队中的：已排队的那几块保持
 *   连续、seq 单调，出问题时"丢了多少"这个数才是准的。
 *   ⚠️ 但**麦克风 DMA 环必须照排空**（不读它它自己也会溢出），所以用一个
 *   `drop_buf` 当垃圾桶 —— 别为了省 1KB 把 DMA 环堵死。
 *
 * 【为什么等回执的是"整块"而不是"半块"】
 *   半块 1024 B 太小：921600 下占线 11ms，往返开销摊不开；整块 2048 B 占线
 *   22ms，而 PC 侧实测按 32000 B/s 的节拍推 10 秒是跟得上的（`测试P3推流.bat`）。
 */

#include "micaud.h"
#include "esp_uart.h"      /* ESP_UART_SendFrame / SendStr */
#include "inmp441.h"       /* INMP441_Read / INMP441_Blocks */
#include "iap_app.h"       /* ★ IAP_IsBusy() —— 互斥闸（iap_app.h:12-17）*/
#include "../../System/time.h" /* GetTick_ms, not ARMCC's standard time.h */
#include <stdio.h>
#include <string.h>

/* ---------- 状态 ---------- */
#define MICAUD_ST_IDLE      0u
#define MICAUD_ST_WAIT      2u   /* wait for ESP TCP readiness; do not capture yet */
#define MICAUD_ST_STREAM    1u   /* 正在发 */

static uint8_t  s_st = MICAUD_ST_IDLE;
static uint8_t  s_reason = MICAUD_EC_NONE;   /* 最近一次收/拒的原因 */
static uint8_t  s_flags;                     /* START 的可选特性位 */

/* ---------- 环（生产者 = 本文件 ①，消费者 = 本文件 ②）---------- */
static uint8_t  s_ring[MICAUD_RING_BLOCKS][MICAUD_CHUNK];
static int16_t  s_drop_buf[MICAUD_HALF_SAMPLES];   /* 环满时的垃圾桶（见文件头） */
static uint16_t s_head, s_tail;      /* 块索引（0..RING-1 循环） */
static uint16_t s_used;              /* 环里有几块整块 */
static uint16_t s_fill;              /* 当前正在填的那块已填的**字节**数（0/1024） */

/* ---------- 会话 ---------- */
static uint16_t s_total_blocks;      /* START 声明的上限块数 */
static uint16_t s_final_blocks;      /* 本段实际要对账的块数（VAD 可提前结束） */
static uint16_t s_ok_blocks;         /* 已被回执确认的块数（= 下一个要发的 seq） */
static uint16_t s_tx_slot;           /* 正在等回执的那块在环里的槽（peek，不 pop） */
static uint8_t  s_inflight;          /* 有 1 块已发出、在等 `0x04` */
static uint8_t  s_retries;           /* 当前这块已经重传几次 */
static uint32_t s_ack_ms;            /* 等回执的截止时刻 */
static uint32_t s_last_block, s_mic_ms;
static uint16_t s_captured_halves;
static uint8_t s_start_payload[11];
static uint8_t s_start_len;
static uint8_t s_sentence_end;       /* 1 = VAD 已请求正常提前收尾 */
static uint16_t s_sess_seq;          /* `0x01` 的 seq = 会话序号（观察用，每段 +1）*/

/* ---------- 计数（验收判据）---------- */
static uint16_t s_lost_half;         /* ★ 丢的半块数：必须是 0 */
static uint16_t s_retry_cnt;
static uint32_t s_frames;            /* 发出去的帧总数（含重传）*/

/* ================================================================== */
uint16_t MicAud_TotalBlocks(uint16_t seconds)
{
    uint32_t half;

    if (seconds == 0u)
        return 0u;

    half = ((uint32_t)seconds * 1000u) / 32u;    /* 半块数（颗粒度 32ms，向下取整）*/
    return (uint16_t)(half / 2u);                /* 整块数（一块 = 2 个半块）*/
}

uint32_t MicAud_TotalBytes(uint16_t seconds)
{
    return (uint32_t)MicAud_TotalBlocks(seconds) * (uint32_t)MICAUD_CHUNK;
}

uint8_t  MicAud_IsStreaming(void)   { return (s_st != MICAUD_ST_IDLE) ? 1u : 0u; }
uint16_t MicAud_SentBlocks(void)    { return s_ok_blocks; }
uint16_t MicAud_LostHalf(void)      { return s_lost_half; }
uint16_t MicAud_Retries(void)       { return s_retry_cnt; }
uint32_t MicAud_Frames(void)        { return s_frames; }
uint8_t MicAud_IsCapturing(void)    { return (s_st == MICAUD_ST_STREAM && s_sentence_end == 0u) ? 1u : 0u; }
uint16_t MicAud_CapturedBlocks(void){ return (uint16_t)(s_captured_halves / 2u); }

const char *MicAud_StateName(void)
{
    return (s_st == MICAUD_ST_WAIT) ? "WAIT_ESP" : ((s_st == MICAUD_ST_STREAM) ? "STREAM" : "IDLE");
}

const char *MicAud_ErrName(void)
{
    switch (s_reason)
    {
    case MICAUD_EC_BUSY: return "已经有一段在跑";
    case MICAUD_EC_IAP:  return "IAP 正在升级（互斥）";
    case MICAUD_EC_ARG:  return "秒数凑不出整块";
    case MICAUD_EC_LINK: return "回执等不到（链路）";
    case MICAUD_EC_MIC:  return "麦克风没起来";
    case MICAUD_EC_BACKPRESSURE: return "ESP/Wi-Fi 背压使发送环满";
    default:             return "没有错误";
    }
}

/* 发一块（含重传）。seq = 块号 = s_ok_blocks（窗口=1，所以就是"下一个待确认的"）。*/
static void send_block(void)
{
    ESP_UART_SendFrame(MICAUD_T_PCM, s_ok_blocks, s_ring[s_tx_slot], MICAUD_CHUNK);
    s_frames++;
    s_ack_ms = GetTick_ms() + MICAUD_ACK_MS;
}

/* 收尾：把 `0x03` 对账帧发出去，状态回 idle。reason 只影响日志。 */
static void finish(uint8_t reason, uint8_t send_end)
{
    if (send_end != 0u)
    {
        uint8_t pay[6];                        /* sent(2) ok(2) lost_half(2) */
        /* 对 VAD 短句来说，此处是实际完整块数，不是 START 的 10 秒上限。 */
        pay[0] = (uint8_t)(s_final_blocks & 0xFFu);
        pay[1] = (uint8_t)(s_final_blocks >> 8);
        pay[2] = (uint8_t)(s_ok_blocks & 0xFFu);
        pay[3] = (uint8_t)(s_ok_blocks >> 8);
        pay[4] = (uint8_t)(s_lost_half & 0xFFu);
        pay[5] = (uint8_t)(s_lost_half >> 8);
        ESP_UART_SendFrame(MICAUD_T_END, s_sess_seq, pay, 6u);
        s_frames++;
    }

    /* ★ 先落 reason 再打日志：反过来的话打出来的是**上一次**的原因（踩过）。 */
    s_reason = reason;
    s_st = MICAUD_ST_IDLE;
    s_inflight = 0u;

    printf("[MICAUD] 收尾：发出 %u/%u 块 重传 %u 丢半块 %u 原因=%s\r\n",
           (unsigned)s_ok_blocks, (unsigned)s_final_blocks,
           (unsigned)s_retry_cnt, (unsigned)s_lost_half, MicAud_ErrName());
}

uint8_t MicAud_Start(uint16_t seconds, uint8_t flags)
{
    uint8_t pay[11];
    uint32_t total;

    if (s_st != MICAUD_ST_IDLE)
    {
        s_reason = MICAUD_EC_BUSY;
        return 0u;
    }
    /* ★ 互斥闸：02 升级期间**拒**、不排队（擦写会把 CPU 停 1~4 秒，
     *   麦克风 64ms 的 DMA 环必溢出）。口径见 iap_app.h:12-17。 */
    if (IAP_IsBusy())
    {
        s_reason = MICAUD_EC_IAP;
        return 0u;
    }
    if (seconds > MICAUD_SEC_MAX)
        seconds = MICAUD_SEC_MAX;

    s_total_blocks = MicAud_TotalBlocks(seconds);
    if (s_total_blocks == 0u)
    {
        s_reason = MICAUD_EC_ARG;
        return 0u;
    }
    total = (uint32_t)s_total_blocks * (uint32_t)MICAUD_CHUNK;   /* = 实际会发的字节数 */

    /* 清账 */
    s_head = s_tail = 0u;
    s_used = 0u;
    s_fill = 0u;
    s_ok_blocks = 0u;
    s_inflight = 0u;
    s_retries = 0u;
    s_lost_half = 0u;
    s_retry_cnt = 0u;
    s_frames = 0u;
    s_reason = MICAUD_EC_NONE;
    s_flags = flags;
    s_sentence_end = 0u;
    s_final_blocks = s_total_blocks;
    s_st = MICAUD_ST_WAIT;
    s_captured_halves = 0u;
    s_last_block = INMP441_Blocks();

    /* `0x01` 音频开始：seconds(2) total(4) rate(2) bits(1) ch(1) —— 小端，共 10 字节。
     * ★ total 必须是**实际会发出去的字节数**（量化之后的），理由见 micaud.h。 */
    pay[0] = (uint8_t)(seconds & 0xFFu);
    pay[1] = (uint8_t)(seconds >> 8);
    pay[2] = (uint8_t)(total & 0xFFu);
    pay[3] = (uint8_t)((total >> 8) & 0xFFu);
    pay[4] = (uint8_t)((total >> 16) & 0xFFu);
    pay[5] = (uint8_t)((total >> 24) & 0xFFu);
    pay[6] = (uint8_t)(MICAUD_RATE & 0xFFu);
    pay[7] = (uint8_t)(MICAUD_RATE >> 8);
    pay[8] = (uint8_t)MICAUD_BITS;
    pay[9] = (uint8_t)MICAUD_CH;
    s_start_len = 10u;
    if ((flags & MICAUD_F_VAD_SHORT) != 0u)
    {
        pay[10] = flags;
        s_start_len = 11u;
    }

    s_sess_seq++;
    memcpy(s_start_payload, pay, s_start_len);
    ESP_UART_SendFrame(MICAUD_T_START, s_sess_seq, pay, s_start_len);
    s_ack_ms = GetTick_ms() + 1500u;
    s_frames++;

    printf("[MICAUD] 开始流式：%u 秒 -> %u 块 x %u B = %lu B"
           "（%.3f 秒音频，量化到 32ms 颗粒度；flags=%u）\r\n",
           (unsigned)seconds, (unsigned)s_total_blocks, (unsigned)MICAUD_CHUNK,
           (unsigned long)total, (double)total / 32000.0, (unsigned)s_flags);
    return 1u;
}

void MicAud_Abort(uint8_t reason)
{
    if (s_st == MICAUD_ST_IDLE)
        return;
    finish(reason, 1u);
}

uint8_t MicAud_RequestSentenceEnd(void)
{
    if ((s_st != MICAUD_ST_STREAM) || ((s_flags & MICAUD_F_VAD_SHORT) == 0u) ||
        (s_sentence_end != 0u))
        return 0u;

    /* 仅能对账完整 2048B 块。正在填的一半自然留在本地，不把它伪报为 DMA 丢失。 */
    s_final_blocks = (uint16_t)(s_captured_halves / 2u);
    if (s_final_blocks == 0u)
        return 0u;
    s_sentence_end = 1u;
    printf("[MICAUD] VAD 静音断句：收尾 %u 块（START 上限 %u 块）\r\n",
           (unsigned)s_final_blocks, (unsigned)s_total_blocks);
    return 1u;
}

void MicAud_OnAck(uint16_t next_seq, uint8_t flags)
{
    if (s_st == MICAUD_ST_IDLE) return;
    if (flags == 2u)
    {
        printf("[MICAUD] ESP 拒绝 START/数据：flags=2 next=%u\r\n", (unsigned)next_seq);
        finish(MICAUD_EC_LINK, 1u);
        return;
    }
    if (s_st == MICAUD_ST_WAIT)
    {
        if (flags == 0u && next_seq == 0u)
        {
            printf("[MICAUD] READY: mode=%s\r\n",
                   (s_flags & MICAUD_F_VAD_SHORT) ? "AUTO (silence ends sentence, max 10s)" : "MANUAL (fixed duration, includes silence)");
            s_last_block = INMP441_Blocks();
            s_mic_ms = GetTick_ms();
            s_st = MICAUD_ST_STREAM;
            s_retries = 0u;
        }
        else
        {
            printf("[MICAUD] 忽略非 READY 回执：next=%u flags=%u\r\n",
                   (unsigned)next_seq, (unsigned)flags);
        }
        return;
    }
    if (s_inflight == 0u)
        return;

    if (flags > 1u) return;
    if (flags == 1u && next_seq != s_ok_blocks) return;
    if (flags == 1u)                             /* ESP 要求重传 */
    {
        if (s_retries >= MICAUD_RETRY_MAX)
        {
            s_lost_half += (uint16_t)(MICAUD_CHUNK / MICAUD_HALF_BYTES);
            finish(MICAUD_EC_LINK, 1u);
            return;
        }
        s_retries++;
        s_retry_cnt++;
        printf("[MICAUD] 重传块 %u（第 %u 次）\r\n", (unsigned)s_ok_blocks, (unsigned)s_retries);
        send_block();
        return;
    }

    if (next_seq != (uint16_t)(s_ok_blocks + 1u))
        return;                                  /* 对不上：等超时兜底，别乱推进 */

    /* 正常推进：**确认之后才 pop**（这样重传还能从环里原样再发，不用第二块缓冲）*/
    s_ok_blocks++;
    s_tail = (uint16_t)((s_tail + 1u) % MICAUD_RING_BLOCKS);
    s_used--;
    s_inflight = 0u;
    s_retries = 0u;

    if (s_ok_blocks >= s_final_blocks)
        finish(MICAUD_EC_NONE, 1u);
}

uint8_t MicAud_Tick(void)
{
    uint32_t now;
    uint32_t avail;
    uint16_t got;

    if (s_st == MICAUD_ST_IDLE) return 0u;
    now = GetTick_ms();
    if (s_st == MICAUD_ST_WAIT)
    {
        if ((int32_t)(now - s_ack_ms) >= 0)
        {
            if (s_retries++ >= MICAUD_RETRY_MAX)
            { finish(MICAUD_EC_LINK, 1u); return 1u; }
            printf("[MICAUD] 等 READY 超时，重发 START（%u/%u）\r\n",
                   (unsigned)s_retries, (unsigned)MICAUD_RETRY_MAX);
            ESP_UART_SendFrame(MICAUD_T_START, s_sess_seq, s_start_payload, s_start_len);
            s_frames++;
            s_ack_ms = GetTick_ms() + 1500u;
        }
        return 1u;
    }

    /* A VAD boundary can fall on a half block AFTER the last whole-block
     * ACK. No more PCM will be sent then, so no future ACK can finish us. */
    if (s_sentence_end != 0u && s_inflight == 0u && s_ok_blocks >= s_final_blocks)
    {
        finish(MICAUD_EC_NONE, 1u);
        return 1u;
    }

    /* Blocks() is a monotonically increasing DMA completion counter, NOT
     * a queue depth. Read() copies the latest completed half and consumes
     * nothing. Only that half is stable: DMA is already rewriting the other. */
    avail = INMP441_Blocks();
    if (s_sentence_end == 0u && s_captured_halves < (uint16_t)(s_total_blocks * 2u) && avail != s_last_block)
    {
        uint32_t delta = avail - s_last_block;
        uint8_t *dst = &s_ring[s_head][s_fill];
        uint8_t full = (s_fill == 0u && s_used >= MICAUD_RING_BLOCKS);
        if (delta > 1u)
            s_lost_half += (uint16_t)((delta - 1u > 65535u) ? 65535u : delta - 1u);
        s_last_block = avail;
        s_mic_ms = now;
        if (full) dst = (uint8_t *)(void *)s_drop_buf;
        got = INMP441_Read((int16_t *)(void *)dst, MICAUD_HALF_SAMPLES);
        if (full || got != MICAUD_HALF_SAMPLES || INMP441_Blocks() != avail)
            s_lost_half++;
        else
        {
            s_captured_halves++;
            s_fill += MICAUD_HALF_BYTES;
            if (s_fill == MICAUD_CHUNK)
            {
                s_fill = 0u;
                s_head = (uint16_t)((s_head + 1u) % MICAUD_RING_BLOCKS);
                s_used++;
            }
        }
        /* Do not manufacture a full-length recording by silently replacing
         * lost samples with later audio. The ESP leaves a partial WAV. */
        if (s_lost_half)
        {
            /* `full` 是网络回执慢、发送队列堆满；DMA计数停住才是麦克风。
             * 两者必须分开报告，不能把 Wi-Fi 抖动误导成接线故障。 */
            finish(full ? MICAUD_EC_BACKPRESSURE : MICAUD_EC_MIC, 1u);
            return 1u;
        }
    }
    if (s_sentence_end == 0u && s_captured_halves < (uint16_t)(s_total_blocks * 2u) &&
        (uint32_t)(now - s_mic_ms) > 1000u)
    { finish(MICAUD_EC_MIC, 1u); return 1u; }

    /* ---------- ③ 超时（先判，因为它决定这圈还能不能发新块）---------- */
    now = GetTick_ms();
    if (s_inflight != 0u)
    {
        if ((int32_t)(now - s_ack_ms) >= 0)
        {
            if (s_retries >= MICAUD_RETRY_MAX)
            {
                printf("[MICAUD] 块 %u 等回执超时 %u 次，中止会话\r\n",
                       (unsigned)s_ok_blocks, (unsigned)s_retries);
                finish(MICAUD_EC_LINK, 1u);
                return 1u;
            }
            s_retries++;
            s_retry_cnt++;
            printf("[MICAUD] 块 %u 等回执超时，重传（第 %u 次）\r\n",
                   (unsigned)s_ok_blocks, (unsigned)s_retries);
            send_block();
        }
        return 1u;                                 /* 窗口里还有一块，这圈不再发新块 */
    }

    /* ---------- ② 发送：窗口空着、环里有整块才发 ---------- */
    if (s_used == 0u)
        return 1u;                                 /* 还没攒够一块 */

    s_tx_slot  = s_tail;
    s_inflight = 1u;
    s_retries  = 0u;
    send_block();                                  /* 一帧 2048 B 占线约 22ms */
    return 1u;
}

void MicAud_Diag(void)
{
    printf("[MICAUD] %s 块 %u/%u 环 %u/%u 重传 %u 丢半块 %u 帧 %lu 原因=%s\r\n",
            MicAud_StateName(), (unsigned)s_ok_blocks, (unsigned)s_final_blocks,
           (unsigned)s_used, (unsigned)MICAUD_RING_BLOCKS,
           (unsigned)s_retry_cnt, (unsigned)s_lost_half,
           (unsigned long)s_frames, MicAud_ErrName());
}
