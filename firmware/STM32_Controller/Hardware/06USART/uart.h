/**
 * @file uart.h
 * 用途：USART2 调试口初始化、printf 重定向和前台命令输入。
 * 使用：系统时钟建立后初始化，PA2=TX/PA3=RX，115200；中断仅收字节，命令在主循环处理。
 * 时序与边界：调试文本与 ESP 协议串口分离；阻塞 printf 仍消耗前台时间，录音期限制大段输出。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __UART_H
#define __UART_H

#include "stm32f4xx.h" /* 标准库 */

/* ==================================================================
 * 串口模块（ 从 USART1 换到 USART2 —— 调试打印，DMA 发送）
 * ------------------------------------------------------------------
 *  为什么从 USART1 搬走：
 *   USART1 = **PA9/PA10**，是板上 USB 串口芯片那一组。32 针 TFT 屏插进 J1
 *   之后，那组排针/USB 口**被屏物理挡住、够不着了** ⇒ 调试日志改走
 *   **USART2 = PA2/PA3**（板上标注「空闲」的两根脚，⭐⭐⭐⭐⭐）。
 *
 *    **UART4 / UART5 不能用**：PC10/PC11/PC12/PD2 和 SDIO 抢脚，会撞 SD 卡。
 *    USART3 (PB10/PB11) 是 ESP 那条链，也别占。
 *
 * 换口（换回 USART1、或以后再换别的）：**只改下面这一组宏，.c 一行都不用动**。
 *   | 项       | USART1（原）              | USART2（现在）             |
 *   |----------|---------------------------|----------------------------|
 *   | 外设     | USART1                    | USART2                     |
 *   | 引脚     | PA9 = TX / PA10 = RX      | PA2 = TX / PA3 = RX        |
 *   | 时钟     | RCC_APB2Periph_USART1     | RCC_APB1Periph_USART2      |
 *   | 时钟函数 | RCC_**APB2**PeriphClockCmd| RCC_**APB1**PeriphClockCmd |
 *   | 复用     | GPIO_AF_USART1            | GPIO_AF_USART2             |
 *   | 中断     | USART1_IRQn / _IRQHandler | USART2_IRQn / _IRQHandler  |
 *   | TX DMA   | DMA2_Stream7 / CH4 / TCIF7| DMA1_Stream6 / CH4 / TCIF6 |
 *   ⇒ `UART_CLK_CMD` 和 `UART_TX_DMA_TCIF` 就是为"少踩这两个坑"才单独抽出来的宏
 *     （总线不同 → 时钟函数名不同；Stream 号不同 → 标志位名不同，写错都不报错、只是哑）。
 *
 * 发送用 DMA（非阻塞）：printf/send_* 只把字节写进环形缓冲，
 *   DMA 从缓冲搬到 UART->DR，发完中断续发。
 *   接收仍用中断（UART_IRQHandler → 1024字节缓冲（UART_GetLine取行）；旧byte/flag仅兼容保留）。
 * ================================================================== */

/* ---------- 外设 / 引脚 / 时钟 ---------- */
#define UART USART2                         /* 串口外设 */
#define UART_CLK RCC_APB1Periph_USART2      /* 外设时钟使能宏 */
#define UART_CLK_CMD RCC_APB1PeriphClockCmd /*  USART2 在 APB1（USART1 才是 APB2） */
#define UART_GPIO_CLK RCC_AHB1Periph_GPIOA  /* GPIO 时钟使能宏 */
#define UART_PORT GPIOA                     /* GPIO 端口 */
#define UART_TX_PIN GPIO_Pin_2              /* TX 引脚 */
#define UART_RX_PIN GPIO_Pin_3              /* RX 引脚 */
#define UART_TX_SRC GPIO_PinSource2         /* TX 引脚源 */
#define UART_RX_SRC GPIO_PinSource3         /* RX 引脚源 */
#define UART_AF GPIO_AF_USART2              /* 复用功能编号（AF7） */
#define UART_IRQn USART2_IRQn               /* 接收中断号 */
#define UART_IRQHandler USART2_IRQHandler   /*  向量名必须与启动文件一致 */

/* ---------- DMA 发送（USART2_TX = DMA1_Stream6 / Channel4） ---------- */
#define UART_TX_DMA_STREAM DMA1_Stream6
#define UART_TX_DMA_CLK RCC_AHB1Periph_DMA1
#define UART_TX_DMA_CHANNEL DMA_Channel_4
#define UART_TX_DMA_IRQn DMA1_Stream6_IRQn
#define UART_TX_DMA_IRQHandler DMA1_Stream6_IRQHandler
#define UART_TX_DMA_TCIF                                                                           \
    DMA_IT_TCIF6 /*  跟着 Stream 编号走（S6→TCIF6），写错不报错、只是永远不续发 */

#define UART_TX_BUF_SIZE 256 /* 发送环形缓冲（2 的幂） */

/* ---------- 串口参数 ---------- */
/* 115200-8-N-1。标准库 USART_Init 内部按实际 PCLK 自动算 BRR，
 * 所以 APB1(42MHz) 和 APB2(84MHz) 都不用改这里。 */
#define UART_BAUD 115200

/* ==================== 函数声明 ==================== */
void UART_Init(void); /* 初始化 USART2（PA2/PA3，115200，DMA 发送+中断接收） */
void send_data(const char *data,
               uint16_t len); /* 发送 len 字节原始数据（非阻塞 DMA，写环形缓冲） */
void send_str(const char *str); /* 发送字符串（遇 '\0' 自动结束，非阻塞 DMA） */

/* 主循环取一行：1=收到，0=等待，-1=过长/丢字/错误，整行已丢弃。 */
int UART_GetLine(char *out, uint16_t cap);

/* ---------- 接收变量（RXNE 中断填充，见 uart.c） ---------- */
extern volatile uint8_t uart_rx_byte; /* 最新收到的字节 */
extern volatile uint8_t uart_rx_flag; /* 收到新字节标志（=1 待处理） */

#endif /* __UART_H */
