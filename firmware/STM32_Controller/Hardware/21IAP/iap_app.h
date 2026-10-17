/**
 * @file iap_app.h
 * 用途：应用侧 OTA 请求确认、下载槽写入、CRC 校验和提交状态机。
 * 使用：Init 后命令/帧入口接受请求，实体按键确认后允许 BEGIN；Tick 执行延后擦除及状态推进。
 * 时序与边界：只写 Slot B，完整 CRC 正确才写 READY 元数据；Bootloader 重启后搬到 A。协议回执需发 ESP 而非只 printf。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __IAP_APP_H
#define __IAP_APP_H

#include "stm32f4xx.h"

/* ---- 状态（与 `docs/flash-and-ota.md` 的迁移图一致）---- */
#define IAP_S_IDLE 0u
#define IAP_S_ERASE 1u
#define IAP_S_DATA 2u
#define IAP_S_VERIFY 3u
#define IAP_S_COMMIT 4u
#define IAP_S_DONE 5u

/* ---- 错误码（走 `@IAP:ERR,<code>`）---- */
#define IAP_EC_SIZE 1u /* 整包超过 224 KB 硬上限 ——  必须在 BEGIN 就拒，不能等收完 */
#define IAP_EC_CRC 2u   /* 整包 CRC32 不一致 */
#define IAP_EC_SEQ 3u   /* 块序号错序 */
#define IAP_EC_STATE 4u /* 当前状态不接受这条命令 */
#define IAP_EC_FLASH 5u /* 擦 / 写失败 */
#define IAP_EC_ARG 6u   /* 参数不合法 */
#define IAP_EC_BUSY 7u  /* IAP 期间别的命令撞进来 */

#define IAP_T_DATA 0x10u  /* ESP→F407  固件数据块（裸二进制） */
#define IAP_T_ACK 0x11u   /* F407→ESP  第 <seq> 块收下（流控窗口） */
#define IAP_T_NAK 0x12u   /* F407→ESP  该块重发 */
#define IAP_T_META 0x13u  /* ESP→F407  总长 / 整包 CRC32 / 目标槽 */
#define IAP_T_STATE 0x14u /* F407→ESP  槽状态 / 已收字节 / 进度 */
#define IAP_T_CTRL 0x15u  /* 双向      payload[0] = BEGIN/END/VERIFY/COMMIT/ABORT/RESUME */

/* ---- 0x15 的子命令 ---- */
#define IAP_C_BEGIN 1u
#define IAP_C_END 2u
#define IAP_C_VERIFY 3u
#define IAP_C_COMMIT 4u
#define IAP_C_ABORT 5u
#define IAP_C_RESUME 6u

/* `0x13` META 的 payload（**小端**，本模块自己定的，因为 0x10~0x1F 是 02 的段）：
 *   total(4) | crc32(4) | slot(1) | ver(2) | rsv(1)   = 12 字节
 *  `slot` 现在只有一个合法值（Slot B）；留着是为了将来扩 A/B 双槽。 */
#define IAP_META_PAYLOAD_LEN 12u

/* ---- `0x10` 单块 payload 上限（`docs/flash-and-ota.md`：**不是硬上限**，上板按丢块计数调）---- */
#define IAP_CHUNK_MAX 128u

/* ---- 进 DONE 之后等 `@IAP:JUMP#` 的上限；0 = 不自动复位（调试期可设 0）---- */
#define IAP_JUMP_TIMEOUT_MS 15000u

/* ---- 先把回话发出去、**下一圈**再擦；否则用户看不到回话，界面静默 1~4 秒 ---- */
#define IAP_ERASE_DELAY_MS 50u

void IAP_AppInit(void);                   /* 上电调一次：自检 + 打印当前槽状态 */
void IAP_AppTick(uint32_t now);           /* 主循环每圈调：擦除延后 / DONE 超时 */
uint8_t IAP_IsBusy(void);                 /* 1 = IAP 正在进行，别的命令该拒 */
uint8_t IAP_AppCommand(const char *line); /* 认 `@IAP:` 文本帧；1 = 已消费 */
uint8_t IAP_ConfirmRequest(void); /* K-UP 确认已显示的 OTA 申请；1 = 本次按键被消费 */
uint8_t IAP_RequestWaiting(void); /* Read-only: physical approval prompt pending. */
void IAP_OnFrame(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len);
const char *IAP_StateName(void);
uint32_t IAP_Got(void);
uint32_t IAP_AppBase(void); /* 本镜像的**链接基址**（从链接器符号算出来，见 .c §）*/

/* ---------------------------------------------------------------------------
 * TODO（第一次上板要做的两件事，都是**一次就能定**的）
 * 1. `IAP_META` 里的 `a_crc` 现在写 0 = "未知"（回滚判据暂无基线）。
 *    要填真值需要 App 镜像范围 `Image$$ER_IROM1$$Base .. $$Limit`。
 *    `$$Base` 已验证能链接（`User/system_stm32f4xx.c` 在用），**`$$Limit` 没试过** ——
 *    所以没敢引用。上板确认能链接后填上即可（改 `.c` § do_commit 一处）。
 * 2. `IAP_CHUNK_MAX` 取 128 是**保守值，不是硬件上限**（`docs/flash-and-ota.md`）。
 *    上板后按**丢块计数**调大，别凭感觉。
 * ------------------------------------------------------------------------- */

#endif /* __IAP_APP_H */
