/* ============================================================================
 * iap_app.h —— App 侧升级状态机：收固件 → 写 Slot B → 验整包 CRC → 写元数据 → 等重启
 *
 * ★ 分工（`规划部分/IAP布局.md` §5 / §6）
 *   传输（ESP↔PC 的 WiFi 段、以及 `ESP_UART_SendFrame()` 那个封装）**全归 01语音**。
 *   本模块**不碰 WiFi、不碰 USART6 寄存器**，只通过 .c 里那个 `iap_tx()` 单一出口发帧
 *   ⇒ 后端一换只改一处（见 `IAP_TX_BACKEND`）。
 *
 * ★ 本模块**全程不擦 Slot A**，只擦 Slot B(S6+S7)
 *   ⇒ 任何时刻 `@IAP:ABORT` 或掉电，老固件都完好 ⇒ **中止永远安全**。
 *
 * ★★ 谁该用 `IAP_IsBusy()` —— IAP 非空闲时，别的模块要**拒**、不要排队：
 *   - `@MOVE:` / `@TEXT:` 等   → 丢弃 + 回 `@IAP:ERR,7#`   （IAP布局 §6.2-1）
 *      ★ 2026-09-21 订正：这里原来写的是 `@IAP:ERR,BUSY` —— 那是**实现里不存在的写法**。
 *        本模块唯一的错误出口是 `iap_fail()`，它只会发 `@IAP:ERR,%u#`；忙码 = `IAP_EC_BUSY`(7)
 *        ⇒ 线上真实字符串是 `@IAP:ERR,7#`。**别去 grep `ERR,BUSY`，全库一个都没有。**
 *      ★ 且「拒绝」这个动作**不在本模块里**：是 01语音 的 ailink 分发先调 `IAP_IsBusy()`，
 *        为真时当场拦下、根本不转给本模块。本模块只管状态机，不做互斥、不主动回忙。
 *   - `@OTA_REQ`（云端来）      → 无条件回 `NO`                （§6.2-2）
 *   - `@MIC:start`             → 拒（安全联锁）                （§6.2-4）
 *     理由：擦写那 1~4 秒 CPU 被硬件拉停（F4 单 bank，擦写期间取指停在总线上）
 *           ⇒ 麦克风 64 ms 的 DMA 环**必溢出**、音频静默缺块，录音会话就毁了。
 * ========================================================================= */
#ifndef __IAP_APP_H
#define __IAP_APP_H

#include "stm32f4xx.h"

/* ---- 状态（与 `IAP布局.md §6.1` 的迁移图一致）---- */
#define IAP_S_IDLE      0u
#define IAP_S_ERASE     1u
#define IAP_S_DATA      2u
#define IAP_S_VERIFY    3u
#define IAP_S_COMMIT    4u
#define IAP_S_DONE      5u

/* ---- 错误码（走 `@IAP:ERR,<code>`）---- */
#define IAP_EC_SIZE     1u   /* 整包超过 224 KB 硬上限 —— ★ 必须在 BEGIN 就拒，不能等收完 */
#define IAP_EC_CRC      2u   /* 整包 CRC32 不一致 */
#define IAP_EC_SEQ      3u   /* 块序号错序 */
#define IAP_EC_STATE    4u   /* 当前状态不接受这条命令 */
#define IAP_EC_FLASH    5u   /* 擦 / 写失败 */
#define IAP_EC_ARG      6u   /* 参数不合法 */
#define IAP_EC_BUSY     7u   /* IAP 期间别的命令撞进来 */

/* ---- 二进制帧 type（= `合作表 §4.3`；`0x10`~`0x1F` 是本模块的段）---- */
#define IAP_T_DATA      0x10u   /* ESP→F407  固件数据块（裸二进制） */
#define IAP_T_ACK       0x11u   /* F407→ESP  第 <seq> 块收下（流控窗口） */
#define IAP_T_NAK       0x12u   /* F407→ESP  该块重发 */
#define IAP_T_META      0x13u   /* ESP→F407  总长 / 整包 CRC32 / 目标槽 */
#define IAP_T_STATE     0x14u   /* F407→ESP  槽状态 / 已收字节 / 进度 */
#define IAP_T_CTRL      0x15u   /* 双向      payload[0] = BEGIN/END/VERIFY/COMMIT/ABORT/RESUME */

/* ---- 0x15 的子命令 ---- */
#define IAP_C_BEGIN     1u
#define IAP_C_END       2u
#define IAP_C_VERIFY    3u
#define IAP_C_COMMIT    4u
#define IAP_C_ABORT     5u
#define IAP_C_RESUME    6u

/* `0x13` META 的 payload（**小端**，本模块自己定的，因为 0x10~0x1F 是 02 的段）：
 *   total(4) | crc32(4) | slot(1) | ver(2) | rsv(1)   = 12 字节
 * ★ `slot` 现在只有一个合法值（Slot B）；留着是为了将来扩 A/B 双槽。 */
#define IAP_META_PAYLOAD_LEN  12u

/* ---- `0x10` 单块 payload 上限（`IAP布局 §5.4`：**不是硬上限**，上板按丢块计数调）---- */
#define IAP_CHUNK_MAX        128u

/* ---- 进 DONE 之后等 `@IAP:JUMP#` 的上限；0 = 不自动复位（调试期可设 0）---- */
#define IAP_JUMP_TIMEOUT_MS  15000u

/* ---- 先把回话发出去、**下一圈**再擦；否则用户看不到回话，界面静默 1~4 秒 ---- */
#define IAP_ERASE_DELAY_MS   50u

void        IAP_AppInit(void);              /* 上电调一次：自检 + 打印当前槽状态 */
void        IAP_AppTick(uint32_t now);     /* 主循环每圈调：擦除延后 / DONE 超时 */
uint8_t     IAP_IsBusy(void);              /* 1 = IAP 正在进行，别的命令该拒 */
uint8_t     IAP_AppCommand(const char *line);   /* 认 `@IAP:` 文本帧；1 = 已消费 */
uint8_t     IAP_ConfirmRequest(void);           /* K-UP 确认已显示的 OTA 申请；1 = 本次按键被消费 */
uint8_t     IAP_RequestWaiting(void);          /* Read-only: physical approval prompt pending. */
void        IAP_OnFrame(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len);
const char *IAP_StateName(void);
uint32_t    IAP_Got(void);
uint32_t    IAP_AppBase(void);   /* 本镜像的**链接基址**（从链接器符号算出来，见 .c §③）*/

/* ---------------------------------------------------------------------------
 * TODO（第一次上板要做的两件事，都是**一次就能定**的）
 * 1. `IAP_META` 里的 `a_crc` 现在写 0 = "未知"（回滚判据暂无基线）。
 *    要填真值需要 App 镜像范围 `Image$$ER_IROM1$$Base .. $$Limit`。
 *    `$$Base` 已验证能链接（`User/system_stm32f4xx.c` 在用），**`$$Limit` 没试过** ——
 *    所以没敢引用。上板确认能链接后填上即可（改 `.c` §⑤ do_commit 一处）。
 * 2. `IAP_CHUNK_MAX` 取 128 是**保守值，不是硬件上限**（`IAP布局 §5.4`）。
 *    上板后按**丢块计数**调大，别凭感觉。
 * ------------------------------------------------------------------------- */

#endif /* __IAP_APP_H */
