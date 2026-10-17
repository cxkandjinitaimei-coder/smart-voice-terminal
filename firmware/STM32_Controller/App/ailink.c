/**
 * @file ailink.c
 * 用途：ESP 下行命令入口，协调舵机、表情、时间线、待机动作及升级回执。
 * 使用：main 初始化后反复调用 AILink_Poll/Tick；文本按白名单分派，二进制帧按类型路由。
 * 时序与边界：解析只武装耗时任务；主循环推进动作。参数修订号和板端回执用于同步，姿态是估计值。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "ailink.h"
#include "camera_link.h"
#include "esp_uart.h"
#include "pca9685.h"
#include "servo_motion.h"
#include "motion_timeline.h"
#include "idle_pet.h"
#include "media.h"
#include "led_pwm.h"
#include "time.h" /* GetTick_ms —— 静默窗和步进序列都靠它 */

#include "miclink.h" /* @MIC: 帧 -> MicLink_RequestStart/Stop（只记请求，见下） */
#include "font_upload.h"
#include "micaud.h" /* 0x04 流控回执 -> MicAud_OnAck；流式状态 -> 让路/互斥 */
/* ===  === */
/* === 交界：二进制帧 + @IAP: 文本帧的分发就在本文件（见 handle_esp_frame）
 *   需要一个入口和两个查询： */
#include "iap_app.h" /* IAP_AppCommand / IAP_OnFrame / IAP_IsBusy / IAP_StateName */
#include <stdio.h>
#include <stdlib.h> /* strtoul —— OTA 进度要从 `@IAP:REQUEST,<total>,<crc>` 取总长 */
#include <string.h>

/* ==================================================================
 * 对外计数（状态页/排查用）
 * ================================================================== */
volatile uint16_t ailink_ok_cnt = 0;
volatile uint16_t ailink_drop_cnt = 0;

/* ==================================================================
 * 静默窗
 * ================================================================== */
static uint32_t ai_due_ms;   /* 0 = 没有待办的重绘 */
static uint8_t ai_need_draw; /* 1 = 到点后要重画 AI 页 */
static uint32_t diag_ms;

/* ==================================================================
 * 非阻塞步进序列 —— 手势和 @SWEEP 共用一套
 * ------------------------------------------------------------------
 * 为什么不直接 delay 做完整段：主循环还背着麦克风（每 500ms 报音量）、
 * 按键扫描、串口取行。一旦在某帧里死等 350ms，麦克风的半块回调就会积压。
 * ================================================================== */
#define AILINK_STEPS_MAX 16u
#define AILINK_SWEEP_HALF 800u /* 扫动每半程的毫秒数：0↔180 一个来回 1.6 秒 */

static uint8_t st_id[AILINK_STEPS_MAX];
static uint16_t st_ang[AILINK_STEPS_MAX];
static uint16_t st_hold[AILINK_STEPS_MAX];
static uint8_t st_len;
static uint8_t st_pos;
static uint8_t st_waiting;
static uint32_t st_due_ms;

static MotionTimeline timeline;
static IdlePet pet;
static char pet_face[25];
static uint8_t pet_face_saved, pet_owns_timeline;
static uint32_t pose_due;
static uint16_t pet_seq;
static uint8_t srv_min[16], srv_max[16], srv_front[16];
static uint8_t srv_nod = 1u, srv_yaw = 2u;
static uint32_t srv_revision;
static uint8_t timeline_paint_wait;
static uint16_t srv_mask, srv_home_pending, srv_home_speed;
static void pet_cancel(uint8_t restore);
static uint8_t pet_install(const uint8_t *p, uint16_t len);
static void pet_tick(uint32_t now, uint8_t blocked);
static void pose_report(uint32_t now, uint8_t blocked);

uint8_t AILink_GetServoInfo(uint8_t id, AILink_ServoInfo *info)
{
    uint8_t ch;
    if (id < 1u || id > 16u || info == 0)
        return 0u;
    ch = (uint8_t)(id - 1u);
    info->minimum = srv_min[ch];
    info->maximum = srv_max[ch];
    info->front = srv_front[ch];
    info->configured = (uint8_t)(srv_revision != 0u && (srv_mask & (1u << ch)) != 0u);
    info->target = (uint16_t)(servo_motion[ch].target / 1000u);
    info->speed = servo_motion[ch].speed;
    return 1u;
}

uint8_t AILink_GetScreenAngle(uint16_t *degrees)
{
    return PCA9685_GetAngleEstimate(PCA_ID_TO_CH(3u), degrees);
}

static void timeline_report(const char *state)
{
    char ack[96];
    sprintf(ack, "@TIMELINE:%s,%u,%u,%u,%u,%u\n", state, (unsigned)timeline.seq,
            (unsigned)timeline.pos, (unsigned)timeline.count, (unsigned)timeline.cycle,
            (unsigned)timeline.repeat);
    ESP_UART_SendStr(ack);
}

static void timeline_stop(void)
{
    if (timeline.active)
    {
        ServoMotion_Stop(timeline.mask);
        timeline.active = 0u;
        timeline_paint_wait = 0u;
        timeline_report("stopped");
    }
}

/* Complete limit snapshot. Applied only after all entries have validated. */
/* 参数同步先验证整包和角度范围，再发布修订号；半包不得部分改变机械限位。 */
static uint8_t do_limits(const uint8_t *p, uint16_t len)
{
    uint8_t i, j, nod = 0u, yaw = 0u;
    uint16_t seen = 0u;
    uint32_t revision;
    char ack[40];
    if (len < 6u || p[0] != 1u || !p[1] || p[1] > 16u || len != 6u + 6u * p[1])
        return 0u;
    revision = TL_u32(p + 2);
    if (!revision)
        return 0u;
    for (i = 0u; i < p[1]; i++)
    {
        const uint8_t *s = p + 6u + 6u * i;
        if (!s[0] || s[0] > 16u || s[1] > s[2] || s[2] > 180u || s[3] < s[1] || s[3] > s[2] ||
            s[4] > 2u || s[5] || (seen & (1u << (s[0] - 1u))))
            return 0u;
        seen |= (uint16_t)(1u << (s[0] - 1u));
        if (s[4] == 1u)
        {
            if (nod)
                return 0u;
            nod = s[0];
        }
        if (s[4] == 2u)
        {
            if (yaw)
                return 0u;
            yaw = s[0];
        }
    }
    if (revision != srv_revision)
    {
        pet_cancel(1u);
        pet.enabled = 0u;
        pet.revision = 0u;
        timeline_stop();
        st_len = st_pos = st_waiting = 0u;
        ServoMotion_Stop(0xFFFFu);
        for (j = 0u; j < 16u; j++)
        {
            srv_min[j] = 0u;
            srv_max[j] = 180u;
            srv_front[j] = 90u;
        }
        for (i = 0u; i < p[1]; i++)
        {
            const uint8_t *s = p + 6u + 6u * i;
            j = s[0] - 1u;
            srv_min[j] = s[1];
            srv_max[j] = s[2];
            srv_front[j] = s[3];
            /* Keep every subsequent interpolated PWM target inside new bounds. */
            if (servo_motion[j].current < (uint32_t)s[1] * 1000u)
                servo_motion[j].current = (uint32_t)s[1] * 1000u;
            if (servo_motion[j].current > (uint32_t)s[2] * 1000u)
                servo_motion[j].current = (uint32_t)s[2] * 1000u;
            servo_motion[j].target = servo_motion[j].current;
        }
        srv_nod = nod;
        srv_yaw = yaw;
        srv_mask = seen;
        srv_revision = revision;
    }
    sprintf(ack, "@LIMITS:OK,%lu\n", (unsigned long)revision);
    ESP_UART_SendStr(ack);
    return 1u;
}

