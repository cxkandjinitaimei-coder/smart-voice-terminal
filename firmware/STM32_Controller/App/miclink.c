/**
 * @file miclink.c
 * 用途：麦克风触发、自动断句和上传忙状态的应用层协调器。
 * 使用：Init 后周期 Tick；VAD 使用最新 DMA 半块 RMS，网页请求只置位，实际开录由前台处理。
 * 时序与边界：自动录音连续越阈值后起录、连续静音后结束；手动录音保留静音。上传完成再经过安静间隔重武装。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "camera_link.h"

#include "miclink.h"
#include "esp_uart.h" /* MicLink_SubmitUserText -> @USER: 帧 */
#include "ailink.h"
#include "media.h"  /* Media_SetFace / Media_DrawAIPage / Media_GetFace */
#include "micrec.h" /* MicRec_Command —— 录音复用现成的 /rec /dump */

#include "font_upload.h"
#include "micaud.h"  /* MicAud_Start/Tick/IsStreaming —— 非阻塞流式上行 */
#include "inmp441.h" /* VAD：每个 32ms DMA 半块的 RMS */
#include "iap_app.h"
#include "../System/time.h" /* GetTick_ms: avoid ARMCC's standard time.h */
/* ===  === */
#include <stdio.h>
#include <string.h>

#define MICREQ_NONE 0u
#define MICREQ_START 1u

#define MICLINK_ST_IDLE 0u  /* 没在录 */
#define MICLINK_ST_ARMED 1u /* 已武装：底部状态更新，下一圈开录 */
#define MICLINK_ST_REC 2u   /* 正在阻塞录音（阻塞期内没有别人在跑） */
/*  流式录音是**非阻塞**的，状态要跨很多圈主循环 —— 所以它必须
 *   是一个独立状态，不能像 RAM 版那样"进了函数立刻回 IDLE"。 */
#define MICLINK_ST_STREAM 3u /* 流式进行中：每圈 MicAud_Tick() 推一块 */

static volatile uint8_t s_req = MICREQ_NONE;
static volatile uint16_t s_req_sec = MICLINK_SEC_DEFAULT;
static uint8_t s_state = MICLINK_ST_IDLE;
static uint8_t s_vad_fast_start;
static uint32_t s_vad_last_block;
static uint16_t s_vad_voice_halves;
static uint16_t s_vad_silent_halves;
static uint32_t s_vad_rearm_at;
static uint32_t s_vad_report_at;
static uint32_t s_vad_floor;
static uint16_t s_vad_settle_halves;
static uint8_t s_upload_busy;
static uint8_t s_vad_enabled = 1u;
static uint16_t s_vad_minimum = MICLINK_VAD_RMS_MIN;
static uint16_t s_vad_margin = MICLINK_VAD_FLOOR_MARGIN;
static uint16_t s_vad_start_halves = MICLINK_VAD_START_HALVES;
static uint16_t s_vad_end_halves = MICLINK_VAD_END_HALVES;

/* 录完自动 /dump（P1 的"上传"这一段就是复用现成的 hex dump 通路）。
 *  代价要知道：2 秒录音 = 128000 个十六进制字符，115200 下线上要吐约 11 秒，
 *   这 11 秒同样是**阻塞**的，期间 1024 字节的 ESP 环没人取。
 *   改成 0 就退回"人手工敲 /dump"的老流程。 */
#define MICLINK_AUTO_DUMP 1u

static void miclink_do_record(uint16_t seconds)
{
    char cmd[16];

    printf("[MICLINK] 开录 %u 秒（19MICREC RAM 版，阻塞；此间不取 ESP 帧）\r\n", (unsigned)seconds);

    /* 直接借用现成的串口命令入口，不另写一套录音循环 —— 19MICREC 那个阻塞
     * 循环是为了"一个样本都不丢"（DMA 环只有 64ms），别在这儿重造一遍。 */
    sprintf(cmd, "/rec %u", (unsigned)seconds);
    (void)MicRec_Command(cmd);

#if MICLINK_AUTO_DUMP
    (void)MicRec_Command("/dump");
#endif
}
/* ===  === */

