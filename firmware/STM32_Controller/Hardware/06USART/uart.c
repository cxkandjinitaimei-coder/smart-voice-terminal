/**
 * @file uart.c
 * 用途：USART2 调试口初始化、printf 重定向和前台命令输入。
 * 使用：系统时钟建立后初始化，PA2=TX/PA3=RX，115200；中断仅收字节，命令在主循环处理。
 * 时序与边界：调试文本与 ESP 协议串口分离；阻塞 printf 仍消耗前台时间，录音期限制大段输出。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "uart.h"
#include "uart_rx.h"
#include "time.h"
#include <stdio.h> /* 必须包含：重定向 printf */

/* ==================================================================
 * 串口驱动实现（只写逻辑，所有外设/引脚配置见 uart.h）
 *  起本模块跑 **USART2（PA2=TX，PA3=RX）** —— 原因是 USART1
 *   (PA9/PA10) 那组排针被插上 J1 的 32 针 TFT 屏挡住了（详见 uart.h 头部）。
 *   发送走 **DMA1_Stream6/CH4**（非阻塞），接收走中断（USART2_IRQHandler）。
 *
 *  本文件里**故意不出现** USART1/USART2 这种具体名字，也没有 APB1/APB2、
 *    Stream7/TCIF7 这类跟口绑定的常量 —— 全从 uart.h 的宏来。
 *    这样以后换串口只改 uart.h 一处，这里一行都不用动。
 *
 * 发送模型：printf/send_* 把字节写进环形缓冲 uart_tx_buf，
 *   fputc 在缓冲不满时立即返回（几乎不阻塞）；
 *   uart_dma_start() 把缓冲里未发的一段交给 DMA 搬到 UART->DR，
 *   发完触发 DMA 传输完成中断，再续发下一段，直到缓冲清空。
 * ================================================================== */

/* ==================== 发送环形缓冲（DMA TX） ==================== */
static volatile uint8_t uart_tx_buf[UART_TX_BUF_SIZE];
static volatile uint16_t uart_tx_head = 0; /* 写入游标 */
static volatile uint16_t uart_tx_tail = 0; /* DMA 已搬走的位置 */
static volatile uint16_t uart_tx_len = 0;  /* 本轮 DMA 配置的字节数 */
static volatile uint8_t uart_tx_busy = 0;  /* DMA 发送中 */

static void uart_dma_start(void); /* 前置声明（uart_tx_put 会调用） */

/* 往环形缓冲写一个字节（DMA 空闲时顺便启动发送） */
static void uart_tx_put(uint8_t ch)
{
    uint16_t next = (uint16_t)((uart_tx_head + 1) & (UART_TX_BUF_SIZE - 1));

    while (next == uart_tx_tail) /* 缓冲满：等 DMA 腾空间（极端情况） */
        ;

    uart_tx_buf[uart_tx_head] = ch;
    uart_tx_head = next;

    if (!uart_tx_busy)
        uart_dma_start();
}

/* 启动 DMA 发送缓冲里未发完的一段（环形不跨越末尾回绕，只发连续段） */
static void uart_dma_start(void)
{
    uint16_t pending, len;
    uint32_t irq_mask = __get_PRIMASK();

    /* Foreground and the completion ISR both enter here. Publish ownership
     * before enabling DMA: a one-byte transfer can complete immediately. */
    __disable_irq();

    if (uart_tx_busy)
    {
        __set_PRIMASK(irq_mask);
        return;
    }

    pending = (uint16_t)((uart_tx_head - uart_tx_tail) & (UART_TX_BUF_SIZE - 1));
    if (pending == 0)
    {
        __set_PRIMASK(irq_mask);
        return;
    }

    /* 只发从 tail 到缓冲末尾的连续字节，不够等 TC 中断后接着发 */
    len = (uint16_t)(UART_TX_BUF_SIZE - uart_tx_tail);
    if (len > pending)
        len = pending;

    /* 先停流再改地址/长度（Normal 模式发完会自动停，这里防残留） */
    DMA_Cmd(UART_TX_DMA_STREAM, DISABLE);
    while (DMA_GetCmdStatus(UART_TX_DMA_STREAM) != DISABLE)
        ;

    UART_TX_DMA_STREAM->M0AR = (uint32_t)&uart_tx_buf[uart_tx_tail];
    uart_tx_len = len;
    DMA_SetCurrDataCounter(UART_TX_DMA_STREAM, len);

    uart_tx_busy = 1;
    __DMB();
    DMA_Cmd(UART_TX_DMA_STREAM, ENABLE);
    __set_PRIMASK(irq_mask);
}

