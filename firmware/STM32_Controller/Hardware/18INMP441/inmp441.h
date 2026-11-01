#ifndef __INMP441_H
#define __INMP441_H

#include "stm32f4xx.h"   /* 标准库 */

/* ==================================================================
 * INMP441 数字麦克风（I2S 从机，STM32 做主机收）★ 2026-09-18 新增
 * ------------------------------------------------------------------
 * 实物：INMP441 模块，6 个脚 = VDD / GND / SCK / WS / SD / L-R。
 *       内部是 MEMS 麦克风 + 24 位 ADC + I2S 接口，输出**二进制补码、
 *       MSB 先出**的数字音频，不是模拟电压 ⇒ 不需要 ADC，也不会被
 *       STM32 的 ADC 噪声污染。
 *
 * ★★ 引脚（走 I2S2 的**备用脚组**，不是主用脚组）：
 *
 *   | 模块脚 | STM32 | 外设信号           | AF  | 方向 |
 *   |--------|-------|--------------------|-----|------|
 *   | SCK    | PB10  | I2S2_CK  位时钟    | AF5 | OUT  |
 *   | WS     | PB9   | I2S2_WS  声道选择  | AF5 | OUT  |
 *   | SD     | PC3   | I2S2_SD  数据      | AF5 | IN   |
 *   | L/R    | GND   | 接地 = 只驱动左声道 | —  | —    |
 *   | VDD    | 3.3V  | ⚠️ **只能 3.3V，接 5V 就烧** | — | — |
 *   | GND    | GND   | **必须与主板共地**  | —  | —    |
 *
 *   ★ 为什么是这组备用脚，而不是 I2S2 的"主用脚" PB12/PB13/PB15：
 *     PB12/PB13/PB14/PB15 + PC5 是板上**屏的触摸 SPI**
 *     （TCS/TSCK/MISO/MOSI/IRQ）。触摸驱动虽然还没写，但那是**屏占着的
 *     物理脚**，别去抢（详见 `引脚分配表.md` §5）。
 *   ★ 为什么 PB10 躲不掉（这是本模块**唯一**的代价，务必知道）：
 *     I2S2_CK 只有 **PB13 / PB10** 两个可能，PB13 归触摸 ⇒ **只剩 PB10**。
 *     ⇒ **PB10 一给麦克风，USART3 就废了**，而 `Hardware\14ESP_UART`
 *       用的正是 USART3(PB10/PB11) ⇒ **"麦克风"与"ESP 走 USART3"二选一**。
 *       好在 `esp_uart` 现在本来就没被 `main.c` 调用（ESP 实际走
 *       USART1 / PA9-PA10），所以现阶段没有实际损失 ——
 *       但**别同时**去调 `ESP_UART_Init()`，两边都不报错、只是麦克风没声。
 *   ★ 为什么不用 I2S3：I2S3_CK 只能 PB3/PC10，**PB3 是板载 W25Q16 的
 *     SPI1_SCK、PC10 是 SDIO_D2** ⇒ 两条路都被占死，用不了。
 *
 * ★ 顺序要求：`INMP441_Init()` 里会 printf ⇒ **必须在 `UART_Init()` 之后调**。
 * ================================================================== */

/* ---------- 外设 / 引脚 / 时钟 ---------- */
#define MIC_I2S            SPI2                     /* I2S2 就是 SPI2 外设（F4 上 I2S 由 spi.c 实现） */
#define MIC_I2S_CLK        RCC_APB1Periph_SPI2      /* ⚠️ I2S2/SPI2 挂在 **APB1** */
#define MIC_GPIO_CLK       (RCC_AHB1Periph_GPIOB | RCC_AHB1Periph_GPIOC)

#define MIC_CK_PORT        GPIOB                    /* PB10 = I2S2_CK */
#define MIC_CK_PIN         GPIO_Pin_10
#define MIC_CK_SRC         GPIO_PinSource10
#define MIC_WS_PORT        GPIOB                    /* PB9  = I2S2_WS */
#define MIC_WS_PIN         GPIO_Pin_9
#define MIC_WS_SRC         GPIO_PinSource9
#define MIC_SD_PORT        GPIOC                    /* PC3  = I2S2_SD */
#define MIC_SD_PIN         GPIO_Pin_3
#define MIC_SD_SRC         GPIO_PinSource3
#define MIC_AF             GPIO_AF_SPI2             /* I2S2/SPI2 的复用号 = AF5 */

/* INMP441_PinCheck() 的返回位。0 表示 MCU 端三根 I2S 线的配置正确；
 * 非 0 表示有引脚被别的外设改走，或 I2S/DMA 没处于预期状态。
 * 注意：它只能验证 MCU 寄存器，物理杜邦线是否插牢由随后 Diag 的数据变化判定。 */