/* Home is semantic: use the last validated front angle, never assume 90.
 * Requests made before the profile arrives remain pending without motor work. */
static void home_tick(uint32_t now, uint8_t paused)
{
    uint8_t i;
    uint16_t mask = (uint16_t)(srv_home_pending & srv_mask);
    char ack[128];
    unsigned used = 10u;
    if (paused || !srv_revision || !mask)
        return;
    timeline_stop();
    st_len = st_pos = st_waiting = 0u;
    strcpy(ack, "@ACK:home,");
    for (i = 0u; i < 16u; i++)
        if (mask & (1u << i))
        {
            uint16_t speed = (srv_home_speed == 0xFFFFu) ? servo_default_speed : srv_home_speed;
            ServoMotion_Move(i, srv_front[i], speed, now);
            used += (unsigned)sprintf(ack + used, "%s%u=%u", used == 10u ? "" : ",",
                                      (unsigned)(i + 1u), (unsigned)srv_front[i]);
        }
    ack[used++] = '\n';
    ack[used] = 0;
    ESP_UART_SendStr(ack);
    srv_home_pending = 0u;
    srv_home_speed = 0xFFFFu;
}

/* 手势驱动哪两路舵机。这两个数要和云端 config.json::hardware.servos 里
 * id1(nod，限 75~105°) / id2(yaw，全行程) 的约定对上。 */
#define AILINK_SRV_NOD 1u
#define AILINK_SRV_YAW 2u

/* 行缓冲。 用静态而不是局部：主栈只有 2KB，别在帧里再摊 192 字节。
 *  原来还有一个 rx_line[]（GetLine 的取行缓冲）—— 取文改成 GetMsg 之后
 *    文本行直接指向 esp_uart 自己的静态缓冲，这份多余的 192 字节已删。 */
static char line_buf[AILINK_LINE_MAX];

/* ------------------------------------------------------------------
 * 匹配前缀；命中返回**前缀之后**的指针，不命中返回 0。
 *  比 strncmp + 手写偏移好：偏移量由同一个字符串字面量算出来，
 *   改前缀时不会出现"判据改了、偏移没改"的错位。
 * ------------------------------------------------------------------ */
static const char *skip_prefix(const char *s, const char *pfx)
{
    while (*pfx != 0)
    {
        if (*s != *pfx)
        {
            return 0;
        }
        s++;
        pfx++;
    }
    return s;
}

/* ------------------------------------------------------------------
 * 解析十进制无符号数。*pp 前进到数字之后；返回 1 = 至少吃到一位数字。
 * 饱和在 UINT32_MAX（配置版本是CRC32）：让越界的输入变成**一个大数**（后面 range check 会拒掉），
 * 而不是让 uint32 绕回去变成一个小数 —— 那样越界反而会被当成合法值放行。
 * ------------------------------------------------------------------ */
static uint8_t scan_u32(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    uint32_t v = 0u;
    uint8_t any = 0u;

    while ((*p >= '0') && (*p <= '9'))
    {
        if (v > 429496729u || (v == 429496729u && (uint32_t)(*p - '0') > 5u))
            v = 0xFFFFFFFFu;
        else
            v = v * 10u + (uint32_t)(*p - '0');
        any = 1u;
        p++;
    }
    *pp = p;
    *out = v;
    return any;
}

/* 去掉帧尾的 '#' 和空白。就地改 line_buf（我们自己那份副本，不动调用者的串）。
 *  副作用：@TEXT: 的正文如果**以 '#' 或空格收尾**会被吃掉。可接受 ——
 *    协议里 @TEXT: 不走 '#' 结尾，正文结尾是 '#' 的情况现实里不会出现。 */
static void trim_tail(char *s)
{
    size_t n = strlen(s);

    while (n > 0u)
    {
        char c = s[n - 1u];
        if ((c != '#') && (c != ' ') && (c != '\t'))
        {
            break;
        }
        s[n - 1u] = 0;
        n--;
    }
}

/* ==================================================================
 * 步进序列
 * ================================================================== */

static void seq_begin(void)
{
    st_len = 0u;
    st_pos = 0u;
    st_waiting = 0u;
}

static void seq_add(uint8_t id, uint16_t ang, uint16_t hold_ms)
{
    if (st_len >= AILINK_STEPS_MAX)
    {
        return;
    }
    st_id[st_len] = id;
    st_ang[st_len] = ang;
    st_hold[st_len] = hold_ms;
    st_len++;
}

static void seq_arm(void)
{
    st_due_ms = GetTick_ms(); /* 第一步立刻执行 */
}

/* 作废序列里**还没执行**的、跟 id 有关的那几步。
 * 用途：一条 @MOVE:servo,1,90 打进来时，如果 1 号正在演手势或扫动，刚设的
 *       角度会被下一步立刻踩掉 —— 现象是"舵机不听话、自己又动了"。
 *       这里干脆把整段序列作废：手势被打断就重来，比半截停住好懂。
 *  只看 st_pos 之后的部分：已经执行过的步骤踩不到现在的角度。 */
static void seq_stop_for(uint8_t id)
{
    uint8_t i;

    for (i = (st_waiting && st_pos) ? st_pos - 1u : st_pos; i < st_len; i++)
    {
        if (st_id[i] == id)
        {
            st_len = 0u;
            st_pos = 0u;
            return;
        }
    }
}

static void seq_tick(uint32_t now)
{
    if (st_len == 0u)
        return;
    if (st_waiting)
    {
        if (ServoMotion_Busy(PCA_ID_TO_CH(st_id[st_pos - 1u])))
            return;
        st_waiting = 0u;
        if (st_pos >= st_len)
        {
            st_len = st_pos = 0u;
            return;
        }
        st_due_ms = now + (uint32_t)st_hold[st_pos - 1u];
    }
    if ((int32_t)(now - st_due_ms) < 0)
        return;
    ServoMotion_Move(PCA_ID_TO_CH(st_id[st_pos]), st_ang[st_pos], servo_default_speed, now);
    st_due_ms = now + (uint32_t)st_hold[st_pos];
    st_waiting = ServoMotion_Busy(PCA_ID_TO_CH(st_id[st_pos]));
    st_pos++;
    if (st_pos >= st_len && !st_waiting)
        st_len = st_pos = 0u;
}

/* ==================================================================
 * 执行后端
 * ================================================================== */

/* 只改"状态"，不碰屏幕 —— 真正的重绘等静默窗到点后统一做一次。 */
static void mark_draw(void)
{
    ai_need_draw = 1u;
    ai_due_ms = GetTick_ms() + AILINK_SETTLE_MS;
}

/* @MOVE:servo,<id>,<angle>   id 1~16（= PCA9685 丝印号），angle 0~180。
 *  越界**丢弃并报出来，不夹取** —— 越界意味着云端或协议出了错；静默夹到
 *   边界会让舵机转到一个没人预期的角度，比"不动"更难查。 */
static uint8_t do_servo(const char *p)
{
    const char *q = p;
    uint32_t id = 0u, ang = 0u, speed = servo_default_speed;
    if (!scan_u32(&q, &id) || *q++ != ',' || !scan_u32(&q, &ang))
        return 0u;
    if (*q == ',')
    {
        q++;
        if (!scan_u32(&q, &speed))
            return 0u;
    }
    if (*q != 0)
        return 0u;
    if (id < 1u || id > PCA9685_CH_NUM || ang > 180u || speed > 360u)
    {
        printf("[LINK] servo invalid id/angle/speed -> ignored\r\n");
        return 1u;
    }
    srv_home_pending = 0u;
    timeline_stop();
    if (ang < srv_min[id - 1u])
        ang = srv_min[id - 1u];
    if (ang > srv_max[id - 1u])
        ang = srv_max[id - 1u];
    seq_stop_for((uint8_t)id);
    ServoMotion_Move(PCA_ID_TO_CH(id), (uint16_t)ang, (uint16_t)speed, GetTick_ms());
    printf("[LINK] servo %lu -> %lu deg, speed=%lu deg/s (0=immediate)\r\n", (unsigned long)id,
           (unsigned long)ang, (unsigned long)speed);
    return 1u;
}