/* ------------------------------------------------------------------
 * 触发源桩。现在全是空的 —— 这一层只是"该开录了吗"的判定入口。
 * ------------------------------------------------------------------ */
static void trig_none(uint32_t now)
{
    (void)now;
    /* MIC_TRIG_SRC == NONE：只认串口的 /rec，这里永远什么都不做。 */
}

static void trig_button(uint32_t now)
{
    (void)now;
    /* TODO(按键触发)：KEY_UP 现在是**短按切页**（main.c 主循环），
     *   直接复用会和切页打架 ⇒ 得先改成"短按切页 / 长按 ≥500ms 说话"。
     *   接口在这：MicLink_SpeechBegin() / MicLink_SpeechEnd()。 */
}

static void miclink_arm(uint16_t seconds, uint8_t vad)
{
    if (s_state != MICLINK_ST_IDLE)
        return;

    AILink_UserActivity(); /* Wake before the recorder snapshots the face. */
    s_req_sec = seconds;
    s_vad_fast_start = vad;
    s_state = MICLINK_ST_ARMED;
    printf("[MICLINK] %s -> 开始监听，最长 %u 秒\r\n", vad ? "VAD 人声" : "@MIC:start",
           (unsigned)seconds);
}

/* The microphone's idle RMS varies greatly with supply noise and wiring.
 * Calibrate it first, then follow it slowly so spoken audio cannot suddenly
 * raise the trigger point. */
static void vad_update_floor(uint32_t rms, uint8_t settling)
{
    if (s_vad_floor == 0u)
    {
        s_vad_floor = rms;
        return;
    }
    if ((settling != 0u) || (rms < s_vad_floor))
        s_vad_floor = (s_vad_floor * 7u + rms) / 8u;
    else
        s_vad_floor += (rms - s_vad_floor) / 32u + 1u;
}

static uint32_t vad_start_threshold(void)
{
    uint32_t threshold = s_vad_floor + s_vad_margin;
    uint32_t proportional = s_vad_floor + s_vad_floor / 2u;
    if (proportional > threshold)
        threshold = proportional;
    if (threshold < s_vad_minimum)
        threshold = s_vad_minimum;
    return threshold;
}

static uint32_t vad_hold_threshold(void)
{
    uint32_t threshold = s_vad_floor + s_vad_margin / 3u;
    if (threshold < s_vad_minimum)
        threshold = s_vad_minimum;
    return threshold;
}

