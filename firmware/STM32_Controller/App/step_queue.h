#ifndef STEP_QUEUE_H
#define STEP_QUEUE_H
#include <stdint.h>

/* F407 本地三轴步骤队列：ESP 只负责把整段录制转发进来，时间轴由 F407 自己跑。
 * 302 步 × 8 字节 = 2416B，放在完全空闲的 CCM（0x10000000，64KB）。
 * 主机测试没有 CCM，自动换成普通数组。
 * 队列不碰 I2C/中断；AILink 每圈取一个到期步，再调 ServoMotion_Move。 */
#define STEPQ_MAX 302u

typedef struct {
    uint16_t at;     /* 相对 START 的毫秒，0~30000 */
    uint8_t  a[3];   /* S1/S2/S3 绝对角 */
    uint16_t s[3];   /* 三轴速度 deg/s，1~360；不能压成 uint8 */
} StepQItem;

#ifdef __arm__
#define STEPQ_ITEMS ((StepQItem *)0x10000000u)
#else
static StepQItem stepq_host_items[STEPQ_MAX];
#define STEPQ_ITEMS stepq_host_items
#endif

typedef struct {
    uint16_t total, accepted, pos;
    uint32_t start_ms, paused_at;
    uint8_t  active, paused, done;
} StepQState;

static StepQState stepq;

static void StepQ_Reset(uint16_t total)
{
    stepq.total = total > STEPQ_MAX ? (uint16_t)STEPQ_MAX : total;
    stepq.accepted = stepq.pos = 0u;
    stepq.active = stepq.paused = stepq.done = 0u;
    stepq.start_ms = stepq.paused_at = 0u;
}

/* idx 必须严格等于 accepted：丢一行就整批不认，ESP 会按 accepted 重发。 */
static uint8_t StepQ_Append(uint16_t idx, uint16_t at, const uint8_t *a, const uint16_t *s)
{
    uint8_t i;
    if (idx != stepq.accepted || stepq.accepted >= stepq.total || stepq.active) return 0u;
    if (at > 30000u) return 0u;
    if (stepq.accepted && at < STEPQ_ITEMS[stepq.accepted - 1u].at) return 0u;
    for (i = 0u; i < 3u; i++) {
        if (a[i] > 180u || s[i] == 0u || s[i] > 360u) return 0u;
    }
    STEPQ_ITEMS[stepq.accepted].at = at;
    for (i = 0u; i < 3u; i++) {
        STEPQ_ITEMS[stepq.accepted].a[i] = a[i];
        STEPQ_ITEMS[stepq.accepted].s[i] = s[i];
    }
    stepq.accepted++;
    return 1u;
}

/* 只有全部步骤都收齐才允许 START，避免边播边缺步。 */
static uint8_t StepQ_Start(uint32_t now)
{
    if (!stepq.total || stepq.accepted != stepq.total) return 0u;
    stepq.active = 1u;
    stepq.paused = 0u;
    stepq.done = 0u;
    stepq.pos = 0u;
    stepq.start_ms = now;
    stepq.paused_at = now;
    return 1u;
}

static void StepQ_Stop(void)
{
    stepq.active = 0u;
    stepq.paused = 0u;
}

/* 返回 1=取出一步（调用者发三轴 MOVE），0=还没到点/暂停，2=整段完成。 */
static uint8_t StepQ_Next(uint32_t now, uint8_t pause, StepQItem *out)
{
    if (!stepq.active) return 0u;
    if (pause) {
        if (!stepq.paused) {
            stepq.paused = 1u;
            stepq.paused_at = now;
        }
        return 0u;
    }
    if (stepq.paused) {
        /* 暂停的时长不计入时间轴；恢复后从原来的剩余时间继续。 */
        stepq.start_ms += now - stepq.paused_at;
        stepq.paused = 0u;
    }
    if (stepq.pos >= stepq.total) {
        stepq.active = 0u;
        stepq.done = 1u;
        return 2u;
    }
    if ((uint32_t)(now - stepq.start_ms) < (uint32_t)STEPQ_ITEMS[stepq.pos].at) return 0u;
    *out = STEPQ_ITEMS[stepq.pos++];
    return 1u;
}

static uint8_t  StepQ_Busy(void)     { return stepq.active; }
static uint16_t StepQ_Pos(void)      { return stepq.pos; }
static uint16_t StepQ_Total(void)    { return stepq.total; }
static uint16_t StepQ_Accepted(void) { return stepq.accepted; }

#endif