/* Saved webpage controls; acknowledgement proves MCU accepted the settings. */
static uint8_t do_home(const char *p)
{
    uint32_t id = 0u, speed = 0xFFFFu;
    uint16_t mask = 0xFFFFu;
    if (*p)
    {
        if (!strncmp(p, "all", 3u))
            p += 3;
        else
        {
            if (!scan_u32(&p, &id) || id < 1u || id > 16u)
                return 0u;
            mask = (uint16_t)(1u << (id - 1u));
        }
        if (*p == ',')
        {
            p++;
            if (!scan_u32(&p, &speed) || speed > 360u)
                return 0u;
        }
        if (*p)
            return 0u;
    }
    if (srv_revision && !(mask & srv_mask))
    {
        ESP_UART_SendStr("@ACK:home,unregistered\n");
        return 1u;
    }
    timeline_stop();
    st_len = st_pos = st_waiting = 0u;
    srv_home_pending = mask;
    srv_home_speed = (uint16_t)speed;
    if (!srv_revision)
        ESP_UART_SendStr("@ACK:home,waiting_profile\n");
    return 1u;
}

static uint8_t do_config(const char *p)
{
    uint32_t v[7];
    uint8_t i;
    char ack[40];
    for (i = 0u; i < 7u; i++)
    {
        if (!scan_u32(&p, &v[i]))
            return 0u;
        if (i < 6u)
        {
            if (*p++ != ',')
                return 0u;
        }
    }
    if (*p || !v[0] || v[1] > 1u || v[2] < 50u || v[2] > 8000u || v[3] > 8000u || v[4] < 64u ||
        v[4] > 640u || v[5] < 200u || v[5] > 2016u || v[6] > 360u)
        return 0u;
    if (IAP_IsBusy() || !MicLink_ConfigureVad((uint8_t)v[1], (uint16_t)v[2], (uint16_t)v[3],
                                              (uint16_t)v[4], (uint16_t)v[5]))
        return 1u;
    servo_default_speed = (uint16_t)v[6];
    sprintf(ack, "@CONFIG:OK,%lu\n", (unsigned long)v[0]);
    ESP_UART_SendStr(ack);
    return 1u;
}

/* @MOVE:gesture,<name>
 *   查表变成一串"角度 + 保持时间"，交给上面那套非阻塞序列去走。
 *   角度按云端 config.json 的约定：id1 = nod（限 75~105°），id2 = yaw（全行程）。
 *  名字不在表里 = 协议对不上 = 返回 0 让调用者丢弃，不要"什么都不做但算成功"。 */
static uint8_t do_gesture(const char *name)
{
    uint8_t id;
    if (strcmp(name, "nod") && strcmp(name, "hi") && strcmp(name, "look_left") &&
        strcmp(name, "look_right") && strcmp(name, "shake"))
        return 0u;
    id = (!strcmp(name, "nod") || !strcmp(name, "hi")) ? srv_nod : srv_yaw;
    if (!id)
        return 0u;
    srv_home_pending = 0u;
    timeline_stop();
    if (!strcmp(name, "nod")) /* 点头：低 → 高 → 低 → 回中 */
    {
        seq_begin();
        seq_add(AILINK_SRV_NOD, 75u, 350u);
        seq_add(AILINK_SRV_NOD, 105u, 350u);
        seq_add(AILINK_SRV_NOD, 75u, 350u);
        seq_add(AILINK_SRV_NOD, 90u, 0u);
    }
    else if (!strcmp(name, "hi")) /* 打招呼：来回摆两趟 */
    {
        seq_begin();
        seq_add(AILINK_SRV_NOD, 105u, 400u);
        seq_add(AILINK_SRV_NOD, 75u, 400u);
        seq_add(AILINK_SRV_NOD, 105u, 400u);
        seq_add(AILINK_SRV_NOD, 90u, 0u);
    }
    else if (!strcmp(name, "look_left")) /* 转头：直接到位，不用序列 */
    {
        seq_begin();
        seq_add(AILINK_SRV_YAW, 0u, 0u);
    }
    else if (!strcmp(name, "look_right"))
    {
        seq_begin();
        seq_add(AILINK_SRV_YAW, 180u, 0u);
    }
    else if (!strcmp(name, "shake")) /* 摇头：左 → 右 → 回中 */
    {
        seq_begin();
        seq_add(AILINK_SRV_YAW, 0u, 400u);
        seq_add(AILINK_SRV_YAW, 180u, 400u);
        seq_add(AILINK_SRV_YAW, 90u, 0u);
    }
    else
    {
        return 0u;
    }

    {
        uint8_t i;
        for (i = 0u; i < st_len; i++)
        {
            uint16_t a = st_ang[i];
            st_id[i] = id;
            st_ang[i] =
                (a == 90u) ? srv_front[id - 1u] : (a < 90u ? srv_min[id - 1u] : srv_max[id - 1u]);
        }
    }
    seq_arm();
    printf("[LINK] gesture -> %s\r\n", name);
    return 1u;
}

/* @MOVE:expression,<face>
 *  这里**不**拿 5 个名字去硬校验：表情库在 SD 卡的 FACE\ 目录里，卡里放
 *   什么就能用什么（串口 /faces 列清单）。名字不对时 bmp_blit 会在表情框里
 *   写一行 "NO SUCH BMP"，比在这一层拦掉更好查。 */
static uint8_t do_expression(const char *name)
{
    if (*name == 0)
    {
        return 0u;
    }

    Media_SetFace(name);
    mark_draw();
    printf("[LINK] face -> %s\r\n", name);
    return 1u;
}

/* 只执行本圈到期步骤，后续步骤留在队列；动作接收与动作完成回执属于不同阶段。 */
static void timeline_execute(const uint8_t *s, uint32_t now)
{
    if (s[4] == 1u)
    {
        ServoMotion_Move(s[5] - 1u, TL_u16(s + 6), TL_u16(s + 8), now);
    }
    else
    {
        char name[9];
        memcpy(name, s + 10, 8u);
        name[8] = 0;
        (void)do_expression(name);
        timeline_paint_wait = 1u;
    }
}

/* @SWEEP:servo,<id>
 *  扫动**有终点**：来回 4 趟（8 步 × 800ms ≈ 6.4 秒）就自己停。
 *   不做"永久扫"—— 下命令的人走了以后舵机会一直顶着来回走，费电、发热、
 *   减速箱磨损，现场看起来还像"板子失控了"。
 *   想中途停：对同一路发一条 @MOVE:servo,<id>,<角度> 即可（seq_stop_for）。 */
static uint8_t do_sweep(const char *p)
{
    const char *q = p;
    uint32_t id = 0u;
    uint8_t i;

    if ((scan_u32(&q, &id) == 0u) || (*q != 0))
    {
        return 0u;
    }
    if ((id < 1u) || (id > (uint32_t)PCA9685_CH_NUM))
    {
        printf("[LINK] sweep id %lu out of 1~%u -> ignored\r\n", (unsigned long)id,
               (unsigned)PCA9685_CH_NUM);
        return 1u;
    }

    srv_home_pending = 0u;
    seq_begin();
    for (i = 0u; i < 8u; i++)
    {
        seq_add((uint8_t)id, ((i & 1u) != 0u) ? srv_max[id - 1u] : srv_min[id - 1u],
                AILINK_SWEEP_HALF);
    }
    seq_arm();
    printf("[LINK] sweep id %lu: 4 round trips then stops\r\n", (unsigned long)id);
    return 1u;
}