/* VAD 按 DMA 完成次数更新，不能因主循环跑得快把同一半块累计成多次连续人声。 */
static void trig_vad(uint32_t now)
{
    uint32_t blocks;
    uint32_t rms;
    uint32_t start_threshold;
    uint32_t hold_threshold;

    blocks = INMP441_Blocks();
    if (blocks == s_vad_last_block)
        return;
    s_vad_last_block = blocks;
    rms = INMP441_Rms();

    if (s_state == MICLINK_ST_STREAM)
    {
        /* The webpage's fixed-duration recording must not run silence VAD. */
        if (s_vad_fast_start == 0u || MicAud_IsCapturing() == 0u)
            return;
        hold_threshold = vad_hold_threshold();
        if (rms >= hold_threshold)
        {
            s_vad_silent_halves = 0u;
            return;
        }
        if (s_vad_silent_halves < 0xFFFFu)
            s_vad_silent_halves++;
        if (s_vad_silent_halves >= s_vad_end_halves &&
            MicAud_CapturedBlocks() >= MICLINK_VAD_MIN_BLOCKS)
        {
            if (MicAud_RequestSentenceEnd() != 0u)
            {
                printf("[MICLINK] VAD 静音 %u x32ms，短句正常收尾\r\n",
                       (unsigned)s_vad_silent_halves);
                s_vad_rearm_at = now + MICLINK_VAD_REARM_MS;
            }
        }
        return;
    }

    if ((s_state != MICLINK_ST_IDLE) || (s_req != MICREQ_NONE) || (s_upload_busy != 0u) ||
        (s_vad_enabled == 0u) || IAP_IsBusy() || FontUpload_Active() ||
        ((int32_t)(now - s_vad_rearm_at) < 0))
        return;

    if (s_vad_settle_halves < MICLINK_VAD_SETTLE_HALVES)
    {
        vad_update_floor(rms, 1u);
        s_vad_settle_halves++;
        s_vad_voice_halves = 0u;
        if ((int32_t)(now - s_vad_report_at) >= 0)
        {
            s_vad_report_at = now + 1000u;
            printf("[MICLINK] VAD calibrating: rms=%lu floor=%lu %u/%u\r\n", (unsigned long)rms,
                   (unsigned long)s_vad_floor, (unsigned)s_vad_settle_halves,
                   (unsigned)MICLINK_VAD_SETTLE_HALVES);
        }
        return;
    }

    vad_update_floor(rms, 0u);
    start_threshold = vad_start_threshold();

    /* Idle-only diagnostic: it never prints while audio is streaming. */
    if ((int32_t)(now - s_vad_report_at) >= 0)
    {
        s_vad_report_at = now + 1000u;
        printf("[MICLINK] VAD waiting: rms=%lu floor=%lu start=%lu hold=%lu consecutive=%u/%u\r\n",
               (unsigned long)rms, (unsigned long)s_vad_floor, (unsigned long)start_threshold,
               (unsigned long)vad_hold_threshold(), (unsigned)s_vad_voice_halves,
               (unsigned)s_vad_start_halves);
    }
    if (rms >= start_threshold)
    {
        if (s_vad_voice_halves < 0xFFFFu)
            s_vad_voice_halves++;
        if (s_vad_voice_halves >= s_vad_start_halves)
        {
            s_vad_voice_halves = 0u;
            s_vad_silent_halves = 0u;
            miclink_arm(MICLINK_VAD_MAX_SEC, 1u);
        }
    }
    else
    {
        s_vad_voice_halves = 0u;
    }
}

static void trig_esp(uint32_t now)
{
    (void)now;

    if (s_req == MICREQ_NONE)
    {
        return;
    }
    s_req = MICREQ_NONE;

    if (s_state != MICLINK_ST_IDLE)
    {
        /* 正常进不来（录音是阻塞的，这一圈不可能还留着上一圈的请求）。
         * 留一句让人看得见的话，别静默丢。 */
        printf("[MICLINK] @MIC:start 被丢：上一次还没收尾\r\n");
        return;
    }

    miclink_arm(s_req_sec, 0u);
}

static void mic_trig_dispatch(uint32_t now)
{
    /*  写成 switch 而不是 #if：让三个桩**始终被引用**（见文件头）。
     *   现在 MIC_TRIG_SRC == NONE ⇒ 只走 default，不碰任何硬件。 */
    switch (MIC_TRIG_SRC)
    {
    case MIC_TRIG_SRC_BUTTON:
        trig_button(now);
        break;
    case MIC_TRIG_SRC_VAD:
        trig_vad(now);
        trig_esp(now);
        break;
    case MIC_TRIG_SRC_ESP:
        trig_esp(now);
        break;
    default:
        trig_none(now);
        break;
    }
}

/* ==================================================================
 * 一次"说话"的生命周期。以后三种触发源都从这里进出，别各自复制一套。
 * 公开（非 static）是有意的：让"预留接口"是**可调用**的，而不是一堆注释。
 * ================================================================== */
void MicLink_SpeechBegin(uint32_t now)
{
    (void)now;
    /* TODO: 调 19MICREC 开录。接口在 Hardware\19MICREC\micrec.h。
     *  两个已知的坑，真做的时候必须先解决：
     *    **录音期间整段阻塞主循环**（micrec.c 文件头解释过）。这期间
     *      AILink 取不了 ESP 帧、按键也扫不到 ⇒ 5 帧连发会被 256 字节
     *      的环丢掉一部分。要么"录音时暂停取帧并告知云端"，要么把录音
     *      改成分块。**不能装作没这回事。**
     *    录制时长受 s_buf[] 限制（见 miclink.h 的 MICLINK_SEC_MAX_PLANNED）。 */
}

