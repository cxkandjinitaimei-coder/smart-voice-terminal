/**
 * @file inmp441.c
 * 用途：INMP441 的 I2S2/DMA 接收、声道选择、采样统计和读半块接口。
 * 使用：PB10=CK/PB9=WS/PC3=SD；Init 启动 DMA，前台尽快复制最新完成的 32ms 半块。
 * 时序与边界：DMA 计数是累计完成次数，不是可回放队列；另一半块正在写，漏处理不能用新音频伪补旧样本。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "inmp441.h"
#include <stdio.h>

/* ==================================================================
 * INMP441 驱动实现（I2S2 主机接收 + DMA 循环 + 半块中断） * ------------------------------------------------------------------
 * 数据流：
 *   INMP441 --SD--> PC3 --(I2S2 硬件移位)--> SPI2->DR
 *           --(DMA1_Stream3 循环搬)--> g_dma_buf[1024]
 *           --(HT/TC 中断只加一个计数)--> 主循环读"最近完成的半块"
 *
 *  中断里**只加计数，不做拷贝**：主循环慢（画屏一格要几 ms）也不会
 *   错过数据，最坏只是把旧的半块丢掉 —— 音量表完全不在乎。
 *  双半块 = 双缓冲：DMA 写一半时主循环读另一半，**天然不会互相踩**。
 *
 *  为什么 24 位的麦只取 16 位：
 *   INMP441 输出 24 位，但 STM32 这边配的是 16 位帧（一帧 = 左 16 + 右 16）。
 *   数据是 MSB 先出的 ⇒ 被读走的正好是**最高的 16 位**，丢的是最低 8 位的
 *   精度。人声有效动态约 60dB，16 位有 96dB，够用。
 *   换成 24b/32b 能拿全 24 位，但那样 DMA 每次要读两下 DR 才凑一个样本，
 *   麻烦且对"上传云端做语音识别"没有收益 ⇒ 不值当。
 *
 *  左右声道为什么要"隔一个取一个"：
 *   I2S 一帧里含**左 16 位 + 右 16 位**，DMA 收到的是左右**交错**的流。
 *   而 L/R 脚接地 ⇒ 麦只在**左**槽驱动数据，右槽是三态（读回恒定值）。
 *   所以必须按奇偶挑一路，否则一半样本是垃圾，音量/波形全是错的。
 *   至于是"偶数为左"还是"奇数为左"，由 `INMP441_Calibrate()` 定标。
 * ================================================================== */

/* ---------- 内部状态 ---------- */
static int16_t g_dma_buf[MIC_DMA_SAMPLES]; /* DMA 循环写入（4KB）。uint16 数组天然 2 字节对齐，
                                                 半字传输够用（AHB 只要求对齐到数据宽度） */
static volatile uint32_t g_blocks = 0u;    /* 已完成的**半块**数（HT/TC 各 +1） */
static volatile uint32_t g_dma_err = 0u;   /* DMA 传输错误次数（正常应恒为 0） */
static uint8_t g_parity = MIC_LEFT_PARITY; /* 左声道在缓冲里的下标奇偶 */
static uint8_t g_valid = 0u;               /* 最近一次测量是否有效 */
static uint32_t g_rms = 0u;                /* 最近半块的 RMS（已去直流） */
static int32_t g_mean = 0;                 /* 最近半块的直流均值 */
static int32_t g_min = 0;                  /* 左声道最小值 */
static int32_t g_max = 0;                  /* 左声道最大值 */

/* ==================================================================
 * 整数开平方（floor）。故意不引 math.h：
 *    语音只要个"音量条"，不需要浮点精度；
 *    省掉 libm 的链接体积，也避免 FPU 上下文带来的麻烦。
 * 算法是经典的"逐位试商"法，输入要 < 2^32（本文件最大 32767^2 ≈ 2^30，安全）。
 * ================================================================== */
