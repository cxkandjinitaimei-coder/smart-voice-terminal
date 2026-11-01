#ifndef __ESP_UART_H
#define __ESP_UART_H

#include "stm32f4xx.h"

/* ==================================================================
 * ESP8266 专用串口 — USART6 (PC6=TX, PC7=RX)，115200
 * ------------------------------------------------------------------
 * 接线（与 NodeMCU 硬件串口对应）：
 *   F407 PC6 (USART6_TX) → ESP GPIO3 (RX)   [H1（右侧）排针 第 39 针]
 *   F407 PC7 (USART6_RX) ← ESP GPIO1 (TX)   [H1（右侧）排针 第 38 针]
 *   GND ↔ GND                               [H1（右侧）排针 第 7~10 针]
 *   ⚠️ ESP 的 VCC 必须**独立 3.3V 供电**（发射峰值 300mA+），
 *      别从 MCU 的 LDO 取 —— 现象是"连不上/反复重启"，极难归因到电源。
 *
 * ⚠️ 2026-09-18 从 USART3 (PB10/PB11) 迁到这里，原因：
 *    PB10 正是麦克风 I2S2_CK 的脚（inmp441.h:29-32），而 I2S2_CK 只有
 *    PB10 / PB13 两个可能、PB13 归屏幕触摸 ⇒ 麦克风和 ESP **物理二选一**。
 *    迁到 USART6 后这个死结自动解开，PB10 还给麦克风。
 *
 * ⚠️⚠️ USART6 挂在 **APB2**（USART2/USART3 在 APB1）。
 *     所以 ESP_USART_CLK_CMD 必须是 RCC_APB2PeriphClockCmd。
 *     写成 APB1 的后果是**不报错、只是哑** —— 时钟永远不开，串口一声不响。
 *
 * 两端均为 **921600 8N1**（2026-09-21 由 115200 提上来，为 P3 的音频流让路）。
 * ★★ **两头必须同时烧**：F407 这边改了、ESP 那边没改，就是全线乱码 ——
 *    现象是"收到一堆 error/overflow、一个字都认不出"，很容易误判成接线或电源。
 * 主循环绘屏期间由 **1024 字节环**缓存突发数据；溢出会报错并重新同步。
 * ⚠️ 旧注释说"不能靠提高波特率来解决缓冲溢出"—— 那是 921600 之前的判断：
 *    抬波特率只是把**到达更快**，真正解决溢出的是**环 + 前台取尽 + 二进制帧**
 *    （行路径的 192 字节上限和"见 0x00 判整行作废"才是老瓶颈，见下面 §二进制帧）。
 * ⚠️ 921600 下**每字节 10.85 µs**：发一块 2048 B 的音频帧 = 忙等 **22 ms**。
 *    麦克风 DMA 环一共 64 ms ⇒ 单帧阻塞发得下，但**别在流式期间干 ≥1s 的慢任务**。
 *
 * 下行（ESP→F407，行分隔 \n，帧尾 # 可选）：
 *   @PING                      返回 @PONG（不操作外设）
 *   @MOVE:servo,<id>,<angle>    id 1~16（= PCA9685 丝印号），angle 0~180
 *   @MOVE:gesture,<name>        nod / hi / look_left / look_right / shake
 *   @MOVE:expression,<face>     happy / sad / sleepy / angry / normal
 *   @TEXT:<文字>                按前缀截后面全部，**可含逗号，不要 split**
 *   @SWEEP:servo,<id>           本机非阻塞来回扫
 *   @LED_ON,LED<1|2>  /  @LED_OFF,LED<1|2>
 *   @IAP:...                   升级（02OTA 的段，见 iap_app.h；★ 2026-09-21 起
 *                              这条线**也能发 @IAP: 文本帧**了，以前只有调试口能）
 *   ⇒ 解析在 App\ailink.c。
 *   ⚠️⚠️ 认识的行才处理，**不认识的一律丢弃**，绝不能转给 Media_Command：
 *      media.c 的兜底会把任何非 '/' 开头的行直接当"要显示的文字"上屏，
 *      且**不报错** —— 一个漏过去的 @MOVE: 会连着 '@' 前缀显示在屏幕上。
 *   ESP 应用日志仅走 Serial1(GPIO2)；仍须拒绝启动乱码和未知行。
 *
 * 上行（F407→ESP）：
 *   @ACK:ok,servo,1,100
 *   @SENSOR:... / @USER:... / @ALERT:...
 *   ★ 二进制帧（音频块等）走 ESP_UART_SendFrame()，见下面 §二进制帧。
 *
 * 主循环必须 while (ESP_UART_GetMsg(...)) 取尽，避免多帧滞留。
 * ================================================================== */