void MicLink_SpeechEnd(void)
{
    /* TODO: 停录 → 拿到音频 → （本地不可能 ASR）→ 得上云。
     *  **音频怎么上云是个大工程**，不是本包的事：ESP 现在只会发
     *    JSON/HTTP 文本（@USER:/@SENSOR:），**没有音频通路**。
     *    ⇒ 现实的过渡做法是先把这个接口用在"文本"上：UI/按键拿到一句文本
     *      （哪怕是固定的），直接 MicLink_SubmitUserText() 送出去，
     *      先把"用户意图 → 云端 → 舵机/表情"这条闭环跑通，
     *      音频识别作为**替换掉文本来源**的最后一步再上。 */
}

/* ==================================================================
 * 上云
 * ================================================================== */
uint8_t MicLink_SubmitUserText(const char *text)
{
    char frame[128];
    size_t n;

    if ((text == 0) || (text[0] == 0))
    {
        return 0u;
    }

    n = strlen(text);
    /* 截断而不是溢出：缓冲区大小自己算，别指望 caller 递进来的串有多短。 */
    if (n > (sizeof(frame) - 10u))
    {
        n = sizeof(frame) - 10u;
    }

    memcpy(frame, "@USER:", 6u);
    memcpy(frame + 6u, text, n);
    frame[6u + n] = '\r';
    frame[6u + n + 1u] = '\n';
    frame[6u + n + 2u] = 0;

    ESP_UART_SendStr(frame);

    /*  只打长度不打内容：正文可能是中文/任意 UTF-8，串口终端编码一变
     *    就是花屏（这是本项目已经已知问题）。 */
    printf("[MICLINK] sent @USER (%u bytes)\r\n", (unsigned)n);
    return 1u;
}

/* ==================================================================
 * @MIC: 三条帧的请求入口（只有 App\ailink.c 的 do_mic() 会调）
 * ================================================================== */
void MicLink_RequestStart(uint16_t seconds)
{
    if (FontUpload_Active())
    {
        printf("[MICLINK] busy: font upload active\r\n");
        return;
    }
    if (s_upload_busy && s_state == MICLINK_ST_IDLE)
    {
        printf("[MICLINK] busy: previous audio still uploading\r\n");
        return;
    }
    if (seconds == 0u)
    {
        seconds = MICLINK_SEC_DEFAULT;
    }

    /* 夹到上限，而不是当格式错拒收：云端多要几秒是**正常**的，不该罚掉整条命令。
     *  上限从"RAM 版缓冲的 2 秒"变成 P3 流式的 10 秒
     *   （见 miclink.h 的 MICLINK_SEC_MAX_PLANNED）。 */
    if (seconds > MICLINK_SEC_MAX_PLANNED)
    {
        seconds = MICLINK_SEC_MAX_PLANNED;
    }

    /*  流式正在跑时**不收新请求**：覆盖 s_req 会把正在录的那段弄丢，
     *   而用户真正想做的是"等它录完"或"先 @MIC:stop"。 */
    if (s_state == MICLINK_ST_STREAM)
    {
        printf("[MICLINK] @MIC:rec,%u 被忽略：流式还在录（先等它录完，或 @MIC:stop）\r\n",
               (unsigned)seconds);
        return;
    }

    s_req_sec = seconds;
    s_req = MICREQ_START;
}

void MicLink_OnAudioStatus(uint8_t busy)
{
    uint8_t next = (busy != 0u) ? 1u : 0u;
    if (next == s_upload_busy)
        return;
    s_upload_busy = next;
    s_vad_voice_halves = 0u;
    if (next == 0u)
        s_vad_rearm_at = GetTick_ms() + MICLINK_VAD_REARM_MS;
    printf("[MICLINK] ESP audio %s; VAD %s\r\n", next ? "upload busy" : "upload done",
           next ? "paused" : "rearm after quiet gap");
}