/* @LED_ON,LED<1|2> / @LED_OFF,LED<1|2>
 * 板载双 LED（PA6/PA7 = TIM3_CH1/CH2，**低电平点亮**，CCR 越大越亮）。
 *  前提是 main.c 自检里已经调过 LED_PWM_Init()，否则 TIM3 时钟没开，
 *    这两个函数写进去也没反应，而且**不报错**。 */
static uint8_t do_led(const char *name, uint8_t on)
{
    uint8_t ch;

    if (!strcmp(name, "LED1"))
    {
        ch = 1u;
    }
    else if (!strcmp(name, "LED2"))
    {
        ch = 2u;
    }
    else
    {
        return 0u;
    }

    LED_PWM_SetBrightness(ch, (on != 0u) ? 999u : 0u);
    printf("[LINK] LED%u %s\r\n", (unsigned)ch, (on != 0u) ? "ON" : "OFF");
    return 1u;
}

static uint8_t do_mic(const char *arg)
{
    const char *q = arg;
    uint32_t sec = 0u;

    if (IAP_IsBusy() != 0u)
    {
        printf("[LINK] MIC refused: IAP busy (%s)\r\n", IAP_StateName());
        return 1u; /* 认出来了、只是拒了 ⇒ 别掉进 drop_line 当未知帧 */
    }

    if (!strcmp(arg, "start")) /* 用默认时长 */
    {
        MicLink_RequestStart(0u);
        printf("[LINK] MIC start (default %us)\r\n", (unsigned)MICLINK_SEC_DEFAULT);
        return 1u;
    }

    if (!strcmp(arg, "stop"))
    {
        MicLink_RequestStop(); /* 里面会自己打结果（撤回了 / 忽略 / 停不下来） */
        printf("[LINK] MIC stop\r\n");
        return 1u;
    }

    if (!strncmp(arg, "rec,", 4u))
    {
        q = arg + 4u;
        /* 必须"至少一位数字 + 到串尾为止全是数字"：rec,  / rec,2x / rec,,2 都算格式错 */
        if ((scan_u32(&q, &sec) == 0u) || (*q != 0))
        {
            return 0u;
        }

        if ((sec < 1u) || (sec > 99u))
        {
            /* 认出来了但参数不合理 ⇒ 记一笔（返回 1 = 已消费），别当未知帧。
             * 这里**只拒 0 和 >99**；1~99 都收下，再由 miclink 夹到
             * MICLINK_SEC_MAX_PLANNED（现在 = 10，`miclink.h:44`）—— 云端要 5 秒是正常需求，
             * 不该整条罚掉。 */
            printf("[LINK] MIC rec %lu out of 1~99 -> ignored\r\n", (unsigned long)sec);
            return 1u;
        }

        MicLink_RequestStart((uint16_t)sec);
        printf("[LINK] MIC rec %lus\r\n", (unsigned long)sec);
        return 1u;
    }

    return 0u;
}
/* ===  === */

/* 丢弃一行。 只打长度，绝不打内容 —— 理由见文件头。 */
static void drop_line(const char *p)
{
    size_t n = strlen(p);

    ailink_drop_cnt++;

    if (p[0] == '@')
    {
        /* '@' 开头却不在白名单 = **协议问题**（固件比云端旧、或云端发错），
         * 这是真故障，一定要看得见。 */
        printf("[LINK] drop @frame (%u bytes) - not in whitelist\r\n", (unsigned)n);
        return;
    }

    /* 非 '@' 开头 = ESP 自己的调试日志（"[HTTP] ..."）。这是**必然出现**的，
     * 不是故障。头 3 条打出来让你确认"线是通的、ESP 确实在说话"，之后只记数
     * 不刷屏 —— 否则每点一次网页就多两行噪声，真出问题时反而被埋掉。 */
    if (ailink_drop_cnt <= 3u)
    {
        printf("[LINK] drop non-frame (%u bytes) - ESP debug log, normal\r\n", (unsigned)n);
    }
}

#define AILINK_T_MINE_LO 0x01u
#define AILINK_T_MINE_HI 0x0Fu
#define AILINK_T_IAP_LO 0x10u
#define AILINK_T_IAP_HI 0x1Fu

static uint16_t frame_drop_cnt; /* 丢掉的二进制帧（未知 type / 方向不对 / 残帧）*/

/* ==================================================================
 * === OTA 的**屏上进度**（用户要求："加了提示，
 *     我才知道有没有开始"）
 * ------------------------------------------------------------------
 *  为什么放在 ailink：0x10 数据块**唯一**的必经之路是本文件的
 *   handle_esp_frame() —— 只有这儿同时看得见"总量"（REQUEST 行）和
 *   "已收多少"（02 的 IAP_Got()）。02 的模块只报已收字节、不报总量。
 *  为什么进度**不按块刷**：paint 一次 150~300ms，而整包在 921600 上
 *   只跑 1s 上下 ⇒ 按块刷屏会把传输拖成好几秒，还可能撞上 ESP 那边
 *   1200ms 的块 ACK 超时。**判据取时间（≥1s 才允许重绘一次），不取百分比。**
 *  状态全在本文件，不占 02 的接口、不改它的 .h。
 * ================================================================== */
#define AILINK_OTA_WAIT_MS 125000u /* 没人按 K-UP：02 那边等 120s ⇒ 多留 5s 收尾 */
#define AILINK_OTA_STALL_MS                                                                        \
    20000u                        /* 收数据停滞：COMMIT 后到重启最多 15s
                                        * （IAP_JUMP_TIMEOUT_MS）⇒ 取 20s，成功那条路
                                        * 会先重启，屏上不会闪出这句 */
#define AILINK_OTA_PAINT_MS 1000u /* 两次进度重绘的最小间隔 */

static uint32_t ota_total;    /* 本次申请的整包长度；0 = 没有进行中的 OTA */
static uint32_t ota_req_ms;   /* 收到 REQUEST 的时刻（用于"没人按键"的收尾）*/
static uint32_t ota_last_ms;  /* 最近一块 0x10 的时刻；0 = 还没开始收数据 */
static uint32_t ota_paint_ms; /* 上次把进度写上屏的时刻（节流用）*/
static uint8_t ota_pct;       /* 当前百分比（判"进度变了没有"）*/
static char ota_cap[48];      /* 上屏文字；Media_SetCaption() 会自己 strncpy 走 */

/* 我的段（0x01~0x0F）里，F407 **只该收到** 0x04；
 * 0x01/0x02/0x03 都是上行才用的类型，下行收到 = 对面发错了。 */
