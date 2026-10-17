/**
 * @file sd.h
 * 用途：原生 SDIO 一位总线的 SD 卡识别与扇区读写。
 * 使用：PC12=CK/PD2=CMD/PC8=D0；先低速完成卡初始化，再提速，由 diskio 适配 FatFs。
 * 时序与边界：扇区为 512 字节；卡类型决定地址单位，所有等待有超时。读写自测会写回原扇区内容。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __SD_H
#define __SD_H

#include "stm32f4xx.h"

/* ==================================================================
 * SD 卡驱动（SDIO 模式，默认 1 位宽，全轮询无中断）
 *
 * 硬件接法（见工程根目录 SD.png：卡座是直接连到 SDIO 信号的，没有接普通 GPIO）
 *   卡座 5 脚  SCK → PC12 = SDIO_CK
 *   卡座 3 脚  CMD → PD2  = SDIO_CMD
 *   卡座 7 脚  D0  → PC8  = SDIO_D0
 *   卡座 8 脚  D1  → PC9  = SDIO_D1     （只有开 4 位模式才用）
 *   卡座 1 脚  D2  → PC10 = SDIO_D2     （只有开 4 位模式才用）
 *   卡座 2 脚  D3  → PC11 = SDIO_D3     （只有开 4 位模式才用；SPI 模式下它是 CS）
 *   卡座 4 脚  Vcc → 3.3V     6 脚 GND
 *   R51~R55 = 五颗 100K 上拉；内部上拉补强。
 *   工作时钟从 24MHz 开始，启动读比对失败则降到 12/6MHz，不改变接线。
 *   卡座没有 CD（卡检测）脚 → 靠 CMD8/ACMD41 等应答判断；CMD0 本身无应答
 *   SDIO 六根线是 F407 的 AF12 固定引脚，**不可改**，只能从板上绕
 *
 *  冲突说明（本工程最需要注意的一条）：
 *   · PC8  = SDIO_D0 ↔ HCSR04 的 ECHO（TIM3_CH3）
 *     → 用 SD 卡时**不能** HCSR04_Init()，并且要把超声波模块从板子上**拔掉**：
 *       模块的 ECHO 是推挽输出，会顶住 SDIO 数据线，卡直接认不到。
 *   · PC9  = SDIO_D1 ↔ KEYPAD 的 COL0（**只有开 4 位模式才碰到**，默认 1 位不用管）
 *   · PC12 / PD2 / PC10 / PC11 本工程其它模块都没占用
 *
 * 时钟：SDIOCLK = PLL48CK = 48MHz（本工程 HSE 8M → PLL_M8/N336/Q7）
 *   SDIO_CK = 48MHz / (ClockDiv + 2)
 * ================================================================== */

/* 同步 zProject/03 已通过真机的原生 SDIO 修复。
 * 本板 LCD=FSMC NE1/A18，不与 SDIO 重叠；这里只提供扇区读取，没有 FatFs。
 */

/* ---------- 引脚绑定（改硬件只改这里） ---------- */
#define SD_GPIO_CLK (RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOD)
#define SD_SDIO_CLK RCC_APB2Periph_SDIO
#define SD_GPIO_AF GPIO_AF_SDIO

#define SD_CK_PORT GPIOC
#define SD_CK_PIN GPIO_Pin_12

#define SD_CMD_PORT GPIOD
#define SD_CMD_PIN GPIO_Pin_2

#define SD_D0_PORT GPIOC
#define SD_D0_PIN GPIO_Pin_8

/* 4 位模式开关：默认 0（1 位，最稳、只占 3 根线）
 * 改 1 会多占 PC9/PC10/PC11，其中 PC9 和 KEYPAD 的 COL0 冲突（键盘不能同时用） */
#ifndef SD_BUS_WIDTH_4BIT
#define SD_BUS_WIDTH_4BIT 0
#endif
#if SD_BUS_WIDTH_4BIT
#define SD_D1_PORT GPIOC
#define SD_D1_PIN GPIO_Pin_9
#define SD_D2_PORT GPIOC
#define SD_D2_PIN GPIO_Pin_10
#define SD_D3_PORT GPIOC
#define SD_D3_PIN GPIO_Pin_11
#endif

/* ---------- 速率（CLKCR.ClockDiv） ---------- */
#define SD_INIT_CLK_DIV 118   /* 48MHz/(118+2) = 400kHz，SD 规范要求上电必须 ≤400kHz */
#define SD_TRANSFER_CLK_DIV 0 /* 24MHz; init verifies x3, falls back to 12/6MHz */

/* ---------- 返回值 ---------- */
#define SD_OK 0
#define SD_ERR_NOCARD 1   /* 卡未完成初始化 */
#define SD_ERR_CMD8 2     /* 命令阶段失败（CMD8 回显不符 / CMD16 设块长失败） */
#define SD_ERR_ACMD41 3   /* ACMD41 一直 busy：卡不认 / 电压不合 / 坏卡 */
#define SD_ERR_VOLTAGE 4  /* ACMD41 OCR 与主机电压窗口不匹配 */
#define SD_ERR_TIMEOUT 5  /* 命令、数据或等卡就绪超时 */
#define SD_ERR_READ 6     /* 命令/数据 CRC 错、FIFO 溢出、数据不完整 */
#define SD_ERR_PARAM 7    /* 入参非法 */
#define SD_ERR_RESPONSE 8 /* R1/R6 卡状态错误、应答号或 CSD 格式错误 */

/* ---------- 对外接口 ---------- */
/* 原生 SDIO：CMD0/8/ACMD41 → CMD2/3/9/7/13 → 低速读验证 → 工作速率读验证。
 * SD_OK 仅在容量有效、选卡成功且两次读一致后返回。不要求扇区带 55AA 签名。 */
u8 Sd_Init(void);

/* 读扇区：内部就是反复单块读（CMD17），一次一个扇区。
 * 不做 CMD18 多块 + CMD12 停止：FatFs 读文件时每回只传 1 块，多块读用不上，
 * 少一条命令就少一处出错的地方。cnt 只是方便连续读几个扇区。 */
u8 Sd_Read_Sector(u32 sector, u8 *buf, u16 cnt);

/* 写扇区：同理，反复单块写（CMD24），每条后面等卡把内部 flash 写完（CMD13）。
 * buf 不要求 4 字节对齐（内部用 memcpy 组装字）。 会真的改卡上内容。 */
u8 Sd_Write_Sector(u32 sector, const u8 *buf, u16 cnt);

/* 写能力自测：**非破坏性** —— 拿卡上最后一个扇区写图案、回读比对、再把
 * 原内容写回去比对。返回 SD_OK 才说明这块卡的写通路真的可用。
 *  会 printf ⇒ 必须在 UART_Init() 和 Sd_Init() 之后调。
 *  写通路牵涉卡的内部编程时间，比读慢一到两个数量级（毫秒级/扇区）；
 *    这是自测，只在启动时跑一次，不要放进主循环。 */
u8 Sd_WriteSelfTest(void);

/* 卡信息 */
u32 Sd_Get_Sector_Count(void);      /* 容量，单位 512 字节扇区 */
const char *Sd_Get_Card_Name(void); /* "SDHC/SDXC" / "SDSC v2" / "SDSC v1" / "MMC" / "无卡" */
void Sd_Get_Card_Info(void);        /* 串口打印卡信息（调试/实验报告用） */

#endif /* __SD_H */