static uint32_t isqrt32(uint32_t num)
{
    uint32_t res = 0u;
    uint32_t bit = (uint32_t)0x40000000u; /* 1 << 30 */

    while (bit > num)
    {
        bit >>= 2;
    }
    while (bit != 0u)
    {
        if (num >= res + bit)
        {
            num -= res + bit;
            res = (res >> 1) + bit;
        }
        else
        {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

/* ==================================================================
 * 一小段"真会执行"的延时：计数器是 volatile，编译器不能把循环优化掉。
 * 只给 Diag() 用来量 DMA 的 NDTR 有没有在变 —— 变=数据在搬，不变=没搬。
 * ================================================================== */
static void mic_short_delay(void)
{
    volatile uint32_t i;

    for (i = 0u; i < 300000u; i++)
    {
    }
}

/* GPIO 配置不能只相信 Init() 里的宏：PB10 原先也能被 USART3 占用，
 * 后续有人初始化别的模块时，AF 会被静默改写。这里从实际寄存器反查。
 * GPIO_MODER 的 AF 模式为 2，AFR 的 I2S2 选择为 AF5。 */
static uint8_t mic_pin_is_af5(GPIO_TypeDef *port, uint8_t pin)
{
    uint32_t mode;
    uint32_t af;

    mode = (port->MODER >> ((uint32_t)pin * 2u)) & 3u;
    af = (port->AFR[pin >> 3] >> (((uint32_t)pin & 7u) * 4u)) & 0x0Fu;
    return ((mode == 2u) && (af == (uint32_t)MIC_AF)) ? 1u : 0u;
}

uint8_t INMP441_PinCheck(void)
{
    uint8_t bad = 0u;
    uint8_t pb10_ok, pb9_ok, pc3_ok;
    uint32_t cfgr;

    pb10_ok = mic_pin_is_af5(MIC_CK_PORT, MIC_CK_SRC);
    pb9_ok = mic_pin_is_af5(MIC_WS_PORT, MIC_WS_SRC);
    pc3_ok = mic_pin_is_af5(MIC_SD_PORT, MIC_SD_SRC);

    if (pb10_ok == 0u)
    {
        bad |= MIC_PINCHK_PB10;
    }
    if (pb9_ok == 0u)
    {
        bad |= MIC_PINCHK_PB9;
    }
    if (pc3_ok == 0u)
    {
        bad |= MIC_PINCHK_PC3;
    }

    cfgr = MIC_I2S->I2SCFGR;
    if (((cfgr & SPI_I2SCFGR_I2SMOD) == 0u) || (((cfgr & SPI_I2SCFGR_I2SCFG) >> 8) != 3u) ||
        ((cfgr & SPI_I2SCFGR_I2SE) == 0u))
    {
        bad |= MIC_PINCHK_I2S;
    }
    if (((MIC_DMA->CR & DMA_SxCR_EN) == 0u) || (((MIC_DMA->CR & DMA_SxCR_CHSEL) >> 25) != 0u))
    {
        bad |= MIC_PINCHK_DMA;
    }

    printf("[MIC] 引脚自检: PB10(CK)=AF%lu/%s PB9(WS)=AF%lu/%s "
           "PC3(SD)=AF%lu/%s\r\n",
           (unsigned long)((MIC_CK_PORT->AFR[1] >> 8) & 0x0Fu), pb10_ok ? "OK" : "BAD",
           (unsigned long)((MIC_WS_PORT->AFR[1] >> 4) & 0x0Fu), pb9_ok ? "OK" : "BAD",
           (unsigned long)((MIC_SD_PORT->AFR[0] >> 12) & 0x0Fu), pc3_ok ? "OK" : "BAD");
    printf("[MIC] 引脚自检: I2S2=%s DMA1-S3=%s result=0x%02X\r\n",
           (bad & MIC_PINCHK_I2S) ? "BAD" : "OK", (bad & MIC_PINCHK_DMA) ? "BAD" : "OK",
           (unsigned)bad);
    if (bad != 0u)
    {
        printf("[MIC] ★ 配置异常：PB10/PB9/PC3 必须都是 AF5；"
               "PB10 不能同时初始化 USART3。\r\n");
        printf("[MIC]   result 位: 01=PB10 02=PB9 04=PC3 08=I2S2 10=DMA1-S3\r\n");
    }
    else
    {
        printf("[MIC] 引脚映射正确；下一段数据诊断会继续判断模块供电和实物接线。\r\n");
    }
    return bad;
}

/* ==================================================================
 * 从"最近完成的半块"里解出左声道并算统计量（内部用，不导给外部）
 * ================================================================== */
static void mic_measure(void)
{
    const int16_t *half;
    uint16_t i, cnt = 0u;
    int32_t mean = 0, mn = 32767, mx = -32768, v;
    uint32_t sum = 0u;

    g_valid = 0u;
    if (g_blocks == 0u)
    {
        return; /* 一个半块都还没好 ⇒ 麦没接 / I2S 没跑起来（Diag 会区分这两种） */
    }

    /* 第 1 个半块 → 缓冲前半；第 2 个 → 后半；第 3 个 → 又回前半……
     * 所以"最近完成的那个"下标 = (已完成数 - 1) & 1 */
    half = &g_dma_buf[((g_blocks - 1u) & 1u) * MIC_HALF_SAMPLES];

    /* 第一遍：求直流均值（INMP441 有固定直流偏置，不去掉的话静音也有一大截） */
    for (i = g_parity; i < MIC_HALF_SAMPLES; i += 2u)
    {
        v = (int32_t)half[i];
        mean += v;
        if (v < mn)
        {
            mn = v;
        }
        if (v > mx)
        {
            mx = v;
        }
        cnt++;
    }
    if (cnt == 0u)
    {
        return;
    }
    mean /= (int32_t)cnt;

    /* 第二遍：去直流后求 RMS */
    for (i = g_parity; i < MIC_HALF_SAMPLES; i += 2u)
    {
        v = (int32_t)half[i] - mean;
        if (v < 0)
        {
            v = -v;
        }
        /* 防御：极端偏置下 |v| 理论上能到 65535，v*v 会溢出 int32。
         * 实际到不了，但夹一下比留个静默 UB 划算（而且不影响正常量程）。 */
        if (v > 32767)
        {
            v = 32767;
        }
        sum += (uint32_t)(v * v);
    }

    g_mean = mean;
    g_min = mn;
    g_max = mx;
    g_rms = isqrt32(sum / (uint32_t)cnt);
    g_valid = 1u;
}

/* ==================================================================
 * 初始化
 * ================================================================== */
uint8_t INMP441_Init(void)
{
    GPIO_InitTypeDef gpio;
    I2S_InitTypeDef i2s;
    DMA_InitTypeDef dma;
    NVIC_InitTypeDef nvic;
    uint32_t guard;

    g_blocks = 0u;
    g_dma_err = 0u;
    g_valid = 0u;
    g_parity = MIC_LEFT_PARITY;

    /* ----  GPIO 与 DMA 的时钟（这两块在 AHB1） ---- */
    RCC_AHB1PeriphClockCmd(MIC_GPIO_CLK | MIC_DMA_CLK, ENABLE);

    /* ----  PLLI2S：I2S 的时钟源 -------------------------------
     *  顺序踩过坑：必须「先关 PLLI2S → 再改分频 → 再开」，
     *    而且**必须早于**使能 SPI2 的 APB1 时钟、早于 I2S_Init()。
     *    库函数 I2S_Init() 是**直接去读 RCC 寄存器反算分频**的，
     *    顺序反了它按旧值算，采样率就跑飞（现象：声音又尖又快）。 */
    RCC_PLLI2SCmd(DISABLE);
    guard = 0u;
    while (((RCC->CR & RCC_CR_PLLI2SRDY) != 0u) && (guard < 0x20000u))
    {
        guard++;
    }

    RCC_PLLI2SConfig(MIC_PLLI2SN, MIC_PLLI2SR); /* 8MHz/8 × 258 / 3 = 86MHz */

    RCC_PLLI2SCmd(ENABLE);
    guard = 0u;
    while (((RCC->CR & RCC_CR_PLLI2SRDY) == 0u) && (guard < 0x100000u))
    {
        guard++;
    }
    if ((RCC->CR & RCC_CR_PLLI2SRDY) == 0u)
    {
        printf("[MIC] ★ PLLI2S 起不来（等不到 PLLI2SRDY）—— 查 HSE 是否为 8MHz\r\n");
        return 1u;
    }

    /* 选 PLLI2S 当 I2S 时钟。 库注释明确要求：**必须在使能 SPI2 的 APB1 时钟之前** */
    RCC_I2SCLKConfig(RCC_I2S2CLKSource_PLLI2S);
    RCC_APB1PeriphClockCmd(MIC_I2S_CLK, ENABLE);

    /* ----  引脚：PB10/PB9/PC3 全部 AF5 ------------------------- */
    GPIO_PinAFConfig(MIC_CK_PORT, MIC_CK_SRC, MIC_AF);
    GPIO_PinAFConfig(MIC_WS_PORT, MIC_WS_SRC, MIC_AF);
    GPIO_PinAFConfig(MIC_SD_PORT, MIC_SD_SRC, MIC_AF);

    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_PP; /*  F4 的枚举叫 PP，不是 F1 那个 GPIO_OType_PushPull */
    gpio.GPIO_PuPd = GPIO_PuPd_UP;   /* 对 CK/WS 两个输出脚无害；
                                               对 SD(输入) 是防悬空 —— 麦没插时读回恒定的全 1，
                                               正好能被 Diag() 认出来"线没接" */
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Pin = MIC_CK_PIN | MIC_WS_PIN; /* CK 和 WS 都在 GPIOB */
    GPIO_Init(MIC_CK_PORT, &gpio);
    gpio.GPIO_Pin = MIC_SD_PIN; /* SD 单独在 GPIOC */
    GPIO_Init(MIC_SD_PORT, &gpio);

    /* ----  DMA：循环模式 + 双半块，外设→内存、半字对半字 ------------- */
    DMA_DeInit(MIC_DMA);
    DMA_ClearFlag(MIC_DMA, MIC_DMA_FLAG_HT | MIC_DMA_FLAG_TC | MIC_DMA_FLAG_TE | MIC_DMA_FLAG_DME |
                               MIC_DMA_FLAG_FE);

    dma.DMA_Channel = MIC_DMA_CHANNEL;
    dma.DMA_PeripheralBaseAddr = (uint32_t)(&(MIC_I2S->DR)); /* 永远读同一个数据寄存器 */
    dma.DMA_Memory0BaseAddr = (uint32_t)(&g_dma_buf[0]);
    dma.DMA_DIR = DMA_DIR_PeripheralToMemory;
    dma.DMA_BufferSize = MIC_DMA_SAMPLES;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dma.DMA_Mode = DMA_Mode_Circular; /*  不循环收满就停了 */
    dma.DMA_Priority = DMA_Priority_High;
    dma.DMA_FIFOMode = DMA_FIFOMode_Disable; /* 直连模式：半字对半字用不着 FIFO */
    dma.DMA_FIFOThreshold = DMA_FIFOThreshold_HalfFull;
    dma.DMA_MemoryBurst = DMA_MemoryBurst_Single;
    dma.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
    DMA_Init(MIC_DMA, &dma);

    /* ----  中断：只用来数"第几个半块好了" ---- */
    nvic.NVIC_IRQChannel = MIC_DMA_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2u; /* 比串口(1)低一档：串口丢字比音量表晚一拍更烦人 */
    nvic.NVIC_IRQChannelSubPriority = 0u;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
    /*  这里必须用 SxCR 的**使能位**那一套（MIC_DMA_IT_EN）。
     *   传 LISR 的标志掩码（MIC_DMA_IT_HT/TC/TE）不会报错、也不会被断言拦住，
     *   但 `DMA_ITConfig` 只是 `CR |= 值` ⇒ 会把 **CHSEL 改成 7**（请求再也送不到）、
     *   **MSIZE 改成 32 位**，而 HTIE/TCIE/TEIE 一个都没置上 ⇒ 半块计数恒 0。
     *   完整说明见 .h 里那段注释。 */
    DMA_ITConfig(MIC_DMA, MIC_DMA_IT_EN, ENABLE);

    /* ----  I2S 本体 ---- */
    I2S_StructInit(&i2s);
    i2s.I2S_Mode = I2S_Mode_MasterRx;         /* 麦是从机：CK/WS 由 STM32 出 */
    i2s.I2S_Standard = I2S_Standard_Phillips; /*  标准 I2S：数据比 WS 边沿**晚 1 个 SCK**。
                                                     INMP441 就是这个格式；改成 MSB/LSB 标准
                                                     会整帧错位，表现是"一片噪声"。 */
    i2s.I2S_DataFormat = I2S_DataFormat_16b; /* 见文件头"24 位麦只取高 16 位" */
    i2s.I2S_MCLKOutput = I2S_MCLKOutput_Disable; /* INMP441 自带主时钟不需要；关掉还省下 PC6 */
    i2s.I2S_AudioFreq = MIC_SAMPLE_RATE;
    i2s.I2S_CPOL = I2S_CPOL_Low; /*  INMP441 要求 SCK 空闲电平为低 */
    I2S_Init(MIC_I2S, &i2s);

    /* ----  清掉上电残留的 OVR/RXNE，否则第一包是旧的 ---- */
    (void)(MIC_I2S->DR);
    (void)(MIC_I2S->SR);

    /* ----  开闸：先把 DMA 通路接好，最后才启 I2S ---- */
    SPI_I2S_DMACmd(MIC_I2S, SPI_I2S_DMAReq_Rx, ENABLE);
    DMA_Cmd(MIC_DMA, ENABLE);
    I2S_Cmd(MIC_I2S, ENABLE);
    (void)INMP441_PinCheck();

    printf("[MIC] INMP441 初始化完成：I2S2 主机收 %ukHz/16bit，"
           "PB10=CK PB9=WS PC3=SD（L/R 接地=左声道）\r\n",
           (unsigned)(MIC_SAMPLE_RATE / 1000u));
    return 0u;
}

void INMP441_Start(void)
{
    g_blocks = 0u;
    SPI_I2S_DMACmd(MIC_I2S, SPI_I2S_DMAReq_Rx, ENABLE);
    DMA_Cmd(MIC_DMA, ENABLE);
    I2S_Cmd(MIC_I2S, ENABLE);
}

void INMP441_Stop(void)
{
    I2S_Cmd(MIC_I2S, DISABLE);
    SPI_I2S_DMACmd(MIC_I2S, SPI_I2S_DMAReq_Rx, DISABLE);
    DMA_Cmd(MIC_DMA, DISABLE);
}

/* ==================================================================
 * DMA 中断：HT（前半满）/ TC（后半满）各把计数 +1
 * ------------------------------------------------------------------
 *  只加计数。这里**不要**做算术、不要拷贝、不要 printf。
 * ================================================================== */
void MIC_DMA_IRQHandler(void)
{
    if (DMA_GetITStatus(MIC_DMA, MIC_DMA_IT_HT) != RESET)
    {
        DMA_ClearITPendingBit(MIC_DMA, MIC_DMA_IT_HT);
        g_blocks++;
    }
    if (DMA_GetITStatus(MIC_DMA, MIC_DMA_IT_TC) != RESET)
    {
        DMA_ClearITPendingBit(MIC_DMA, MIC_DMA_IT_TC);
        g_blocks++;
    }
    /* 传输错误：只有 DMA 配错/地址非法才会来。只记账，不复位（Diag 里能看见） */
    if (DMA_GetITStatus(MIC_DMA, MIC_DMA_IT_TE) != RESET)
    {
        DMA_ClearITPendingBit(MIC_DMA, MIC_DMA_IT_TE);
        g_dma_err++;
    }
}

/* ==================================================================
 * 自定标：判定左声道在缓冲里的下标奇偶
 * ------------------------------------------------------------------
 * 判据（不需要示波器就能想明白）：
 *   L/R 接地 ⇒ 麦**只在一个槽**驱动数据，另一个槽是三态（被上拉成恒定值）。
 *   ⇒ "会变化"的那个槽才是麦，它的下标奇偶就是答案。
 * 返回 0 = 定标完成 / 1 = 等不到数据（麦没接或没供电）/ 2 = 分不出来（两个槽
 *   都不变或都在变），此时保持默认奇偶，不算失败。
 * ================================================================== */
uint8_t INMP441_Calibrate(void)
{
    const int16_t *half;
    uint16_t i;
    uint8_t p, varies, found = 0xFFu;
    uint32_t guard;

    /* 等一个完整半块（约 32ms）；给足余量到 ~100ms，避免没接麦时卡死 */
    guard = 0u;
    while ((g_blocks == 0u) && (guard < 8000000u))
    {
        guard++;
    }
    if (g_blocks == 0u)
    {
        return 1u;
    }

    half = &g_dma_buf[((g_blocks - 1u) & 1u) * MIC_HALF_SAMPLES];

    for (p = 0u; p < 2u; p++)
    {
        varies = 0u;
        for (i = (uint16_t)p; i < MIC_HALF_SAMPLES; i += 2u)
        {
            if (half[i] != half[p])
            {
                varies = 1u;
                break;
            }
        }
        if (varies != 0u)
        {
            if (found == 0xFFu)
            {
                found = p;
            }
            else
            {
                found = 0xFEu;
                break;
            } /* 两个槽都在变 ⇒ 分不出来 */
        }
    }

    if ((found == 0xFFu) || (found == 0xFEu))
    {
        g_parity = MIC_LEFT_PARITY; /* 保持默认，不报失败 */
        return 2u;
    }
    g_parity = found;
    return 0u;
}

/* ==================================================================
 * 读数据 / 音量
 * ================================================================== */
uint16_t INMP441_Read(int16_t *dst, uint16_t n)
{
    const int16_t *half;
    uint16_t i, cnt = 0u;

    if ((dst == 0) || (g_blocks == 0u))
    {
        return 0u;
    }
    half = &g_dma_buf[((g_blocks - 1u) & 1u) * MIC_HALF_SAMPLES];

    /* 隔一个取一个 = 把左右交错的流"抽"成单声道 */
    for (i = g_parity; (i < MIC_HALF_SAMPLES) && (cnt < n); i += 2u)
    {
        dst[cnt] = half[i];
        cnt++;
    }
    return cnt;
}

uint8_t INMP441_Level(void)
{
    uint32_t lv;

    mic_measure();
    if (g_valid == 0u)
    {
        return 0u;
    }

    /* RMS 0~32767 → 0~100，用开平方当压缩。
     * 为什么不用线性：线性刻度下小声几乎贴着 0、正常说话就顶格，看不出层次；
     * 开方 ≈ 半个 dB 刻度，观感最接近"音量条"。
     * 20 是静音门限：INMP441 的底噪 RMS 是个位数，低于它就报 0（不然后底噪也闪）。
     *  这两个数（20 / 181）是"看着顺眼"定的，不是物理量，随便调。 */
    if (g_rms < 20u)
    {
        return 0u;
    }
    lv = ((uint32_t)isqrt32(g_rms) * 100u) / 181u; /* isqrt(32767) ≈ 181 */
    if (lv > 100u)
    {
        lv = 100u;
    }
    return (uint8_t)lv;
}

uint32_t INMP441_Rms(void)
{
    mic_measure();
    return g_rms;
}
int32_t INMP441_Mean(void)
{
    mic_measure();
    return g_mean;
}
int32_t INMP441_PeakToPeak(void)
{
    mic_measure();
    return g_max - g_min;
}
uint32_t INMP441_Blocks(void)
{
    return g_blocks;
}
uint16_t INMP441_DmaLeft(void)
{
    return (uint16_t)MIC_DMA->NDTR;
}

/* ==================================================================
 * 串口自检
 * ------------------------------------------------------------------
 * 刻意从**实际寄存器**反算时钟和分频，而不是把配置宏再打一遍 ——
 * 这样才能抓住"宏写对了、但库静默回落到 I2SDIV=2"这种情况。
 * 也刻意把两个槽的 min/max 都打出来，因为"SD 线没接"和"I2S 没跑"
 * 的现象很像，但原因完全在不同地方。
 * ================================================================== */
void INMP441_Diag(void)
{
    uint32_t pllm, plln, pllr, i2sclk, div, odd, fs;
    uint32_t guard;
    const int16_t *half;
    int32_t mn[2], mx[2];
    uint16_t i;
    uint8_t p;

    /* ---- 时钟链：从寄存器反算 ---- */
    pllm = RCC->PLLCFGR & RCC_PLLCFGR_PLLM;
    plln = (RCC->PLLI2SCFGR & RCC_PLLI2SCFGR_PLLI2SN) >> 6;
    pllr = (RCC->PLLI2SCFGR & RCC_PLLI2SCFGR_PLLI2SR) >> 28;
    if ((pllm == 0u) || (pllr == 0u))
    {
        pllm = 8u;
        pllr = 3u;
    } /* 防 0 除 */
    i2sclk = ((uint32_t)(HSE_VALUE / pllm) * plln) / pllr;

    div = MIC_I2S->I2SPR & (uint32_t)SPI_I2SPR_I2SDIV;
    odd = ((MIC_I2S->I2SPR & SPI_I2SPR_ODD) != 0u) ? 1u : 0u;
    fs = (div == 0u) ? 0u : (i2sclk / ((((uint32_t)2u * div) + odd) * 32u));

    printf("[MIC] 时钟链：HSE/%u × %u / %u = %u Hz(I2S)；"
           "I2SDIV=%u ODD=%u ⇒ 实际帧率 %u Hz（目标 %u）\r\n",
           (unsigned)pllm, (unsigned)plln, (unsigned)pllr, (unsigned)i2sclk, (unsigned)div,
           (unsigned)odd, (unsigned)fs, (unsigned)MIC_SAMPLE_RATE);
    if (div < 2u)
    {
        printf("[MIC] ★ I2SDIV 落到了默认值 2（采样率会变成几十 kHz）—— "
               "PLLI2S 没配好，或 PLLI2SN/R 让分频越界了\r\n");
    }

    /* ---- 数据：等一个半块再统计 ---- */
    guard = 0u;
    while ((g_blocks == 0u) && (guard < 8000000u))
    {
        guard++;
    }

    if (g_blocks == 0u)
    {
        uint32_t cr;
        uint16_t ndtr1, ndtr2, cfgr;

        /*  这里**不能**直接下"没在收数据"的结论 —— 第一版就这么下错了
         *   （它说"DMA 都没被触发过"，可当时手上只有"半块=0"这一个数，
         *     根本没有任何证据支撑那句话）。必须再读寄存器才能分开：
         *      NDTR 在变 ⇒ 数据确实在搬，是**中断**这条路断了
         *        （把 LISR 的标志掩码当使能位传 ⇒ HTIE/TCIE/TEIE 没置上。
         *          DMA 搬数据**不需要**中断使能，所以这两件事能同时成立）
         *      NDTR 不动 ⇒ 再看 CR 的 CHSEL：
         *        · CHSEL≠0 ⇒ **通道号被踩坏了**（就是 CHSEL 被改成 7）
         *          ⇒ 请求发出去也送不到这个流 ⇒ 一个字节都没搬
         *        · CHSEL=0 ⇒ 看 I2SCFGR.I2SE：0 = I2S 没使能；
         *          1 = 使能了但时钟没进外设（PLLI2S） */
        ndtr1 = (uint16_t)MIC_DMA->NDTR;
        mic_short_delay();
        ndtr2 = (uint16_t)MIC_DMA->NDTR;
        cr = MIC_DMA->CR;
        cfgr = MIC_I2S->I2SCFGR;

        printf("[MIC] ★ 半块计数=0（中断一次都没进来）。两种可能，看下面这行分：\r\n");
        printf("[MIC]   NDTR %u→%u（%s）\r\n", (unsigned)ndtr1, (unsigned)ndtr2,
               (ndtr1 != ndtr2) ? "在变 ⇒ 数据其实在搬，是**中断**没进来"
                                : "不动 ⇒ 再看下面的 CHSEL 和 I2SE");
        printf("[MIC]   DMA CR: EN=%u CHSEL=%u MSIZE=%u HTIE=%u TCIE=%u TEIE=%u\r\n",
               (unsigned)((cr & DMA_SxCR_EN) ? 1u : 0u), (unsigned)((cr & DMA_SxCR_CHSEL) >> 25),
               (unsigned)((cr & DMA_SxCR_MSIZE) >> 13), (unsigned)((cr & DMA_SxCR_HTIE) ? 1u : 0u),
               (unsigned)((cr & DMA_SxCR_TCIE) ? 1u : 0u),
               (unsigned)((cr & DMA_SxCR_TEIE) ? 1u : 0u));
        printf("[MIC]     （应为 CHSEL=0 MSIZE=1(半字) HTIE=1 TCIE=1 TEIE=1）\r\n");
        printf("[MIC]   I2SCFGR: I2SMOD=%u I2SE=%u I2SCFG=%u（主机收应为 3）\r\n",
               (unsigned)((cfgr & SPI_I2SCFGR_I2SMOD) ? 1u : 0u),
               (unsigned)((cfgr & SPI_I2SCFGR_I2SE) ? 1u : 0u),
               (unsigned)((cfgr & SPI_I2SCFGR_I2SCFG) >> 8));
        printf("[MIC]   对照表：\r\n");
        printf(
            "[MIC]     CHSEL≠0（被改成 7）或 MSIZE≠1 或 HTIE/TCIE=0 ⇒ DMA_ITConfig 的宏用错了，\r\n");
        printf(
            "[MIC]        把标志掩码当使能位传了进去（见 inmp441.h 里那两套宏）—— 2026-09-18 就是这条；\r\n");
        printf("[MIC]     NDTR 在变 + HTIE/TCIE=0 ⇒ 中断那条路断了（数据其实在搬）；\r\n");
        printf(
            "[MIC]     NDTR 不动 + CHSEL=0 + I2SE=0 ⇒ 没使能 I2S；I2SE=1 ⇒ 时钟没进外设（PLLI2S）\r\n");
        return;
    }

    half = &g_dma_buf[((g_blocks - 1u) & 1u) * MIC_HALF_SAMPLES];

    for (p = 0u; p < 2u; p++)
    {
        mn[p] = 32767;
        mx[p] = -32768;
        for (i = (uint16_t)p; i < MIC_HALF_SAMPLES; i += 2u)
        {
            if ((int32_t)half[i] < mn[p])
            {
                mn[p] = (int32_t)half[i];
            }
            if ((int32_t)half[i] > mx[p])
            {
                mx[p] = (int32_t)half[i];
            }
        }
    }

    printf("[MIC] 半块=%lu DMA错误=%lu；左声道判定=下标%s\r\n", (unsigned long)g_blocks,
           (unsigned long)g_dma_err, ((g_parity == 0u) ? "偶数(0)" : "奇数(1)"));
    printf("[MIC] 槽A(偶下标) min=%ld max=%ld | 槽B(奇下标) min=%ld max=%ld\r\n", (long)mn[0],
           (long)mx[0], (long)mn[1], (long)mx[1]);

    if ((mn[0] == mx[0]) && (mn[1] == mx[1]))
    {
        printf("[MIC] ★ 两个槽都恒定（%ld）：SD 数据线没在动 —— 查 PC3 是否接好、"
               "麦是否供上 3.3V、SCK(PB10)/WS(PB9) 是否接反\r\n",
               (long)mn[0]);
    }
    else if ((mn[g_parity] == mx[g_parity]) && (mn[1u - g_parity] != mx[1u - g_parity]))
    {
        printf("[MIC] ★ 被判定为左声道的那个槽是恒定的、另一个却在动 —— "
               "L/R 判定反了，把 inmp441.h 的 MIC_LEFT_PARITY 取反（0↔1）\r\n");
    }
    else
    {
        mic_measure();
        printf("[MIC] 数据正常：直流%ld 峰峰值%ld RMS=%lu 音量=%u%%\r\n", (long)g_mean,
               (long)(mx[g_parity] - mn[g_parity]), (unsigned long)g_rms,
               (unsigned)INMP441_Level());
    }
}