static uint8_t handle_esp_frame(uint8_t type, uint16_t seq, const uint8_t *data, uint16_t len)
{
    if (type == 0x33u)
    {
        CameraLink_Ack(seq, data, len);
        return 1u;
    }
    if (type == 0x23u)
        return pet_install(data, len);
    if (type == 0x20u || type == 0x21u)
        AILink_UserActivity();
    if (type == 0x22u)
        return do_limits(data, len);
    if (type == 0x21u)
    {
        if (len)
            return 0u;
        srv_home_pending = 0u;
        timeline_stop();
        timeline_report("stopped");
        return 1u;
    }
    if (type == 0x20u)
    {
        uint8_t result, i;
        if (!TL_validate(data, len))
        {
            timeline_report("invalid");
            return 0u;
        }
        for (i = 0u; i < data[1]; i++)
        {
            const uint8_t *s = data + 8u + 18u * i;
            if (s[4] == 1u &&
                (TL_u16(s + 6) < srv_min[s[5] - 1u] || TL_u16(s + 6) > srv_max[s[5] - 1u]))
            {
                timeline_report("limits_error");
                return 0u;
            }
        }
        if (timeline.bytes && timeline.seq == seq)
        {
            result = TL_load(&timeline, data, len, seq, GetTick_ms());
        }
        else
        {
            srv_home_pending = 0u;
            timeline_stop();
            st_len = st_pos = st_waiting = 0u;
            ServoMotion_Stop(0xFFFFu);
            result = TL_load(&timeline, data, len, seq, GetTick_ms());
        }
        timeline_report(result ? (timeline.active ? "running" : "done") : "invalid");
        return (uint8_t)(result != 0u);
    }
    /* ---------- 02 的段：IAP ---------- */
    if ((type >= AILINK_T_IAP_LO) && (type <= AILINK_T_IAP_HI))
    {
        if (CameraLink_Busy() || MicAud_IsStreaming() != 0u || FontUpload_Active() != 0u)
        {
            ESP_UART_SendStr("@IAP:ERR,7#\r\n"); /* 7 = IAP_EC_BUSY */
            frame_drop_cnt++;
            printf("[LINK] IAP frame 0x%02X refused: audio streaming\r\n", (unsigned)type);
            return 0u;
        }
        IAP_OnFrame(type, seq, data, len); /* 内容我不解释 */
        /*  ：把 OTA 进度写上屏（状态与节流理由见上面 ota_* 那段）。
         *   判据只看 `0x10`（数据块）—— 0x13/0x15 是控制帧，来了不代表有数据在动。 */
        if ((ota_total != 0u) && (type == IAP_T_DATA))
        {
            uint32_t now = GetTick_ms();
            uint32_t got = IAP_Got();
            ota_last_ms = now;
            if ((ota_paint_ms == 0u) || ((uint32_t)(now - ota_paint_ms) >= AILINK_OTA_PAINT_MS))
            {
                uint8_t pct = (uint8_t)((got >= ota_total) ? 100u : ((got * 100u) / ota_total));
                if ((pct != ota_pct) || (ota_paint_ms == 0u))
                {
                    ota_pct = pct;
                    ota_paint_ms = now;
                    if (pct >= 100u)
                        snprintf(ota_cap, sizeof(ota_cap), "OTA %lu B RECEIVED\nVERIFY + REBOOT",
                                 (unsigned long)got);
                    else
                        snprintf(ota_cap, sizeof(ota_cap), "OTA RECEIVING %u%%\nDO NOT POWER OFF",
                                 (unsigned)pct);
                    Media_SetCaption(ota_cap);
                    mark_draw();
                }
            }
        }
        return 1u;
    }

    /* ---------- 我的段：音频 ---------- */
    if ((type >= AILINK_T_MINE_LO) && (type <= AILINK_T_MINE_HI))
    {
        if (type == MICAUD_T_ACK)
        {
            if (len < 1u) /* flags 都没有 = 残帧 */
            {
                frame_drop_cnt++;
                printf("[LINK] drop frame 0x04: len=%u\r\n", (unsigned)len);
                return 0u;
            }
            /* seq = next_seq，data[0] = flags（0 继续 / 1 从 next_seq 重传）*/
            MicAud_OnAck(seq, data[0]);
            return 1u;
        }
        frame_drop_cnt++;
        printf("[LINK] drop frame 0x%02X: up-only type on downlink\r\n", (unsigned)type);
        return 0u;
    }

    /* ---------- 都不是（信封合法但 type 没分配）---------- */
    frame_drop_cnt++;
    printf("[LINK] drop frame 0x%02X (unassigned, len=%u)\r\n", (unsigned)type, (unsigned)len);
    return 0u;
}

/* ==================================================================
 * 帧入口
 * ================================================================== */
/* 这条帧是不是"会动外设"的那一类 —— 只给 IAP 忙时那道闸用（见 handle_esp_line）。
 *  为什么单独一个谓词、而不是在 dispatch 里逐个分支加判断：
 *   闸必须**先于** dispatch 生效（拒掉就不该走到 do_servo 里去），所以只能是前置判断。
 *  这张前缀表必须跟下面 handle_esp_line 里的 dispatch 表**同步改** ——
 *   两边是同一份白名单的两半，漏一处就会出现"能过闸但没人接"的帧。
 *  `@MIC:` / `@IAP:` / `@PING` 都**不在**这里：各有各的门（见各自的注释）。 */
static uint8_t is_actuator_frame(const char *s)
{
    return ((skip_prefix(s, "@MOVE:servo,") != 0) || (skip_prefix(s, "@MOVE:gesture,") != 0) ||
            (skip_prefix(s, "@MOVE:expression,") != 0) || (skip_prefix(s, "@SWEEP:servo,") != 0) ||
            (skip_prefix(s, "@LED_ON,") != 0) || (skip_prefix(s, "@LED_OFF,") != 0) ||
            (skip_prefix(s, "@TEXT:") != 0))
               ? 1u
               : 0u;
}

static uint32_t link_ping_ms;
static uint8_t link_seen;
uint8_t AILink_LinkAlive(uint32_t now)
{
    return (uint8_t)(link_seen && (uint32_t)(now - link_ping_ms) <= 8000u);
}