void MicLink_RequestStop(void)
{
    if (s_state == MICLINK_ST_ARMED)
    {
        /* 还没开录，撤回请求；页面始终保留。 */
        s_req = MICREQ_NONE;
        s_state = MICLINK_ST_IDLE;
        printf("[MICLINK] @MIC:stop -> 撤回本次录音，页面保留\r\n");
        return;
    }

    if (s_state == MICLINK_ST_STREAM)
    {
        /*  流式是**非阻塞**的，所以这里**停得下来**了 ——
         *   正是当初留这个落点等的那个版本。中止 ⇒ MicAud_Abort() 会尽力
         *   把 `0x03` 对账帧发出去，PC 侧拿到的是**残件**（会落盘、但不送识别）。 */
        MicAud_Abort(MICAUD_EC_NONE);
        s_state = MICLINK_ST_IDLE;
        printf("[MICLINK] @MIC:stop -> 中止流式录音（已确认 %u 块），页面保留\r\n",
               (unsigned)MicAud_SentBlocks());
        return;
    }

    if (s_state == MICLINK_ST_REC)
    {
        /* 走不到这里：RAM 版录音是阻塞的，进了 miclink_do_record() 就没人能
         * 执行到这一句。留着是为了万一以后 RAM 版也改成非阻塞。 */
        printf("[MICLINK] @MIC:stop 无效：正在录，RAM 版停不下来\r\n");
        return;
    }

    printf("[MICLINK] @MIC:stop：没有待录的请求，忽略\r\n");
}

uint8_t MicLink_ConfigureVad(uint8_t enabled, uint16_t minimum, uint16_t margin, uint16_t onset_ms,
                             uint16_t silence_ms)
{
    if (enabled > 1u || minimum < 50u || minimum > 8000u || margin > 8000u || onset_ms < 64u ||
        onset_ms > 640u || silence_ms < 200u || silence_ms > 2016u)
        return 0u;
    if (enabled == s_vad_enabled && minimum == s_vad_minimum && margin == s_vad_margin &&
        (onset_ms + 31u) / 32u == s_vad_start_halves &&
        (silence_ms + 31u) / 32u == s_vad_end_halves)
        return 1u;
    if (MicLink_IsRecording() || IAP_IsBusy())
        return 0u;
    s_vad_enabled = enabled;
    s_vad_minimum = minimum;
    s_vad_margin = margin;
    s_vad_start_halves = (uint16_t)((onset_ms + 31u) / 32u);
    s_vad_end_halves = (uint16_t)((silence_ms + 31u) / 32u);
    s_vad_voice_halves = s_vad_silent_halves = 0u;
    s_vad_rearm_at = GetTick_ms() + MICLINK_VAD_REARM_MS;
    printf("[MICLINK] CONFIG auto=%u minimum=%u margin=%u start=%ums silence=%ums\r\n",
           (unsigned)enabled, (unsigned)minimum, (unsigned)margin,
           (unsigned)(s_vad_start_halves * 32u), (unsigned)(s_vad_end_halves * 32u));
    return 1u;
}

uint8_t MicLink_IsRecording(void)
{
    /*  流式这一段**外面观察得到**了（它是非阻塞的，主循环一直在跑）。
     *   所以现在这个函数可以拿来做"录音时禁用按钮"这类逻辑了 —— 但只在
     *   n > 2 的流式路径上返回 1；RAM 版那 1~2 秒仍然是阻塞的、外面看不到。 */
    return (s_state != MICLINK_ST_IDLE || s_req != MICREQ_NONE) ? 1u : 0u;
}

void MicLink_GetVadInfo(MicLink_VadInfo *info)
{
    info->enabled = s_vad_enabled;
    info->minimum = s_vad_minimum;
    info->margin = s_vad_margin;
    info->onset_ms = (uint16_t)(s_vad_start_halves * 32u);
    info->silence_ms = (uint16_t)(s_vad_end_halves * 32u);
    info->threshold = vad_start_threshold();
}

