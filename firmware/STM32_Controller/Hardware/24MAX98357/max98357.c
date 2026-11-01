#include "max98357.h"
#include "inmp441.h"
#include <string.h>

/* DMA1 Stream4/Channel0 是 SPI2_TX；Stream3 留给 INMP441 接收。 */
#define AMP_DMA          DMA1_Stream4
#define AMP_DMA_CHANNEL  DMA_Channel_0
#define AMP_DMA_HT       DMA_IT_HTIF4
#define AMP_DMA_TC       DMA_IT_TCIF4
#define AMP_DMA_TE       DMA_IT_TEIF4
#define AMP_DMA_FLAGS    (DMA_FLAG_HTIF4 | DMA_FLAG_TCIF4 | DMA_FLAG_TEIF4)

#define AMP_RATE         16000u
#define AMP_RING_BYTES   2048u       /* 64 ms 的 16 kHz/16 bit 单声道 PCM 缓冲；为 OTA RAM 留出空间 */
#define AMP_DMA_WORDS    1024u       /* 512 个立体声样本，2 KB；双半块各 16 ms */
#define AMP_HALF_WORDS   (AMP_DMA_WORDS / 2u)

static uint8_t  g_ring[AMP_RING_BYTES];
static uint16_t g_rh, g_wh;
static uint32_t g_used;
static uint16_t g_dma[AMP_DMA_WORDS];
static volatile uint8_t g_half_event;
static volatile uint8_t g_full_event;
static volatile uint32_t g_underruns;
static uint8_t g_busy;
static uint8_t g_ending;
static uint8_t g_drain;

static uint16_t ring_read16(uint8_t *ok)
{
    uint16_t v;
    if (g_used < 2u) { *ok = 0u; return 0u; }
    v = (uint16_t)g_ring[g_rh];
    g_rh = (uint16_t)((g_rh + 1u) % AMP_RING_BYTES);
    v |= (uint16_t)g_ring[g_rh] << 8;
    g_rh = (uint16_t)((g_rh + 1u) % AMP_RING_BYTES);
    g_used -= 2u;
    *ok = 1u;
    return v;
}

static void fill_half(uint16_t offset)
{
    uint16_t i;
    uint8_t ok;
    for (i = 0u; i < AMP_HALF_WORDS; i += 2u)
    {
        uint16_t sample = ring_read16(&ok);
        if (!ok) {
            sample = 0u;
            if (g_busy && !g_ending) g_underruns++;
        }
        g_dma[offset + i] = sample;
        g_dma[offset + i + 1u] = sample;
    }
}

static void amp_gpio_tx(void)
{
    GPIO_InitTypeDef gpio;
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource9, GPIO_AF_SPI2);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource10, GPIO_AF_SPI2);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource15, GPIO_AF_SPI2);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_15;
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOB, &gpio);
}

static void amp_dma_config(void)
{
    DMA_InitTypeDef dma;
    NVIC_InitTypeDef nvic;
    DMA_Cmd(AMP_DMA, DISABLE);
    DMA_DeInit(AMP_DMA);
    DMA_StructInit(&dma);
    dma.DMA_Channel = AMP_DMA_CHANNEL;
    dma.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DR;
    dma.DMA_Memory0BaseAddr = (uint32_t)g_dma;
    dma.DMA_DIR = DMA_DIR_MemoryToPeripheral;
    dma.DMA_BufferSize = AMP_DMA_WORDS;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_High;
    dma.DMA_FIFOMode = DMA_FIFOMode_Disable;
    dma.DMA_FIFOThreshold = DMA_FIFOThreshold_1QuarterFull;
    dma.DMA_MemoryBurst = DMA_MemoryBurst_Single;
    dma.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
    DMA_Init(AMP_DMA, &dma);
    DMA_ITConfig(AMP_DMA, DMA_IT_HT | DMA_IT_TC | DMA_IT_TE, ENABLE);
    nvic.NVIC_IRQChannel = DMA1_Stream4_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 5u;
    nvic.NVIC_IRQChannelSubPriority = 0u;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
}

uint8_t MAX98357_Init(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB | RCC_AHB1Periph_DMA1, ENABLE);
    g_busy = 0u; g_ending = 0u; g_drain = 0u; g_half_event = 0u; g_full_event = 0u;
    g_underruns = 0u; g_rh = g_wh = 0u; g_used = 0u;
    return 0u;
}

static uint8_t amp_i2s_start(void)
{
    I2S_InitTypeDef i2s;
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
    SPI_I2S_DeInit(SPI2);
    amp_gpio_tx();
    I2S_StructInit(&i2s);
    i2s.I2S_Mode = I2S_Mode_MasterTx;
    i2s.I2S_Standard = I2S_Standard_Phillips;
    i2s.I2S_DataFormat = I2S_DataFormat_16b;
    i2s.I2S_MCLKOutput = I2S_MCLKOutput_Disable;
    i2s.I2S_AudioFreq = AMP_RATE;
    i2s.I2S_CPOL = I2S_CPOL_Low;
    I2S_Init(SPI2, &i2s);
    amp_dma_config();
    fill_half(0u); fill_half(AMP_HALF_WORDS);
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, ENABLE);
    DMA_Cmd(AMP_DMA, ENABLE);
    I2S_Cmd(SPI2, ENABLE);
    return 1u;
}

