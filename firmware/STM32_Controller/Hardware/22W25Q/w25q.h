/* ==================================================================
 * 板载 W25Q SPI Flash 驱动 —— 纯芯片操作层
 * ------------------------------------------------------------------
 * 【本模块只干一件事】对这颗 Flash 发命令：读 ID / 读数据 / 页编程 /
 *   擦扇区 / 擦块 / 等忙。**不含"字库""文件""分区"任何概念** ——
 *   地址怎么用、哪一段放什么，由调用方自己定。
 *
 * 【接线】唯一依据 = 项目根 `引脚分配表.md`（§2，标 🔒 = 板载锁定、不可复用）
 *     CS   -> PB0    软件片选（★ 不能用硬件 NSS，理由见下）
 *     SCK  -> PB3    ⚠️ 同时是 JTDO
 *     MISO -> PB4    ⚠️ 同时是 NJTRST
 *     MOSI -> PB5
 *     VCC  -> 3.3V   ★ 不要给 5V
 *   外设 = **SPI1 的重映射组**（AF5）。
 *
 * 【为什么只能是这三根脚】SPI2 默认组 PB12-PB15 被触摸屏占满，备用组
 *   PB10/PC2/PC3 也只剩 PC2 一根、凑不齐 4 根；SPI3 的 PB3-PB5 与
 *   PC10-PC12 两边都被占。⇒ 全板就这一条通路能走。
 *
 * 【★★ PB3/PB4 的两个坑（本板历史上就卡在这，别重新踩）】
 *   1) 复位后 PB3=JTDO、PB4=NJTRST，属于 **AF0(JTAG)**。F4 上**没有**
 *      AFIO_MAPR，所以 `GPIO_Remap_SWJ_JTAGDisable` / `GPIO_Remap_SPI1`
 *      这两个宏**在本工程的 Lib 里根本不存在**（那是 F1 的写法，网上
 *      教程大量照抄，包括本项目的旧文档）。唯一正确做法：
 *          GPIO_PinAFConfig(GPIOB, GPIO_PinSourceN, GPIO_AF_SPI1)
 *      切到 AF5，PB3/PB4 就自动脱离 JTAG。代价是整条 JTAG-DP 废掉 ——
 *      这与 `引脚分配表.md §7`「本板 JTAG 已关闭（SWD-only）」的现状一致。
 *      ⚠️ Keil 里 Debug 端口必须选 **SW**：选 JTAG 的话，`W25Q_Init()`
 *         执行前能连上、执行后就连不上，症状极迷惑。
 *   2) `GPIO_PinSourceX` 是**引脚下标 0~15**，不是位掩码！写成
 *          GPIO_PinSource3 | GPIO_PinSource4 | GPIO_PinSource5
 *      ⇒ 3|4|5 = 7 ⇒ 改的是 **PB7**。而 PB7 = 本板 PCA9685 的 I2C1_SDA
 *      ⇒ **不报错、SPI1 一点没生效、还把 PCA9685 打死**，且
 *      `IS_GPIO_PIN_SOURCE(0x07)` 判定合法、连断言都拦不住。
 *      ⇒ 必须**三次单独调用**（见 w25q.c 的 w25q_gpio_af_init）。
 *
 * 【时钟 / 速率】APB2 = 84MHz，预分频 8 ⇒ SCK = 10.5MHz
 *   （W25Q 普通读 0x03 的上限是 50MHz，余量足够；想提速只改 W25Q_PRESCALER，
 *    ⚠️ 但换了时钟树就必须同步改这里）。
 *
 * 【依赖】★ **零依赖**：本文件与 w25q.c 只 include `stm32f4xx.h`，
 *   **不 printf、不 include `time.h`** —— 所有等待都用"循环计数 + 超时兜底"
 *   而不是 tick。理由有两条：① 见 `17PCA9685/pca9685.h` 那句"TIM6 万一没跑，
 *   tick 永不前进，拿 tick 当超时恰好在你最需要它兜底的时候失效"；
 *   ② 这样将来 bootloader（独立 Keil 工程）能复用同一份源码。
 *   ⚠️ 但"复用到 bootloader"不只差一行 IncludePath：`Bootloader/AIBootloader.uvprojx`
 *      的源文件清单里**没有 `stm32f4xx_spi.c`**，得先把 `..\Lib\src\stm32f4xx_spi.c`
 *      加进那个工程，否则 `SPI_Init()` 一族会**链接失败**。
 *   ⚠️ 也别给 bootloader 单独复制一份 `stm32f4xx_conf.h`：那个文件里有
 *      `#include "stm32f4xx_spi.h"`，分叉出去漏了它，本文件会以**隐式声明**
 *      编过（ARM 调用约定下常常"看着能跑"），极脆。
 *
 * 【打印 / 自检】printf 全在同目录的 `w25q_selftest.c`，**本文件一个字节都不打**。
 *   上电自检要调的是那边的 `W25Q_SelfTest()`（只读，不擦写）。
 * ================================================================== */
#ifndef __W25Q_H
#define __W25Q_H

#include "stm32f4xx.h"
#include <stdint.h>