const char *MicLink_AudioStateName(void)
{
    if (s_state == MICLINK_ST_ARMED)
        return s_vad_fast_start ? "AUTO:ARMED" : "MANUAL:ARMED";
    if (s_state == MICLINK_ST_STREAM)
    {
        if (!strcmp(MicAud_StateName(), "WAIT_ESP"))
            return "WAIT ESP READY";
        if (!MicAud_IsCapturing())
            return "DRAINING AUDIO";
        return s_vad_fast_start ? "AUTO:RECORDING" : "MANUAL:RECORDING";
    }
    if (s_state == MICLINK_ST_REC)
        return "MANUAL:RAM REC";
    if (s_upload_busy)
        return "UPLOADING";
    if ((int32_t)(GetTick_ms() - s_vad_rearm_at) < 0)
        return "QUIET GAP";
#if MIC_TRIG_SRC == MIC_TRIG_SRC_VAD
    return !s_vad_enabled ? "MANUAL READY"
                          : (s_vad_settle_halves < MICLINK_VAD_SETTLE_HALVES ? "CALIBRATING"
                                                                             : "WAITING FOR VOICE");
#else
    return "IDLE";
#endif
}

void MicLink_Init(void)
{
    s_req = MICREQ_NONE;
    s_req_sec = MICLINK_SEC_DEFAULT;
    s_state = MICLINK_ST_IDLE;
    s_vad_fast_start = 0u;
    s_vad_enabled = 1u;
    s_vad_minimum = MICLINK_VAD_RMS_MIN;
    s_vad_margin = MICLINK_VAD_FLOOR_MARGIN;
    s_vad_start_halves = MICLINK_VAD_START_HALVES;
    s_vad_end_halves = MICLINK_VAD_END_HALVES;
    s_vad_rearm_at = 0u;
    ESP_UART_SendStr("@CONFIG:NEED\n");
    s_upload_busy = 0u;
    s_vad_last_block = INMP441_Blocks();
    s_vad_report_at = 0u;
    s_vad_floor = 0u;
    s_vad_settle_halves = 0u;
    s_vad_voice_halves = 0u;
    s_vad_silent_halves = 0u;
    printf("[MICLINK] trigger source = %s\r\n",
           (MIC_TRIG_SRC == MIC_TRIG_SRC_BUTTON) ? "BUTTON (reserved)"
           : (MIC_TRIG_SRC == MIC_TRIG_SRC_VAD)  ? "VAD auto sentence + ESP manual"
           : (MIC_TRIG_SRC == MIC_TRIG_SRC_ESP)  ? "ESP (@MIC: frames)"
                                                 : "NONE - serial /rec only, unchanged");
    if ((MIC_TRIG_SRC == MIC_TRIG_SRC_ESP) || (MIC_TRIG_SRC == MIC_TRIG_SRC_VAD))
    {
        printf(
            "[MICLINK] 认这三条下行帧（白名单在 App\\ailink.c::do_mic，小写，尾缀 '#' 可有可无）：\r\n");
        printf("[MICLINK]   @MIC:start      录 %u 秒（默认）\r\n", (unsigned)MICLINK_SEC_DEFAULT);
        printf("[MICLINK]   @MIC:rec,<n>    n = 1~%u（超了夹到上限）分两条路：\r\n",
               (unsigned)MICLINK_SEC_MAX_PLANNED);
        printf(
            "[MICLINK]     n <= %u  走 19MICREC 的 RAM 版（**阻塞**录完再 dump，行为与改造前一致）\r\n",
            (unsigned)MICLINK_SEC_RAM_MAX);
        printf(
            "[MICLINK]     n >  %u  走 20MICAUD 的**流式**版（非阻塞，边录边发 -> ESP -> PC 流式 POST）\r\n",
            (unsigned)MICLINK_SEC_RAM_MAX);
        printf(
            "[MICLINK]     ⚠️ 流式期间**屏面暂停刷新**（保护 64ms 的 DMA 环），别当成死机\r\n");
        printf("[MICLINK]   @MIC:stop       撤回待录请求或中止流式录音\r\n");
        printf("[MICLINK] 录音保留当前页面，底部显示录音/上传/等待状态\r\n");
#if MICLINK_AUTO_DUMP
        printf(
            "[MICLINK] 录完自动 /dump（hex 文本，2 秒约吐 11 秒，PC 端 Tools\\micrec\\dump2wav.py 收）\r\n");
#else
        printf("[MICLINK] 录完**不**自动 dump：要人手工敲 /dump\r\n");
#endif
    }
    if (MIC_TRIG_SRC == MIC_TRIG_SRC_VAD)
    {
        printf("[MICLINK] VAD: 自适应底噪；校准=%u半块，起录下限=%u，裕量=%u\r\n",
               (unsigned)MICLINK_VAD_SETTLE_HALVES, (unsigned)s_vad_minimum,
               (unsigned)s_vad_margin);
        printf("[MICLINK] VAD: 起录连续=%u半块；静音=%u半块；最短=%u块；最长=%u秒\r\n",
               (unsigned)s_vad_start_halves, (unsigned)s_vad_end_halves,
               (unsigned)MICLINK_VAD_MIN_BLOCKS, (unsigned)MICLINK_VAD_MAX_SEC);
    }
}