uint8_t MAX98357_Begin(uint16_t session, const uint8_t *params, uint16_t len)
{
    uint32_t rate;
    uint8_t bits, channels;
    if (g_busy || !params || len < 8u) return 2u;
    rate = (uint32_t)params[0] | ((uint32_t)params[1] << 8) |
           ((uint32_t)params[2] << 16) | ((uint32_t)params[3] << 24);
    bits = params[4]; channels = params[5];
    if (rate != AMP_RATE || bits != 16u || channels != 1u) return 2u;
    INMP441_Stop();
    g_rh = g_wh = 0u; g_used = 0u;
    (void)session;
    g_ending = 0u; g_drain = 0u; g_half_event = g_full_event = 0u;
    g_underruns = 0u; g_busy = 1u;
    if (!amp_i2s_start()) { g_busy = 0u; INMP441_Init(); INMP441_Start(); return 2u; }
    return 0u;
}

uint8_t MAX98357_Pcm(uint16_t seq, const uint8_t *pcm, uint16_t bytes)
{
    uint16_t i;
    (void)seq;
    if (!g_busy || g_ending || !pcm || !bytes || (bytes & 1u)) return 2u;
    if ((uint32_t)bytes > AMP_RING_BYTES - g_used) return 1u;
    for (i = 0u; i < bytes; i++) {
        g_ring[g_wh] = pcm[i];
        g_wh = (uint16_t)((g_wh + 1u) % AMP_RING_BYTES);
    }
    g_used += bytes;
    return 0u;
}

uint8_t MAX98357_End(uint16_t session)
{
    /*
     * BEGIN 的 seq 是播放会话标识；后续 PCM/END 帧的 seq 是逐帧序号，
     * 因此 END 不能再拿当前帧序号去和 g_session 比较。ESP 桥在发送
     * END 时会递增帧序号，旧的比较会让最后一帧永远被拒绝，表现为
     * “文字已返回但扬声器没有完整收尾”。当前 F407 只有一个播放槽，
     * 且每个 PCM/END 都经过同一条 UART 帧通道；只要有活动播放会话，
     * 接受 END 并进入 DMA 排空即可。
     */
    (void)session;
    if (!g_busy) return 2u;
    g_ending = 1u;
    g_drain = 2u;
    return 0u;
}

void MAX98357_Stop(void)
{
    if (!g_busy) return;
    I2S_Cmd(SPI2, DISABLE);
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, DISABLE);
    DMA_Cmd(AMP_DMA, DISABLE);
    g_busy = 0u; g_ending = 0u; g_drain = 0u;
    /* 恢复同一组 I2S2 接收配置，播放与录音不会同时争用 SPI2。 */
    (void)INMP441_Init();
    INMP441_Start();
}

void MAX98357_Tick(void)
{
    uint8_t do_stop = 0u;
    uint8_t progressed = 0u;
    if (!g_busy) return;
    /* 中断只置位事件；主循环先取副本再清零，避免在填充 DMA 缓冲时
     * 丢掉下一次半块通知。收尾计数只能由真实 DMA 事件推进，不能按
     * 主循环次数递减，否则最后一小段 PCM 会被过早截断。 */
    if (g_half_event) { g_half_event = 0u; fill_half(0u); progressed = 1u; }
    if (g_full_event) { g_full_event = 0u; fill_half(AMP_HALF_WORDS); progressed = 1u; }
    if (g_ending && g_used == 0u && progressed && g_drain) g_drain--;
    if (g_ending && g_used == 0u && g_drain == 0u) do_stop = 1u;
    if (do_stop) MAX98357_Stop();
}

uint8_t MAX98357_Busy(void) { return g_busy; }
uint32_t MAX98357_Underruns(void) { return g_underruns; }

void MAX98357_DMA_IRQHandler(void)
{
    if (DMA_GetITStatus(AMP_DMA, AMP_DMA_HT) != RESET) {
        DMA_ClearITPendingBit(AMP_DMA, AMP_DMA_HT);
        g_half_event = 1u;
    }
    if (DMA_GetITStatus(AMP_DMA, AMP_DMA_TC) != RESET) {
        DMA_ClearITPendingBit(AMP_DMA, AMP_DMA_TC);
        g_full_event = 1u;
    }
    if (DMA_GetITStatus(AMP_DMA, AMP_DMA_TE) != RESET) {
        DMA_ClearITPendingBit(AMP_DMA, AMP_DMA_TE);
        g_underruns++;
    }
}