uint8_t handle_esp_line(const char *line)
{
    const char *a;
    uint8_t handled = 0u;

    if (line == 0)
    {
        return 0u;
    }
    if (strlen(line) >= sizeof(line_buf))
    {
        drop_line(line);
        return 0u;
    }

    /* 拷进自己的可写缓冲：要就地剪掉帧尾的 '#'，不能动调用者的串。 */
    strncpy(line_buf, line, (size_t)(AILINK_LINE_MAX - 1u));
    line_buf[AILINK_LINE_MAX - 1u] = 0;
    trim_tail(line_buf);

    if (line_buf[0] == 0)
    {
        return 0u;
    } /* 空行（ESP 打的裸 CRLF）不算事 */

    if (!strncmp(line_buf, "@CAM:", 5u))
    {
        unsigned sid;
        char tail;
        if (sscanf(line_buf, "@CAM:SNAP,%u%c", &sid, &tail) == 1 && sid > 0u && sid <= 65535u)
        {
            if (!CameraLink_Request((uint16_t)sid))
            {
                char err[64];
                sprintf(err, "@CAM:ERROR,%u,MCU_BUSY\r\n", sid);
                ESP_UART_SendStr(err);
            }
            return 1u;
        }
        if (sscanf(line_buf, "@CAM:DONE,%u%c", &sid, &tail) == 1 && sid <= 65535u)
        {
            CameraLink_Complete((uint16_t)sid, 1u);
            return 1u;
        }
        if (sscanf(line_buf, "@CAM:FAIL,%u%c", &sid, &tail) == 1 && sid <= 65535u)
        {
            CameraLink_Complete((uint16_t)sid, 0u);
            return 1u;
        }
        return 0u;
    }
    /* Link probe is independent of actuators and command counters. */
    if (!strcmp(line_buf, "@PING"))
    {
        /*  行尾用 `\r\n` 不是 `\n`：这跟 `miclink.c:216` 造 `@USER:` 帧的写法一致，
         *   而 `\r\n` 是**严格更宽**的那种 —— 对面若按 `\r\n` 精确切行，一条 `\n`
         *   结尾的行会被**并进下一行**；反过来按 `\n` 切再 trim，`\r\n` 照样对。
         *   ESP 侧 `readStringUntil('\n')` + `line.trim()`（`.ino:661-662`）两种都吃。 */
        link_seen = 1u;
        link_ping_ms = GetTick_ms();
        ESP_UART_SendStr("@PONG\r\n");
        return 1u;
    }

    if (IAP_IsBusy() != 0u)
    {
        if (is_actuator_frame(line_buf) != 0u)
        {
            ESP_UART_SendStr("@IAP:ERR,7#\r\n"); /* 7 = IAP_EC_BUSY */
            frame_drop_cnt++;
            printf("[LINK] actuator frame refused: IAP busy (%s)\r\n", IAP_StateName());
            /*  返回 1 = **已消费**：这是认出来的协议帧，不是未知帧 ——
             *   再掉进 drop_line() 只会白涨 ailink_drop_cnt、把真故障埋掉。 */
            return 1u;
        }
    }
    /* ===  === */

    if (skip_prefix(line_buf, "@OTA_REQ,") != 0)
    {
        if (CameraLink_Busy() || MicAud_IsStreaming() != 0u || FontUpload_Active() != 0u)
        {
            ESP_UART_SendStr("@OTA_ACK,NO#\r\n");
            printf("[LINK] OTA_REQ refused: audio streaming\r\n");
        }
        else
        {
            /* TODO(01 -> 02OTA): IAP_OnOtaReq() 交付后接这里 */
            printf("[LINK] OTA_REQ: no handler yet (等 02OTA 的 IAP_OnOtaReq)\r\n");
        }
        return 1u;
    }
    /* ===  === */

    if (is_actuator_frame(line_buf) || skip_prefix(line_buf, "@MOVE:home") ||
        skip_prefix(line_buf, "@MIC:") || skip_prefix(line_buf, "@IAP:REQUEST,") ||
        !strcmp(line_buf, "@TIMELINE:STOP"))
        AILink_UserActivity();

    if ((a = skip_prefix(line_buf, "@CONFIG:")) != 0)
    {
        handled = do_config(a);
    }
    else if (!strcmp(line_buf, "@MOVE:home"))
    {
        handled = do_home("");
    }
    else if ((a = skip_prefix(line_buf, "@MOVE:home,")) != 0)
    {
        handled = do_home(a);
    }
    else if ((a = skip_prefix(line_buf, "@MOVE:servo,")) != 0)
    {
        handled = do_servo(a);
    }
    else if ((a = skip_prefix(line_buf, "@MOVE:gesture,")) != 0)
    {
        handled = do_gesture(a);
    }
    else if ((a = skip_prefix(line_buf, "@MOVE:expression,")) != 0)
    {
        timeline_stop();
        handled = do_expression(a);
    }
    else if ((a = skip_prefix(line_buf, "@SWEEP:servo,")) != 0)
    {
        timeline_stop();
        handled = do_sweep(a);
    }
    else if (!strcmp(line_buf, "@TIMELINE:STOP"))
    {
        timeline_stop();
        timeline_report("stopped");
        handled = 1u;
    }
    else if ((a = skip_prefix(line_buf, "@LED_ON,")) != 0)
    {
        handled = do_led(a, 1u);
    }
    else if ((a = skip_prefix(line_buf, "@LED_OFF,")) != 0)
    {
        handled = do_led(a, 0u);
    }
    else if ((a = skip_prefix(line_buf, "@TEXT:")) != 0)
    {
        /*  按前缀截后面**全部**，不要按逗号 split —— 决策协议v2.md 明确
         *   @TEXT: 里可以带逗号（"hello, world, ok" 要整句上屏）。 */
        Media_SetCaption(a);
        mark_draw();
        handled = 1u;
    }

    /*  跟 @TEXT: 一样，**按前缀截后面全部**再自己严格比对（见 do_mic）。
     *    这个分支只记请求、不阻塞，所以**不需要** mark_draw()：
     *      要不要重画由 MicLink_Tick() 的返回值决定（它才知道表情变没变）。 */
    else if ((a = skip_prefix(line_buf, "@MIC:")) != 0)
    {
        handled = do_mic(a);
    }
    else if (!strcmp(line_buf, "@AUDIO:BUSY"))
    {
        MicLink_OnAudioStatus(1u);
        handled = 1u;
    }
    else if (!strcmp(line_buf, "@AUDIO:READY"))
    {
        MicLink_OnAudioStatus(0u);
        handled = 1u;
    }
    else if ((a = skip_prefix(line_buf, "@AUDIO:READY,")) != 0)
    {
        char *end;
        unsigned long lag = strtoul(a, &end, 10);
        if (a[0] != 0 && *end == 0)
        {
            MicLink_OnAudioStatus(0u);
            printf("[MICLINK] ESP upload confirmed, after capture %lu ms\r\n", lag);
            handled = 1u;
        }
    }
    else if (!strcmp(line_buf, "@AUDIO:FAIL"))
    {
        MicLink_OnAudioStatus(0u);
        printf("[MICLINK] ESP upload failed; see cloud audio status\r\n");
        handled = 1u;
    }
    /* ===  === */
    /* ===  ===
     * @IAP: 文本帧 ——  起经 **ESP 这条线**也能下发 IAP 命令了
     * （以前只有调试口 USART2 那条路走 IAP_AppCommand，见 main.c）。
     *  传的是**整行**（含 "@IAP:" 前缀），不是 a —— 02 的解析器自己 starts_with
     *   比对前缀（iap_app.c:300 起），把前缀吃掉再给他会变成"不认识的命令"。
     *  互斥闸跟二进制帧同源（理由见 handle_esp_frame 上面那段）：
     *   流式录音期间**拒**，并回一个数字码 ERR,7 (= BUSY)。
     *  帧尾的 '#' 已被 trim_tail 剪掉 —— 对 02 的解析无害：
     *   他全用前缀比对 + strtoul/strchr，没有一处依赖 '#'。 */
    else if (skip_prefix(line_buf, "@IAP:") != 0)
    {
        if (CameraLink_Busy() || MicAud_IsStreaming() != 0u || FontUpload_Active() != 0u)
        {
            ESP_UART_SendStr("@IAP:ERR,7#\r\n");
            frame_drop_cnt++;
            printf("[LINK] IAP cmd refused: audio streaming\r\n");
            /*  拒掉的那条算"已消费"：它是**认出来的**协议帧，不是未知帧 ——
             *   再掉进 drop_line() 只会白涨 ailink_drop_cnt、把真故障埋掉。 */
            handled = 1u;
        }
        else
        {
            /* 0 = 02 也不认这条子命令（`@IAP:FOO`）⇒ **照常掉进 drop_line**，
             * 让"@ 开头却没人认"这个真故障看得见。别在这儿兜底成 1。 */
            handled = IAP_AppCommand(line_buf);

            /*  请求**从 ESP 这条线**进来时也要在屏上给提示。
             *   原先那句提示只写在 main.c 的**调试口**收行路径上（`main.c:567`），
             *   而 ESP 来的 `@IAP:REQUEST` 根本不走那儿 ⇒ 屏上什么都不显示，
             *   用户只能盲按 K-UP。 照本文件自己的习惯来：SetCaption + mark_draw()，
             *   由静默窗统一重绘一次（同 `@TEXT:` 那一段的写法）。 */
            if (handled != 0u && skip_prefix(line_buf, "@IAP:REQUEST,") != 0)
            {
                /*  键名改回 **K-UP**（02 的 iap_app.c:426 打印的就是
                 *   "press K-UP to approve"，ESP 侧 bridge_iap_esp.h:12 也写 K-UP ——
                 *   原先这儿写 PE3 是我按 key.h 那个新键抄的，与协议文档不符）；
                 *    顺手把**包长**记下来，后面按 IAP_Got()/total 出百分比。 */
                const char *arg = skip_prefix(line_buf, "@IAP:REQUEST,");
                ota_total = (uint32_t)strtoul(arg, 0, 10); /* 解析口径同 02：前缀 + strtoul */
                ota_req_ms = GetTick_ms();
                ota_last_ms = 0u;
                ota_paint_ms = 0u;
                ota_pct = 0u;
                snprintf(ota_cap, sizeof(ota_cap), "OTA REQUESTED %lu B\nPRESS PE3 OR K-UP",
                         (unsigned long)ota_total);
                Media_SetCaption(ota_cap);
                mark_draw();
            }
        }
    }
    /* ===  === */
    else
    {
        handled = 0u;
    }

    if (handled != 0u)
    {
        ailink_ok_cnt++;
        /*  认出来的帧把静默窗往后推：一次 5 帧连发要等它们**全到齐**再重绘
         *   一次，而不是刷 5 次。没有待重绘的东西时推它没意义，跳过。 */
        if (ai_need_draw != 0u)
        {
            ai_due_ms = GetTick_ms() + AILINK_SETTLE_MS;
        }
        return 1u;
    }

    drop_line(line_buf);
    return 0u;
}