/* 状态推进与屏幕反馈分离；流式收尾后仍要等待 ESP 上传状态解除 busy 才允许下一句。 */
uint8_t MicLink_Tick(uint32_t now)
{
    if (CameraLink_Busy())
    {
        s_vad_voice_halves = 0u;
        s_req = MICREQ_NONE;
        return 0u;
    }
    /* Execute the previous loop's armed request; only the status row changed. */
    if (s_state == MICLINK_ST_ARMED)
    {
        if (s_req_sec <= MICLINK_SEC_RAM_MAX)
        {
            /* ---- 老路：RAM 阻塞版，**逐字未动**（n ≤ 2 秒的行为跟改造前一样）---- */
            s_state = MICLINK_ST_REC;
            miclink_do_record(s_req_sec);
            s_state = MICLINK_ST_IDLE;

            printf("[MICLINK] 录音结束，页面保留\r\n");
        }
        else
        {
            /* ---- P3 新路：流式（**非阻塞**，从这里开始由 b 每圈推进）----
             * 收尾由下面的流式状态轮询完成。 */
            if (MicAud_Start(s_req_sec, s_vad_fast_start ? MICAUD_F_VAD_SHORT : 0u) != 0u)
            {
                s_state = MICLINK_ST_STREAM;
                printf("[MICLINK] mode=%s, max=%us -> 流式上传；%s\r\n",
                       s_vad_fast_start ? "AUTO" : "MANUAL", (unsigned)s_req_sec,
                       s_vad_fast_start ? "按已设置静音时长断句" : "固定时长，保留静音");
            }
            else
            {
                /* Rejected: report the reason and restore the idle status row. */
                s_state = MICLINK_ST_IDLE;
                printf("[MICLINK] 流式开录被拒：%s\r\n", MicAud_ErrName());
            }
        }
    }

    /* 流式进行中每圈推进一块，结束后状态行恢复等待/上传。 */
    if (s_state == MICLINK_ST_STREAM)
    {
        (void)MicAud_Tick();
        if (MicAud_IsStreaming() == 0u)
        {
            if (s_vad_fast_start != 0u)
                printf(
                    "[MICLINK] AUTO end: silent=%u/%u, hold=%lu, floor=%lu, captured=%u blocks\r\n",
                    (unsigned)s_vad_silent_halves, (unsigned)s_vad_end_halves,
                    (unsigned long)vad_hold_threshold(), (unsigned long)s_vad_floor,
                    (unsigned)MicAud_CapturedBlocks());
            s_state = MICLINK_ST_IDLE;
            s_vad_fast_start = 0u;
            printf("[MICLINK] 流式结束（%u 块已确认 / 丢半块 %u / 重传 %u），页面保留\r\n",
                   (unsigned)MicAud_SentBlocks(), (unsigned)MicAud_LostHalf(),
                   (unsigned)MicAud_Retries());
        }
    }

    /*  再收这一圈的新请求（只武装，不录）。
     *     流式进行中不收新请求（否则会把正在跑的这段覆盖掉）——
     *      这一点 MicLink_RequestStart 那边也拦了一道，这里是第二道。 */
    mic_trig_dispatch(now);

    /* Recording preserves the current page, face and AI caption.
     * main.c updates only a bounded one-line audio banner, including during
     * streaming. Never load missing ANGRY/NORMAL BMPs for mic transitions. */
    return PAGE_KEEP;
}
