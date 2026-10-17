/**
 * @file miclink.h
 * 用途：麦克风触发、自动断句和上传忙状态的应用层协调器。
 * 使用：Init 后周期 Tick；VAD 使用最新 DMA 半块 RMS，网页请求只置位，实际开录由前台处理。
 * 时序与边界：自动录音连续越阈值后起录、连续静音后结束；手动录音保留静音。上传完成再经过安静间隔重武装。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __MICLINK_H
#define __MICLINK_H

#include "stm32f4xx.h"
#include "media.h" /* 要 PAGE_AI / PAGE_KEEP（跟 ailink.h 同一个理由） */

/* ==================================================================
 * 麦克风「触发 → 录制 → 上云」的预留接口
 * ------------------------------------------------------------------
 *  本模块**不碰** Hardware\18INMP441 和 Hardware\19MICREC 一个字节 ——
 *   那两个现在是好的（/rec /dump /mic 都能跑，见 main.c 的分发链）。
 *   这里只把"以后要改的地方"集中到**一个宏**上。
 *
 * 用户原话："麦克风这一块要容易修改的那种，因为后面是要修改触发条件的
 *            和录制时长的" ⇒ 触发条件 = 下面的 MIC_TRIG_SRC 一处；
 *            录制时长 = 见 MICLINK_SEC_MAX_PLANNED 上面的警告。
 * ================================================================== */

/* ---------- 触发源开关 ---------- */
#define MIC_TRIG_SRC_NONE 0u   /* 只认串口的 /rec —— **现在的行为** */
#define MIC_TRIG_SRC_BUTTON 1u /* 预留：按键触发（按住说话） */
#define MIC_TRIG_SRC_VAD 2u    /* 音量过门限自动触发 */
#define MIC_TRIG_SRC_ESP 3u    /* ESP 下发命令触发 */

/* 默认 VAD：检测到人声自动开录，静音后自动断句。ESP 的 @MIC:rec 仍保留，
 * 因而调试时可继续手动录稳定的 10 秒。 */
#define MIC_TRIG_SRC MIC_TRIG_SRC_VAD

/* VAD 每个麦克风半块采样一次（约 32ms）。阈值取自实板：静音约 26~100，
 * 正常说话上千。以下值是集中调参入口，勿散落在 .c 里。 */
#define MICLINK_VAD_SETTLE_HALVES 64u /* 上电后约 2 秒只校准、不自动起录 */
#define MICLINK_VAD_RMS_MIN 900u      /* 无论底噪多低，也不低于此值 */
#define MICLINK_VAD_FLOOR_MARGIN 800u /* 起录至少高出底噪这么多 */
#define MICLINK_VAD_START_HALVES 5u   /* 约 160ms 连续有声才起录 */
#define MICLINK_VAD_END_HALVES 25u    /* 约 800ms 连续静音即断句 */
#define MICLINK_VAD_MIN_BLOCKS 10u    /* 0.64 秒，太短的噪声不送识别 */
#define MICLINK_VAD_MAX_SEC 10u       /* 仍保留最长句保护 */
#define MICLINK_VAD_REARM_MS 1000u    /* 收句/上传后留 1 秒安静间隔 */

/* ---------- 录制时长上限 ----------
 *  ：2 → **10**（P3 流式）。但**两条路的物理上限完全不同**：
 *
 *   n ≤ MICLINK_SEC_RAM_MAX（= 2 秒）：走 19MICREC 的 RAM 版，**阻塞**录完再 dump。
 *     上限是 `micrec.c` 的 `s_buf[MICREC_SAMPLES_MAX]` —— 2 秒就吃掉主 SRAM
 *     (128KB) 的 64000 字节（map 里 ZI-data 的绝大头）。**这条路一点没变**。
 *   n >  2 秒：走 `20MICAUD` 的**流式**版（非阻塞，边录边发 0x02 帧 → ESP →
 *     PC 的流式 POST）。上限不再是 s_buf，而是**主循环能不能跟上**：
 *     一块 2048 B 占线 22ms，而麦克风 DMA 环只有 64ms ⇒ 流式期间**不许干慢任务**
 *     （main.c 里已让路：SD 重读、屏面刷新）。默认上限 10 秒跑稳。
 *
 *  还是那句：**改这个宏不会让 s_buf 变大**。想在 RAM 版里录更久，只有
 *    "动内存布局"或"做 SD 写通路（本工程 FatFs 是 _FS_READONLY=1、没有
 *    disk_write）"两条路，都不便宜，本包不承诺。 */