uint8_t AILink_ServoBusy(void)
{
    uint8_t i;
    for (i = 0u; i < 16u; i++)
        if (ServoMotion_Busy(i))
            return 1u;
    return 0u;
}

uint8_t AILink_OtaPending(void)
{
    return (uint8_t)(ota_total != 0u);
}
uint8_t AILink_TimelineNeedsPaint(void)
{
    return timeline_paint_wait;
}
void AILink_TimelinePainted(void)
{
    timeline_paint_wait = 0u;
    timeline.last = GetTick_ms();
}
void AILink_ServiceMotion(void)
{
    ServoMotion_Tick(GetTick_ms(),
                     (uint8_t)(CameraLink_Busy() || MicAud_IsStreaming() || MicLink_IsRecording() ||
                               IAP_IsBusy() || FontUpload_Active() || AILink_OtaPending()));
}

static void pet_cancel(uint8_t restore)
{
    if (pet_owns_timeline)
    {
        timeline_stop();
        timeline.bytes = 0u;
        pet_owns_timeline = 0u;
    }
    if (restore && pet_face_saved)
    {
        Media_SetFace(pet_face);
        mark_draw();
    }
    pet_face_saved = 0u;
    Pet_Activity(&pet, GetTick_ms());
}
void AILink_UserActivity(void)
{
    pet_cancel(1u);
}
static uint8_t pet_install(const uint8_t *p, uint16_t len)
{
    uint8_t i, j;
    uint16_t off = 24u, n;
    char ack[48];
    if (!Pet_Valid(p, len) || TL_u32(p + 8) != srv_revision || !srv_revision)
        return 0u;
    for (i = 0u; i < p[1]; i++)
    {
        n = TL_u16(p + off + 2);
        off += 4u;
        for (j = 0u; j < p[off + 1]; j++)
        {
            const uint8_t *s = p + off + 8u + 18u * j;
            if (s[4] == 1u &&
                (!(srv_mask & (1u << (s[5] - 1u))) || TL_u16(s + 6) < srv_min[s[5] - 1u] ||
                 TL_u16(s + 6) > srv_max[s[5] - 1u]))
                return 0u;
        }
        off += n;
    }
    if (pet.revision != TL_u32(p + 4))
    {
        pet_cancel(1u);
        Pet_Load(&pet, p, GetTick_ms());
    }
    sprintf(ack, "@IDLE:OK,%lu\r\n", (unsigned long)pet.revision);
    ESP_UART_SendStr(ack);
    return 1u;
}
static void pet_tick(uint32_t now, uint8_t blocked)
{
    uint8_t chosen;
    if (blocked)
    {
        pet_cancel(0u);
        return;
    }
    if (pet.running && !timeline.active)
    {
        uint8_t sleep = (uint8_t)(pet.kind[pet.running - 1u] == 2u);
        Pet_Finished(&pet, now);
        if (!sleep && pet_face_saved)
        {
            Media_SetFace(pet_face);
            pet_face_saved = 0u;
            mark_draw();
        }
    }
    chosen = Pet_Select(&pet, now,
                        (uint8_t)(timeline.active || AILink_ServoBusy() || st_pos < st_len ||
                                  srv_home_pending || !srv_revision));
    if (!chosen)
        return;
    if (!pet_face_saved)
    {
        strncpy(pet_face, Media_GetFace(), sizeof(pet_face) - 1u);
        pet_face[sizeof(pet_face) - 1u] = 0;
        pet_face_saved = 1u;
    }
    /* Idle shares the single executor; no autonomous task queues accumulate. */
    memset(&timeline, 0, sizeof(timeline));
    pet_seq++;
    if (TL_load(&timeline, pet.clips[chosen - 1u], pet.bytes[chosen - 1u], pet_seq, now))
    {
        pet.running = chosen;
        pet_owns_timeline = 1u;
    }
}
static void pose_report(uint32_t now, uint8_t blocked)
{
    char frame[112];
    uint16_t a[3] = {0u, 0u, 0u};
    uint8_t i, mask = 0u;
    if (blocked || now - pose_due < 250u)
        return;
    pose_due = now;
    for (i = 0u; i < 3u; i++)
        if (PCA9685_GetAngleEstimate(i, &a[i]))
            mask |= (uint8_t)(1u << i);
    sprintf(frame, "@POSE:%lu,%u,%u,%u,%u,%u,%u,%lu\r\n", (unsigned long)now, (unsigned)mask,
            (unsigned)a[0], (unsigned)a[1], (unsigned)a[2], (unsigned)AILink_ServoBusy(),
            (unsigned)(pet.sleeping ? 4u : pet.running), (unsigned long)pet.revision);
    ESP_UART_SendStr(frame);
}