/* ---------- 外设 / 引脚 / 时钟（移植到别的板子只改这一组宏） ---------- */
#define ESP_USART            USART6
#define ESP_USART_CLK        RCC_APB2Periph_USART6     /* ← APB2，不是 APB1 */
#define ESP_USART_CLK_CMD    RCC_APB2PeriphClockCmd    /* ← 同上，写错只哑不报错 */

#define ESP_TX_PIN           GPIO_Pin_6
#define ESP_TX_PORT          GPIOC
#define ESP_TX_CLK           RCC_AHB1Periph_GPIOC
#define ESP_TX_AF            GPIO_AF_USART6
#define ESP_TX_SOURCE        GPIO_PinSource6

#define ESP_RX_PIN           GPIO_Pin_7
#define ESP_RX_PORT          GPIOC
#define ESP_RX_CLK           RCC_AHB1Periph_GPIOC
#define ESP_RX_AF            GPIO_AF_USART6
#define ESP_RX_SOURCE        GPIO_PinSource7

#define ESP_IRQn             USART6_IRQn
#define ESP_IRQHandler       USART6_IRQHandler

/* ---------- 通信参数 ---------- */
/* ★ 2026-09-21：115200 → 921600（P3 音频流的前提）。
 *   算过：16k×16bit×单声道 = 32000 B/s 要传；115200 8N1 只有 11520 B/s
 *   ⇒ 裸二进制都过载 2.8×；921600 有 92160 B/s ⇒ 只用 34.7%、余量 2.88×。
 *   460800 看着够（69.4%）其实不够 —— 只剩 1.44× 余量，突发一来就丢。
 * ⚠️ 这是**板间参数**：ESP 侧 `F407_BAUD` 必须跟着改成 921600，少改一头 = 全线乱码。 */
#define ESP_BAUD             921600

/* 4096-byte ring, 16-bit indices. Must remain a power of two.
 * 2048-byte audio payload plus envelope must fit without forcing the main loop
 * to drain faster than the 921600-baud stream. */
#define ESP_RX_BUF_SIZE      4096u
#define ESP_LINE_MAX         192u
#if (ESP_RX_BUF_SIZE & (ESP_RX_BUF_SIZE - 1u)) || (ESP_RX_BUF_SIZE > 32768u)
#error ESP_RX_BUF_SIZE_must_be_a_power_of_two_up_to_32768
#endif

/* ---------- 二进制帧（`合作表 §4.3`）----------
 * 信封（两个方向同一套，小端）：
 *   0xA5 0x5A | type(1) | seq(2) | len(2) | payload[len] | xor(1)
 *   xor = **type..payload 逐字节异或**（含 seq / len 那 4 个字节，不含两个同步字节）
 *   线上开销 = 8 + len 字节
 *
 * ★ 为什么必须另开一条路：行路径（ESP_UART_GetLine）**见 0x00 就判整行作废**
 *   （esp_uart.c 的 ch==0 分支）、且上限 192 字节 ⇒ 二进制载荷进去必死。
 *
 * type 分配（`合作表.md` §4.3）：0x01~0x0F = 01语音（音频）｜0x10~0x1F = 02OTA（IAP）。
 *   本模块**只负责收发信封**，type 的含义一概不解释 —— 由 App 层分发（ailink.c）。 */