/* ==================== DMA 发送初始化 ==================== */
static void uart_dma_init(void)
{
    DMA_InitTypeDef DMA_InitStruct;
    NVIC_InitTypeDef NVIC_InitStruct;

    RCC_AHB1PeriphClockCmd(UART_TX_DMA_CLK, ENABLE);

    DMA_DeInit(UART_TX_DMA_STREAM);
    DMA_InitStruct.DMA_Channel = UART_TX_DMA_CHANNEL; /* CH4 = USARTx_TX */
    DMA_InitStruct.DMA_PeripheralBaseAddr = (uint32_t)&UART->DR;
    DMA_InitStruct.DMA_Memory0BaseAddr = (uint32_t)uart_tx_buf;
    DMA_InitStruct.DMA_DIR = DMA_DIR_MemoryToPeripheral;
    DMA_InitStruct.DMA_BufferSize = 1; /* 每段启动时用 SetCurrDataCounter 覆盖 */
    DMA_InitStruct.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStruct.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStruct.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStruct.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    DMA_InitStruct.DMA_Mode = DMA_Mode_Normal; /* 一段发完自动停 */
    DMA_InitStruct.DMA_Priority = DMA_Priority_High;
    DMA_InitStruct.DMA_FIFOMode = DMA_FIFOMode_Disable;
    DMA_InitStruct.DMA_FIFOThreshold = DMA_FIFOThreshold_1QuarterFull;
    DMA_InitStruct.DMA_MemoryBurst = DMA_MemoryBurst_Single;
    DMA_InitStruct.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
    DMA_Init(UART_TX_DMA_STREAM, &DMA_InitStruct);

    /* 每段发完触发 TC 中断续发下一段 */
    DMA_ITConfig(UART_TX_DMA_STREAM, DMA_IT_TC, ENABLE);

    NVIC_InitStruct.NVIC_IRQChannel = UART_TX_DMA_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);
}

/* ==================== 初始化函数 ==================== */
/**
  * @brief  初始化串口（PA2=TX，PA3=RX），发送 DMA，接收中断
  * @note   波特率 115200-8-N-1，收发模式。
  */
void UART_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    USART_InitTypeDef USART_InitStruct;
    NVIC_InitTypeDef NVIC_InitStruct;

    UART_RxReset();

    /* 1) 开启时钟：GPIO 在 AHB1，USART 在 APBx（x 由 uart.h 的 UART_CLK_CMD 决定） */
    RCC_AHB1PeriphClockCmd(UART_GPIO_CLK, ENABLE);
    UART_CLK_CMD(UART_CLK, ENABLE);

    /* 2) TX/RX 引脚复用为 USART（F4 必须手动绑复用功能，AF7） */
    GPIO_PinAFConfig(UART_PORT, UART_TX_SRC, UART_AF);
    GPIO_PinAFConfig(UART_PORT, UART_RX_SRC, UART_AF);
    GPIO_InitStruct.GPIO_Pin = UART_TX_PIN | UART_RX_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(UART_PORT, &GPIO_InitStruct);

    /* 3) USART 配置：115200-8-N-1，收发模式 */
    USART_InitStruct.USART_BaudRate = UART_BAUD;
    USART_InitStruct.USART_WordLength = USART_WordLength_8b;
    USART_InitStruct.USART_StopBits = USART_StopBits_1;
    USART_InitStruct.USART_Parity = USART_Parity_No;
    USART_InitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStruct.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(UART, &USART_InitStruct);

    /* 4) 使能 USART */
    USART_Cmd(UART, ENABLE);

    /* 5) 使能接收中断：RXNE（收到一个字节进 USARTx_IRQHandler）+ 开 NVIC */
    USART_ITConfig(UART, USART_IT_RXNE, ENABLE);
    NVIC_InitStruct.NVIC_IRQChannel = UART_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    /* 6) 使能 DMA 发送 + 初始化 DMA */
    USART_DMACmd(UART, USART_DMAReq_Tx, ENABLE);
    uart_dma_init();
}