/* ---------- 外设 / 引脚 / 时钟（要移植到别的板子，只改这一段） ---------- */
#define W25Q_SPI                SPI1
#define W25Q_SPI_CLK            RCC_APB2Periph_SPI1     /* ★ SPI1 在 APB2 */
#define W25Q_GPIO_PORT          GPIOB
#define W25Q_GPIO_CLK           RCC_AHB1Periph_GPIOB    /* ★ GPIOB 在 AHB1 */
#define W25Q_PIN_CS             GPIO_Pin_0              /* 软件片选 */
#define W25Q_PIN_SCK            GPIO_Pin_3
#define W25Q_PIN_MISO           GPIO_Pin_4
#define W25Q_PIN_MOSI           GPIO_Pin_5
#define W25Q_SRC_SCK            GPIO_PinSource3         /* ★ 单脚下标，不是掩码 */
#define W25Q_SRC_MISO           GPIO_PinSource4
#define W25Q_SRC_MOSI           GPIO_PinSource5
#define W25Q_AF                 GPIO_AF_SPI1            /* AF5 */
#define W25Q_PRESCALER          SPI_BaudRatePrescaler_8 /* 84MHz/8 = 10.5MHz */

/* ---------- 命令 ---------- */
#define W25Q_CMD_WRITE_ENABLE   0x06u
#define W25Q_CMD_READ_SR1       0x05u
#define W25Q_CMD_READ_SR2       0x35u
#define W25Q_CMD_READ_SR3       0x15u
#define W25Q_CMD_READ_DATA      0x03u
#define W25Q_CMD_PAGE_PROGRAM   0x02u
#define W25Q_CMD_SECTOR_ERASE   0x20u   /* 4KB */
#define W25Q_CMD_BLOCK_ERASE    0xD8u   /* 64KB */
#define W25Q_CMD_JEDEC_ID       0x9Fu

/* ---------- 器件尺寸 ---------- */
#define W25Q_PAGE_SIZE          256u
#define W25Q_SECTOR_SIZE        4096u
#define W25Q_BLOCK_SIZE         65536u

/* 3 字节地址能覆盖到的上限（16MB）。读/写/擦的地址一律不得越这条线。 */
#define W25Q_ADDR_MAX           0x01000000u

/* ---------- 超时哨兵（循环计数，与 tick 无关） ----------
 * 每次循环要真调一次 SPI_I2S_GetFlagStatus（工程是 Keil Level 0 优化，
 * 不做内联），单次约 0.2~0.4us，所以别按"1 周期 = 6ns"估。
 *   BYTE：最坏等 1 个字节 @10.5MHz = 762ns ⇒ 给 10000 次 ≈ 3ms 上限，余量上千倍。
 *   BUSY：**必须按 64KB 块擦的 800ms 上限定**（4KB 扇区擦只有 400ms，别拿它当顶）。
 *         一次轮询 = 发 0x05 + 收 1 字节 + 软件开销 ≈ 3~8us ⇒ 800ms 需要约 16 万次
 *         ⇒ 给 1000000（1e6），约 6 倍余量。
 * ⚠️ 必须是 uint32_t。用 uint16_t 的话上限 65535 ≈ 0.3s，会在一次正常的
 *    64KB 块擦中途误判超时，然后你可能对还在 BUSY 的器件继续发命令
 *    —— 那些命令会被器件**静默丢弃**。
 * ⚠️ 这两个数是按"当前时钟树 + 当前优化级别"推的；换 Keil 版本或换 -O 级别
 *    要重新校一次。宁可大、不可小。 */
#define W25Q_TIMEOUT_BYTE       10000u
#define W25Q_TIMEOUT_BUSY       1000000u

/* ---------- 返回码（本模块自己的口径，不是库里任何东西的返回值） ---------- */
#define W25Q_OK                 0u
#define W25Q_ERR_NOCHIP         1u      /* JEDEC ID 不是 0xEF 开头 ⇒ 没芯片/没接好/引脚没切 AF */
#define W25Q_ERR_TIMEOUT        2u      /* 等 SPI 标志或等 BUSY 超时 */
#define W25Q_ERR_PARAM          3u      /* 地址越界 / 长度 0 / 页编程跨页 */
#define W25Q_ERR_BUSY           4u      /* 本模块正被另一处调用（重入被拒） */
#define W25Q_ERR_VERIFY         5u      /* 写后回读不一致（只有读写自检会返回它） */

/* ---------- 实测容量（JEDEC ID 第 3 字节查表得来，不写死） ---------- */
#define W25Q_CAP_UNKNOWN        0u

/* ==================== 对外接口 ==================== */

/* 初始化：时钟 + CS(PB0) + SCK/MISO/MOSI 切 AF5 + SPI1 + 读 0x9F 回读验证。
 * ★ 判据是"ID 读得回来"，不是"配置写进去了"（配置永远成功，写错了也不报错）。
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

/* 擦一个 64KB 块（0xD8）。★ 最长要 800ms，调用方别放在会超时的时序里。 */
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
/* ★ 只读，不擦不写 —— 每次上电都擦写等于白磨损 + 平白引入风险。
 * 返回 0 = 芯片在线且 ID 认得出；非 0 = W25Q_Init 的错误码。 */
uint8_t W25Q_SelfTest(void);

/* 擦一个扇区 → 写 → 回读比对。**只在串口手敲时调，绝不放进上电自检。**
 * ★ 会破坏 0 号扇区的内容。返回 0 = 全对；否则是错误码。 */
uint8_t W25Q_SelfTestRW(void);

#endif /* __W25Q_H */