#define ESP_FRAME_MAX        2048u   /* 单帧 payload 上限（= 一个音频块） */
#if (ESP_FRAME_MAX > 65535u)
#error ESP_FRAME_MAX_must_fit_in_uint16
#endif

#define ESP_MSG_TEXT         0u
#define ESP_MSG_FRAME        1u

typedef struct
{
    uint8_t        kind;    /* ESP_MSG_TEXT | ESP_MSG_FRAME */
    uint8_t        type;    /* 仅 FRAME：0x01~0x1F */
    uint16_t       seq;     /* 仅 FRAME */
    uint16_t       len;     /* 有效字节数（TEXT 不含结尾的 0） */
    const uint8_t *data;    /* FRAME：裸 payload，**不保证以 0 结尾**；
                             * TEXT ：以 0 结尾的行（指向模块内静态缓冲，见下） */
} esp_msg_t;

/* ⚠️ 两种 data 都指向**模块内静态缓冲**（和老的 GetLine 一样）：
 *    下一条消息一到就被覆盖 ⇒ **必须当场用完，不许存指针**。 */

extern volatile uint32_t esp_rx_bytes, esp_rx_overflows, esp_rx_errors;

extern volatile uint8_t  esp_rx_seen;
/* ISR 每收到一个字节就刷新（esp_uart.c 的 ESP_IRQHandler）。
 * 用途：ailink.c 的"静默窗" —— 等这条线上安静 50ms 再统一重绘一次屏，
 * 避免 5 帧连发时屏被连刷 5 次（每次 150~300ms，期间继续丢帧）。 */
extern volatile uint32_t esp_last_rx_ms;

void ESP_UART_Init(void);
void ESP_UART_SendByte(uint8_t byte);
void ESP_UART_SendStr(const char *str);

/* 发一帧二进制（信封见上）。**阻塞发完**（沿用 SendByte 的忙等），不进中断：
 * 在 921600 下线上 8+len 字节、每字节 10.85 µs ⇒ 一帧 2048 B 占线约 22 ms。
 * payload 允许为 0（len==0），此时 data 传 0 也行。 */
void ESP_UART_SendFrame(uint8_t type, uint16_t seq,
                        const uint8_t *payload, uint16_t len);

/* ★ 唯一的取文入口（前端主循环每圈取尽）：文本行和二进制帧都从这儿出。
 *   1 = 取到一条（看 m->kind）｜ 0 = 暂时没有 ｜ -1 = 坏帧/超长行（**已整条丢弃并对齐**）
 * 调用方必须 while 取尽，**包括 -1 之后继续取**（和老的 GetLine 同一套规矩）。
 * ★ **坏帧绝不会交出来**：xor 不过 / len 超 ESP_FRAME_MAX ⇒ 丢 + 计数，
 *   并从下一个 `A5 5A` 重新对齐（不会把半帧当文本喂给解析器）。 */
int ESP_UART_GetMsg(esp_msg_t *m);

/* 只取文本行的兼容层，**语义与老的逐字一致**（供旧调用点 / 行回归用）。
 * ★ 撞上二进制帧时**一个字节都不消费**（在本环里让出去、返回 0）⇒ 帧不会丢，
 *   但**得有人用 GetMsg 来取**，否则帧会一直压着、直到环满报 overflow
 *   （`esp_frame_defer_line` 记这种情况发生了多少次）。
 *   ⇒ **新代码一律用 GetMsg**；只有"纯文本、且同时有 GetMsg 在取"才用这个。 */
int ESP_UART_GetLine(char *out, uint16_t out_size);

extern volatile uint32_t esp_frame_ok, esp_frame_bad, esp_frame_over;   /* 收帧计数 */
extern volatile uint32_t esp_frame_defer_line;   /* GetLine 模式撞到帧、让出去的次数 */

#endif /* __ESP_UART_H */