/* ==================== 发送数据 ==================== */
/**
  * @brief  发送 len 字节原始数据（非阻塞 DMA，写入环形缓冲）
  */
void send_data(const char *data, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++)
        uart_tx_put((uint8_t)data[i]);
}

/* ==================== 发送字符串 ==================== */
/**
  * @brief  发送字符串（遇 '\0' 自动结束，非阻塞 DMA）
  */
void send_str(const char *str)
{
    while (*str)
        uart_tx_put((uint8_t)*str++);
}

/* ==================== 重定向 printf ==================== */
/**
  * @brief  printf 底层输出：写环形缓冲，由 DMA 发送（几乎不阻塞）
  */
int fputc(int ch, FILE *f)
{
    uart_tx_put((uint8_t)ch);
    return ch;
}

/* ==================== DMA 发送完成中断 ==================== */
/**
  * @brief  DMA 传输完成中断（DMA1_Stream6）：一段发完，更新游标并续发
  */
void UART_TX_DMA_IRQHandler(void)
{
    if (DMA_GetITStatus(UART_TX_DMA_STREAM, UART_TX_DMA_TCIF) != RESET)
    {
        DMA_ClearITPendingBit(UART_TX_DMA_STREAM, UART_TX_DMA_TCIF);

        uart_tx_tail = (uint16_t)((uart_tx_tail + uart_tx_len) & (UART_TX_BUF_SIZE - 1));
        uart_tx_busy = 0;

        if (uart_tx_head != uart_tx_tail) /* 缓冲里还有数据就接着发 */
            uart_dma_start();
    }
}

/* ==================== 接收（中断方式） ==================== */
volatile uint8_t uart_rx_byte = 0; /* 最新收到的字节 */
volatile uint8_t uart_rx_flag = 0; /* 收到新字节标志 */

/**
  * @brief  串口接收中断服务函数（向量名必须与启动文件一致，不能改名）
  * @note   放在本模块里封装，stm32f4xx_it.c 不再单独写。
  *         收到一个字节：保存到 uart_rx_byte，置 uart_rx_flag=1，
  *         主循环检测到后处理并清标志。
  *          函数名走 uart.h 的 UART_IRQHandler 宏（现在是 USART2_IRQHandler）——
  *           换串口时不用记得改这里，改宏就行。
  */
/* SR后读DR，按STM32要求清RXNE/ORE/FE/NE/PE；不写状态位掩盖丢字。 */
void UART_IRQHandler(void)
{
    uint32_t status = UART->SR;
    uint8_t byte;
    if (status & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE))
    {
        byte = (uint8_t)UART->DR;
        if (status & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE))
            UART_RxFeed(0u, 1u, GetTick_ms());
        else if (status & USART_SR_RXNE)
        {
            uart_rx_byte = byte;
            uart_rx_flag = 1;
            UART_RxFeed(byte, 0u, GetTick_ms());
        }
    }
}

int UART_GetLine(char *out, uint16_t cap)
{
    return UART_RxGetLine(out, cap, GetTick_ms());
}
