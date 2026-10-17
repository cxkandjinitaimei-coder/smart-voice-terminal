/**
 * @file w25q.h
 * 用途：W25Q SPI NOR 的识别、容量、页写和扇区擦除。
 * 使用：SPI 初始化后先读 JEDEC，再使用有边界检查的读写接口；字库和图片层依赖此驱动。
 * 时序与边界：页写不可跨页，擦除前等空闲；该存储是板外器件，不属于 F407 的 A/B OTA 槽。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __W25Q_H
#define __W25Q_H

#include "stm32f4xx.h"
#include <stdint.h>

/* ---------- 外设 / 引脚 / 时钟（要移植到别的板子，只改这一段） ---------- */
#define W25Q_SPI SPI1
#define W25Q_SPI_CLK RCC_APB2Periph_SPI1 /*  SPI1 在 APB2 */
#define W25Q_GPIO_PORT GPIOB
#define W25Q_GPIO_CLK RCC_AHB1Periph_GPIOB /*  GPIOB 在 AHB1 */
#define W25Q_PIN_CS GPIO_Pin_0             /* 软件片选 */
#define W25Q_PIN_SCK GPIO_Pin_3
#define W25Q_PIN_MISO GPIO_Pin_4
#define W25Q_PIN_MOSI GPIO_Pin_5
#define W25Q_SRC_SCK GPIO_PinSource3 /*  单脚下标，不是掩码 */
#define W25Q_SRC_MISO GPIO_PinSource4
#define W25Q_SRC_MOSI GPIO_PinSource5
#define W25Q_AF GPIO_AF_SPI1                   /* AF5 */
#define W25Q_PRESCALER SPI_BaudRatePrescaler_8 /* 84MHz/8 = 10.5MHz */

/* ---------- 命令 ---------- */
#define W25Q_CMD_WRITE_ENABLE 0x06u
#define W25Q_CMD_READ_SR1 0x05u
#define W25Q_CMD_READ_SR2 0x35u
#define W25Q_CMD_READ_SR3 0x15u
#define W25Q_CMD_READ_DATA 0x03u
#define W25Q_CMD_PAGE_PROGRAM 0x02u
#define W25Q_CMD_SECTOR_ERASE 0x20u /* 4KB */
#define W25Q_CMD_BLOCK_ERASE 0xD8u  /* 64KB */
#define W25Q_CMD_JEDEC_ID 0x9Fu

/* ---------- 器件尺寸 ---------- */
#define W25Q_PAGE_SIZE 256u
#define W25Q_SECTOR_SIZE 4096u
#define W25Q_BLOCK_SIZE 65536u

/* 3 字节地址能覆盖到的上限（16MB）。读/写/擦的地址一律不得越这条线。 */
#define W25Q_ADDR_MAX 0x01000000u

/* ---------- 超时哨兵（循环计数，与 tick 无关） ----------
 * 每次循环要真调一次 SPI_I2S_GetFlagStatus（工程是 Keil Level 0 优化，
 * 不做内联），单次约 0.2~0.4us，所以别按"1 周期 = 6ns"估。
 *   BYTE：最坏等 1 个字节 @10.5MHz = 762ns ⇒ 给 10000 次 ≈ 3ms 上限，余量上千倍。
 *   BUSY：**必须按 64KB 块擦的 800ms 上限定**（4KB 扇区擦只有 400ms，别拿它当顶）。
 *         一次轮询 = 发 0x05 + 收 1 字节 + 软件开销 ≈ 3~8us ⇒ 800ms 需要约 16 万次
 *         ⇒ 给 1000000（1e6），约 6 倍余量。
 *  必须是 uint32_t。用 uint16_t 的话上限 65535 ≈ 0.3s，会在一次正常的
 *    64KB 块擦中途误判超时，然后你可能对还在 BUSY 的器件继续发命令
 *    —— 那些命令会被器件**静默丢弃**。
 *  这两个数是按"当前时钟树 + 当前优化级别"推的；换 Keil 版本或换 -O 级别
 *    要重新校一次。宁可大、不可小。 */
#define W25Q_TIMEOUT_BYTE 10000u
#define W25Q_TIMEOUT_BUSY 1000000u