uint8_t AILink_Poll(void)
{
    uint32_t now = GetTick_ms();
    int result;
    esp_msg_t msg;

    /*  把这一圈到达的**文本行和二进制帧**取尽。
     *    一次 5 帧连发必须全部吃掉：留到下一圈的话重绘会被拖成好几次，
     *    而且 1024 字节的环会被撑满、静默丢字节。
     *     音频流期间这条循环**几乎每圈都空转**（一条 POST 上来才一个 0x04），
     *      真正吃带宽的是 0x02 上行，方向不同、互不干扰。
     *  取到 -1（坏帧 / 超长行）**必须接着取**：解析器已经把那一整条丢弃并对齐
     *    （见 esp_uart.c 的 msg_poll），环里剩下的好帧还等着被取走 ——
     *    看到 -1 就 break 会让它们压到下一圈甚至被撑爆。老 GetLine 也是这个规矩。 */
    while ((result = ESP_UART_GetMsg(&msg)) != 0)
    {
        if (result > 0)
        {
            /*  分流的**唯一**判据是 kind，不是"内容像不像帧"。
             *   帧的 payload 里可以有 0x0A 和 0x00，行解析器碰一下就废（文件头）。 */
            if (msg.kind == ESP_MSG_FRAME)
                (void)handle_esp_frame(msg.type, msg.seq, msg.data, msg.len);
            else
                (void)handle_esp_line((const char *)msg.data); /* TEXT 保证以 0 结尾 */
        }
        else
        {
            ailink_drop_cnt++;
            printf(
                "[LINK] RX discarded: long/corrupt burst; bytes=%lu overflow=%lu uart_error=%lu\r\n",
                (unsigned long)esp_rx_bytes, (unsigned long)esp_rx_overflows,
                (unsigned long)esp_rx_errors);
        }
    }

    CameraLink_Tick(GetTick_ms());

    /*  手势 / 扫动的步进（非阻塞，每圈最多推进一步） */
    now = GetTick_ms(); /* Commands above may have just reset a channel's timestamp. */
    {
        uint8_t paused =
            (uint8_t)(CameraLink_Busy() || MicAud_IsStreaming() || MicLink_IsRecording() ||
                      IAP_IsBusy() || FontUpload_Active() || AILink_OtaPending());
        uint8_t was_paused = timeline.paused;
        uint8_t event;
        pet_tick(now, (uint8_t)(paused || !strcmp(MicLink_AudioStateName(), "UPLOADING")));
        pose_report(now, paused);
        ServoMotion_Tick(now, paused);
        home_tick(now, paused);
        if (timeline_paint_wait && !paused)
        {
            timeline.last = now;
            event = 0u;
        }
        else
            event = TL_tick(&timeline, now, paused, AILink_ServoBusy(), timeline_execute);
        if (event || (timeline.active && was_paused != timeline.paused))
            timeline_report(timeline.active ? (timeline.paused ? "paused" : "running") : "done");
    }
    if (!CameraLink_Busy() && !MicAud_IsStreaming() && !IAP_IsBusy())
        seq_tick(now);

    if ((uint32_t)(now - diag_ms) >= 5000u && !MicAud_IsStreaming() && !FontUpload_Active() &&
        !AILink_ServoBusy() && !IAP_IsBusy())
    {
        diag_ms = now;
        printf("[LINK] RX bytes=%lu ok=%u drop=%u overflow=%lu uart_error=%lu\r\n",
               (unsigned long)esp_rx_bytes, (unsigned)ailink_ok_cnt, (unsigned)ailink_drop_cnt,
               (unsigned long)esp_rx_overflows, (unsigned long)esp_rx_errors);
        /* 二进制帧一行（ 排查音频/OTA 时先看这行）：
         *   ok   = 收下的好帧（音频流期间 = 每个 0x04 一个）
         *   bad  = xor 不过 / len 超限，**整帧已丢**（esp_uart.c 收的时候就算）
         *   over = payload 超 ESP_FRAME_MAX 的帧（同上）
         *   dropped = 信封好、但**分发时**没人要的（未知 type / 上行类型跑到下行）
         *   defer   = 有人还在用老的 GetLine 模式、撞到帧让出去的次数（应为 0） */
        printf("[LINK] FRAME ok=%lu bad=%lu over=%lu dropped=%u defer=%lu\r\n",
               (unsigned long)esp_frame_ok, (unsigned long)esp_frame_bad,
               (unsigned long)esp_frame_over, (unsigned)frame_drop_cnt,
               (unsigned long)esp_frame_defer_line);
    }

    /*   ：给 OTA 的两个"卡住"收尾 —— 屏上那句提示不能永远挂着，
     *    否则用户分不清"还在等"还是"已经黄了"（这正是这次现场踩到的：屏上写着提示，
     *    按了键没反应，人只能猜）。
     *     两个判据都**必须**带 `!IAP_IsBusy()`：擦/写/校验/提交期间 CPU 本来就忙，
     *       那不是卡住。 停滞窗口取 20s（> IAP_JUMP_TIMEOUT_MS=15s）：成功那条路
     *       在 JUMP 之前就重启了，屏上根本来不及闪出这句 ⇒ 不会误报成功为"停了"。 */
    if (ota_total != 0u)
    {
        if (ota_last_ms == 0u) /* 阶段一：还在等用户按 K-UP */
        {
            /* ota_req_ms is set while this same poll is dispatching the
             * REQUEST, after `now` was sampled.  Do not let unsigned underflow
             * turn a new request into an immediate timeout. */
            if ((IAP_IsBusy() == 0u) &&
                ((int32_t)(now - ota_req_ms) >= (int32_t)AILINK_OTA_WAIT_MS))
            {
                Media_SetCaption("OTA REQUEST CANCELLED\nNO KEY; NOTHING DONE");
                ota_total = 0u;
                ota_req_ms = 0u;
                ota_pct = 0u;
                ota_paint_ms = 0u;
                mark_draw();
                printf("[LINK] OTA request expired: no K-UP within %lums\r\n",
                       (unsigned long)AILINK_OTA_WAIT_MS);
            }
        }
        else /* 阶段二：已经在收数据 —— 停了就是真停了 */
        {
            if ((IAP_IsBusy() == 0u) &&
                ((int32_t)(now - ota_last_ms) >= (int32_t)AILINK_OTA_STALL_MS))
            {
                Media_SetCaption("OTA STOPPED\nCHECK PC + ESP LOG");
                ota_total = 0u;
                ota_req_ms = 0u;
                ota_last_ms = 0u;
                ota_pct = 0u;
                ota_paint_ms = 0u;
                mark_draw();
                printf("[LINK] OTA stalled: no 0x10 for %lums (see PC tool + ESP DBG)\r\n",
                       (unsigned long)AILINK_OTA_STALL_MS);
            }
        }
    }

    /*  静默窗到点 → 告诉 main.c 该重画 AI 页了。
     *     这里**只返回页号、不自己画**：main.c 收到后会调 show_page()，
     *      在那里面画。自己画一遍再让它画一遍 = 白刷一次屏（150~300ms）。 */
    if ((ai_due_ms != 0u) && ((int32_t)(now - ai_due_ms) >= 0) &&
        ((int32_t)(now - esp_last_rx_ms) >= (int32_t)AILINK_SETTLE_MS))
    {
        ai_due_ms = 0u;
        if (ai_need_draw != 0u)
        {
            ai_need_draw = 0u;
            return PAGE_AI;
        }
    }

    return PAGE_KEEP;
}

void AILink_Init(void)
{
    uint8_t i;
    memset(&timeline, 0, sizeof(timeline));
    memset(&pet, 0, sizeof(pet));
    pet_face_saved = 0u;
    pose_due = GetTick_ms();
    pet_seq = 0u;
    timeline_paint_wait = 0u;
    for (i = 0u; i < 16u; i++)
    {
        srv_min[i] = 0u;
        srv_max[i] = 180u;
        srv_front[i] = 90u;
    }
    srv_min[0] = 75u;
    srv_max[0] = 105u;
    srv_nod = 1u;
    srv_yaw = 2u;
    srv_revision = 0u;
    srv_mask = 0u;
    srv_home_pending = 0xFFFFu;
    srv_home_speed = 0xFFFFu;
    ServoMotion_Init(GetTick_ms());
    ai_due_ms = 0u;
    ai_need_draw = 0u;
    st_len = 0u;
    st_pos = 0u;
    st_due_ms = 0u;
    diag_ms = GetTick_ms();
    ailink_ok_cnt = ailink_drop_cnt = 0;
    frame_drop_cnt = 0;

    /* OTA 屏上进度的状态（BSS 本来也是 0，显式写一遍免得将来加了软复位
     * 路径后在旧状态上接着算 —— 那些值只对"本次上电这一笔 OTA"有意义）。 */
    ota_total = ota_req_ms = ota_last_ms = ota_paint_ms = 0u;
    ota_pct = 0u;
    ota_cap[0] = 0;

    printf("[LINK] baud=921600, RX ring=1024, text lines + binary frames\r\n");
    printf("[LINK] ESP downlink whitelist (trailing '#' optional):\r\n");
    printf("[LINK]   @MOVE:servo,<1~16>,<0~180>[,<0~360 deg/s>]\r\n");
    printf("[LINK]   @MOVE:home[,all|<id>[,<speed>]] -> saved front angles\r\n");
    printf("[LINK]   @MOVE:gesture,<nod|hi|look_left|look_right|shake>\r\n");
    printf("[LINK]   @MOVE:expression,<happy|sad|sleepy|angry|normal>\r\n");
    printf("[LINK]   @TEXT:<text, commas allowed, never split>\r\n");
    printf("[LINK]   @SWEEP:servo,<1~16>  @LED_ON,LED<1|2>  @LED_OFF,LED<1|2>\r\n");

    printf("[LINK]   @MIC:start  @MIC:stop  @MIC:rec,<1~99 夹到 10>  (lowercase only)\r\n");
    /* ===  === */
    /* ===  === */
    printf("[LINK]   @IAP:<INFO|ABORT|JUMP|COMMIT|RESUME,<n>>  (02OTA 的段，原样转交)\r\n");
    /* ===  === */
    printf(
        "[LINK] Binary frames: 0x20 timeline / 0x21 stop / 0x22 servo limits; audio/IAP unchanged\r\n");
    printf("[LINK] Everything else is DROPPED (never fed to Media_Command).\r\n");
}