#define MIC_PINCHK_PB10    (1u << 0)  /* PB10 不是 AF5 / I2S2_CK */
#define MIC_PINCHK_PB9     (1u << 1)  /* PB9  不是 AF5 / I2S2_WS */
#define MIC_PINCHK_PC3     (1u << 2)  /* PC3  不是 AF5 / I2S2_SD */
#define MIC_PINCHK_I2S     (1u << 3)  /* SPI2 没处于 I2S 主机接收 */
#define MIC_PINCHK_DMA     (1u << 4)  /* SPI2 RX DMA1 Stream3 未启用 */

/* ---------- DMA（SPI2_RX = DMA1_Stream3 / Channel0，见 RM0090 Table 42） ---------- */
#define MIC_DMA            DMA1_Stream3
#define MIC_DMA_CLK        RCC_AHB1Periph_DMA1      /* DMA1 在 AHB1，别写成 APB */
#define MIC_DMA_CHANNEL    DMA_Channel_0
#define MIC_DMA_IRQn       DMA1_Stream3_IRQn
#define MIC_DMA_IRQHandler DMA1_Stream3_IRQHandler  /* ★ 向量名必须与启动文件一致 */
/* ★★ 这里有两套长得极像、用途完全不同的宏，**混用不报错、只会静默失效**：
 *   ① `DMA_IT_HT/TC/TE`    = `DMA_SxCR` 里的**中断使能位**（0x08 / 0x10 / 0x04）
 *      ⇒ 只能给 `DMA_ITConfig()` 用。
 *   ② `DMA_IT_HTIF3/…`     = `LISR/HISR` 里的**中断标志掩码**（带寄存器偏移，0x14004000 这种）
 *      ⇒ 只能给 `DMA_GetITStatus()` / `DMA_ClearITPendingBit()` 用。
 *   ★★ 2026-09-18 踩过（上机第一次就这么挂的）：把 ② 传给了 `DMA_ITConfig()`，
 *      而它只是 `CR |= 值` ⇒ 三个标志掩码被按位或进 CR 后，**压在别的字段上**：
 *        · bit 25/26/27 = **CHSEL** ⇒ 通道被从 0 改成 **7**
 *          ⇒ SPI2_RX 的请求再也送不到这个流 ⇒ **DMA 一个字节都不搬**
 *        · bit 13/14 = **MSIZE** ⇒ 从"半字"变成"32 位字"
 *        · bit 15 = PINCOS ⇒ 被置 1（PINC=0 时无所谓，但白改）
 *        · 而真正想要的 HTIE/TCIE/TEIE（bit 3/4/2）**一个都没置上**
 *      ⇒ 现象：`半块计数` 恒 0、`NDTR` 也**不动**，和"I2S 没跑 / 线没接"
 *        一模一样 ⇒ 极容易误诊成接线或时钟问题（第一版 Diag 就误诊了）。
 *      ★ 教训：这类"标志掩码"和"使能位"是**两套值**，STM32 库里到处都是
 *        （I2C/ADC/USART 也这样），传错都不报错、只是静默失效。
 *   ★ S3 与 S7 共用 HT/TC 标志位，所以 ② 的宏名必须带 "_3"。 */
#define MIC_DMA_IT_EN      (DMA_IT_HT | DMA_IT_TC | DMA_IT_TE)   /* ← 给 DMA_ITConfig() */
#define MIC_DMA_IT_HT      DMA_IT_HTIF3                          /* ← 给 Get/ClearITPendingBit() */
#define MIC_DMA_IT_TC      DMA_IT_TCIF3
#define MIC_DMA_IT_TE      DMA_IT_TEIF3
#define MIC_DMA_FLAG_HT    DMA_FLAG_HTIF3
#define MIC_DMA_FLAG_TC    DMA_FLAG_TCIF3
#define MIC_DMA_FLAG_TE    DMA_FLAG_TEIF3           /* 传输错误 */
#define MIC_DMA_FLAG_DME   DMA_FLAG_DMEIF3          /* 直接模式错误 */
#define MIC_DMA_FLAG_FE    DMA_FLAG_FEIF3           /* FIFO 错误 */

/* ---------- 音频参数 ---------- */
#define MIC_SAMPLE_RATE    16000u   /* 16 kHz 单声道 —— 语音识别的事实标准采样率 */
#define MIC_HALF_SAMPLES   1024u    /* 半个 DMA 缓冲的**半字**数（= 512 个单声道样本 = 32ms） */
#define MIC_DMA_SAMPLES    (MIC_HALF_SAMPLES * 2u)  /* 环形缓冲共 1024 半字 = 64ms */