/* ---------- 返回码（本模块自己的口径，不是库里任何东西的返回值） ---------- */
#define W25Q_OK 0u
#define W25Q_ERR_NOCHIP 1u /* JEDEC ID 不是 0xEF 开头 ⇒ 没芯片/没接好/引脚没切 AF */
#define W25Q_ERR_TIMEOUT 2u /* 等 SPI 标志或等 BUSY 超时 */
#define W25Q_ERR_PARAM 3u   /* 地址越界 / 长度 0 / 页编程跨页 */
#define W25Q_ERR_BUSY 4u    /* 本模块正被另一处调用（重入被拒） */
#define W25Q_ERR_VERIFY 5u  /* 写后回读不一致（只有读写自检会返回它） */

/* ---------- 实测容量（JEDEC ID 第 3 字节查表得来，不写死） ---------- */
#define W25Q_CAP_UNKNOWN 0u

/* ==================== 对外接口 ==================== */

/* 初始化：时钟 + CS(PB0) + SCK/MISO/MOSI 切 AF5 + SPI1 + 读 0x9F 回读验证。
 *  判据是"ID 读得回来"，不是"配置写进去了"（配置永远成功，写错了也不报错）。
 * 返回 0 = OK；1 = 没认到 Winbond ID；2 = 超时。 */
uint8_t W25Q_Init(void);

/* 最近一次 W25Q_Init() 读到的 JEDEC ID（3 字节，失败时全 0）。
 * 板载若是 W25Q16，期望 EF 40 15。返回的是内部静态数组，别改它。 */
const uint8_t *W25Q_ID(void);

/* 由 JEDEC ID 第 3 字节解出的容量（字节）。认不出来返回 W25Q_CAP_UNKNOWN(0)。 */
uint32_t W25Q_CapacityBytes(void);

/* 读。addr 为 24 位片内地址，len 任意长（内部按字节流读完）。
 * 返回 0 = OK；2 = 超时；3 = 地址越界；4 = 重入。 */
uint8_t W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len);

/* 页编程。len 必须 <= 256 且**不得跨页**（跨页会被器件回卷到页首覆盖，不报错！）。
 * 本函数自己判 (addr & 0xFF) + len <= 256，不满足直接返回 3。
 * 返回 0 = OK 且器件已写完（内部等 BUSY 清零）；2/3/4 同上。 */
uint8_t W25Q_PageProgram(uint32_t addr, const uint8_t *buf, uint16_t len);

/* 擦一个 4KB 扇区（0x20）。addr 落在这个扇区里即可。
 * 返回 0 = OK 且擦完（内部等 BUSY 清零）；2/3/4 同上。 */
uint8_t W25Q_SectorErase(uint32_t addr);

/* 擦一个 64KB 块（0xD8）。 最长要 800ms，调用方别放在会超时的时序里。 */
uint8_t W25Q_BlockErase(uint32_t addr);

/* 读状态寄存器：which = 1/2/3 对应 SR1(0x05)/SR2(0x35)/SR3(0x15)。
 * SR1 bit0 = BUSY、bit1 = WEL，查这两样最常用。
 * 返回 0 = OK；2 = 超时；3 = which 不是 1/2/3；4 = 重入。 */
uint8_t W25Q_ReadStatus(uint8_t which, uint8_t *out);

/* 等器件把写/擦周期跑完（轮询 SR1 bit0 到 0）。带超时。
 * 返回 0 = 空闲；2 = 超时（器件可能还在忙，**别继续发命令**）；4 = 重入。 */
uint8_t W25Q_WaitBusy(void);

/* 置写使能位（0x06）。单独用它的场合很少 —— PageProgram / 擦除已内置。 */
uint8_t W25Q_WriteEnable(void);

/* ==================== 上电自检（实现在 w25q_selftest.c） ==================== */
/*  只读，不擦不写 —— 每次上电都擦写等于白磨损 + 平白引入风险。
 * 返回 0 = 芯片在线且 ID 认得出；非 0 = W25Q_Init 的错误码。 */
uint8_t W25Q_SelfTest(void);

/* 擦一个扇区 → 写 → 回读比对。**只在串口手敲时调，绝不放进上电自检。**
 *  会破坏 0 号扇区的内容。返回 0 = 全对；否则是错误码。 */
uint8_t W25Q_SelfTestRW(void);

#endif /* __W25Q_H */