#define MICLINK_SEC_MAX_PLANNED 10u
#define MICLINK_SEC_RAM_MAX 2u /* ≤ 它走 RAM 阻塞版；> 它走流式 */

void MicLink_Init(void);

/* 每圈推进VAD/录音状态。始终返回PAGE_KEEP，main.c单独更新底部状态行。
 * 录音不切页，不替换AI表情与字幕；参数通过MicLink_ConfigureVad调整。 */
uint8_t MicLink_Tick(uint32_t now);
void MicLink_SpeechBegin(uint32_t now); /* 一次"说话"的开始（预留） */
void MicLink_SpeechEnd(void);           /* 一次"说话"的结束（预留） */

/* 把一句用户文本交给 ESP → 云端。 **这条路径现在是通的，不是桩**：
 *   ESP 侧 ESP_Gateway.ino:530 认 "@USER:"，把它当"用户消息"上报云端，
 *   云端走 DeepSeek 决策后把 moves[] 发回来，最终变成 @MOVE: 帧回到 AILink。
 * 返回 1 = 已发出；0 = 空串不发。 */
uint8_t MicLink_SubmitUserText(const char *text);

#define MICLINK_SEC_DEFAULT 2u /* = 现在 RAM 版的上限，见 micrec.h:69 */

void MicLink_RequestStart(uint16_t seconds); /* @MIC:start / @MIC:rec,<秒> */
void MicLink_RequestStop(void);              /* @MIC:stop */
void MicLink_OnAudioStatus(uint8_t busy);    /* ESP upload busy/ready; gate automatic VAD */
uint8_t MicLink_IsRecording(void);           /* 1 = 正在录（阻塞期内为真） */
uint8_t MicLink_ConfigureVad(uint8_t enabled, uint16_t minimum, uint16_t margin, uint16_t onset_ms,
                             uint16_t silence_ms);
const char *MicLink_AudioStateName(void); /* LCD: mode / upload / rearm */
typedef struct
{
    uint16_t minimum, margin, onset_ms, silence_ms;
    uint32_t threshold;
    uint8_t enabled;
} MicLink_VadInfo;
/* Read-only snapshot; querying parameters never arms or starts recording. */
void MicLink_GetVadInfo(MicLink_VadInfo *info);

/*  录音时贴在表情框里的图（SD 卡 FACE\ 目录，8.3 短名）。
 *   用户红线：**只切预置图，不许在片上画图元、不许运行时生成图片**。
 *
 *  **临时值，不是最终值**：
 *   最终要的是 "rec"（= FACE\REC.BMP，由 Tools\micgen 生成），但**用户手边暂时
 *   没有 SD 读卡器**，REC.BMP 补不进卡里 ⇒ 填 "rec" 的话 bmp_blit 会失败，
 *   表情框只会变成一行报错串，什么也证明不了。
 *   ⇒ 临时借一个**卡上已有的**表情（angry）当"我在录"的指示：切图看得见，
 *     而且**真实付了 bmp_blit 那 150~300ms** —— 两段式的时序余量这才真验到了
 *     （加载失败几乎不花时间，正好验不出东西）。
 *    **拿到读卡器、把 REC.BMP 拷进 FACE\ 之后，把这里改回 "rec"。** */
#define MICLINK_REC_FACE "angry"

/*  录音期间贴在**下方文字区**的提示语。
 *    必须是纯 ASCII —— 屏上字库只有 ASCII(32~126) + 片内 32 个汉字，中文会
 *     画成 '?'。这句全是大写字母和点，能完整显示（`[CAP] missing=` 会是 0）。
 *    录完会**还原成录音前那一句**（miclink.c 的 s_cap_save），所以**不会把
 *     屏上原有的 AI 文字吃掉**。 */
#define MICLINK_LISTEN_TEXT "LISTENING..."
/* ===  === */

#endif /* __MICLINK_H */