/* 左声道在 DMA 缓冲里的**下标奇偶性**：0 = 第 0 个半字就是左声道。
 * 为什么会有这个问题：I2S 一帧 = 左 16 位 + 右 16 位，DMA 收到的是
 * **左右交错**的流；而 L/R 接地 ⇒ 麦只在左槽驱动数据，右槽是三态。
 * I2S 主机使能时从 WS 低（左）开始，所以默认取 0。
 * ★ 真机若"有波形但音量不对/像隔着一层"，看 `INMP441_Diag()` 打出的
 *   实际判定值，对不上就改这个宏。`INMP441_Calibrate()` 会自动定标。 */
#define MIC_LEFT_PARITY    0u

/* ---------- I2S 时钟（PLLI2S）----------
 * I2S 的时钟**不是** PCLK1，而是独立的一个 PLL：PLLI2S。
 *   I2S 时钟 = (HSE / PLL_M) × PLLI2SN / PLLI2SR
 * 本板 HSE = 8MHz，`system_stm32f4xx.c` 里 PLL_M = 8 ⇒ PLLI2S 输入 1MHz
 *   258 × 1MHz / 3 = **86 MHz**
 * 然后库函数 `I2S_Init()` 会**自己去读 `RCC->PLLI2SCFGR` 和 `PLLCFGR`
 * 反算分频**：16kHz / 16 位 / 关 MCLK ⇒ I2SDIV=84、ODD=0
 *   实际 = 86e6 / (168 × 32) = **15997 Hz**（标称 16000，误差 −0.02%，听不出来）
 * ★★ 所以 **必须先配 PLLI2S、再调 I2S_Init()**；顺序反了它按旧值/0 算，
 *    采样率会跑到几十 kHz（现象："声音又尖又快"）。
 * ★★ 换 PLLI2SN/R 也行（如 271/2、344/5），但**必须**让 I2S_Init 反算出的
 *    I2SDIV 落在 2~255 内；越界时库会**静默**回落到 I2SDIV=2，一句警告都没有。
 *    `INMP441_Diag()` 会把实际寄存器值打出来，用来验这条。 */
#define MIC_PLLI2SN        258
#define MIC_PLLI2SR        3

/* ==================== 函数声明 ==================== */

/* 初始化（时钟 / 引脚 / DMA / 中断 / I2S）。返回 0 = 成功。
 * 可重复调用。★ 内部会 printf ⇒ 必须在 UART_Init() 之后调。 */
uint8_t INMP441_Init(void);

/* 读取实际 GPIO/I2S/DMA 寄存器并打印引脚自检。
 * 返回 MIC_PINCHK_* 的按位或；Init 在完成后会自动调用，无需手动触发。 */
uint8_t INMP441_PinCheck(void);

/* 启动 / 停止采集。Init 之后还需 Start 才会开始进数据。 */
void    INMP441_Start(void);
void    INMP441_Stop(void);

/* 自定标：等一个完整半块，判定左声道在缓冲里的奇偶位置（写进内部状态）。
 * 返回 0 = 定标完成，1 = 等不到数据（麦没接/没供电），此时保持默认奇偶。
 * ★ 建议 Init+Start 之后调一次；没接麦时它只是白等约 50ms。 */
uint8_t INMP441_Calibrate(void);

/* 读最近的**单声道**样本（已按左右交错解出左声道）。
 * 最多能取回 MIC_HALF_SAMPLES/2 个，返回实取个数。0 = 还没有完整半块。 */
uint16_t INMP441_Read(int16_t *dst, uint16_t n);

/* 音量：对最近半块算 RMS 再压成 0~100（0 = 静音）。没数据返回 0。 */
uint8_t  INMP441_Level(void);
/* 上一次 INMP441_Level() 算出的原始 RMS（0~32767）。 */
uint32_t INMP441_Rms(void);
/* 已完成的**半块**计数。★ 最好用的判据：没接麦 / I2S 没跑起来时它**一直不涨**。 */
uint32_t INMP441_Blocks(void);
/* DMA 通道里还剩多少个半字没搬完（NDTR 寄存器）。
 * ★ 它**在变**（比起上次读数）就说明数据一直在搬 —— 这是把
 *   「中断这条路断了」和「外设根本没发请求」分开的唯一便宜办法：
 *   `半块` 不涨 + 它在变 ⇒ 是中断的问题，**不是**接线/时钟的问题。 */
uint16_t INMP441_DmaLeft(void);
/* 最近半块的直流均值（INMP441 有个不小的直流偏置，这是正常的，不是 bug）。 */
int32_t  INMP441_Mean(void);
/* 最近半块左声道的峰峰值（max − min）。判断"麦到底有没有在动"最直观的量：
 * 静音只剩底噪时是几十~几百，正常说话能到几千~几万。 */
int32_t  INMP441_PeakToPeak(void);

/* 串口自检：打 PLLI2S/I2SDIV **实际寄存器值** + 数据线是否卡死 + 左右槽对比。
 * 出问题先看这一行，它把"I2S 没跑"和"线接错了"分开了。 */
void     INMP441_Diag(void);

#endif /* __INMP441_H */
