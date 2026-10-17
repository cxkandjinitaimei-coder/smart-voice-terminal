/**
 * @file esp_uart.c
 * 用途：USART6 字节环形队列及文本/二进制混合流解析。
 * 使用：PC6=TX/PC7=RX，921600；ISR 搬字节，前台 GetMsg 解析并路由。SendFrame 统一封装。
 * 时序与边界：二进制为 A5 5A/type/seq16/len16/payload/XOR；长度先检查再写缓冲，返回数据仅在下一次解析前有效。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "esp_uart.h"
#include "time.h"
#include <string.h>

static volatile uint8_t esp_rx_buf[ESP_RX_BUF_SIZE];
static volatile uint16_t esp_rx_head = 0;
static volatile uint16_t esp_rx_tail = 0;
static volatile uint8_t rx_fault, rx_discard;
static char rx_line[ESP_LINE_MAX];
static uint16_t rx_used;
static uint8_t line_discard;
static uint8_t text_utf8_left;

/* ---- 二进制帧解析状态（前台跑，见 fx_step）---- */
#define FX_SYNC1 0u
#define FX_SYNC2 1u
#define FX_TYPE 2u
#define FX_SEQL 3u
#define FX_SEQH 4u
#define FX_LENL 5u
#define FX_LENH 6u
#define FX_PAYLOAD 7u
#define FX_XOR 8u

static uint8_t fx_state = FX_SYNC1;
static uint8_t fx_type;
static uint16_t fx_seq, fx_len, fx_got;
static uint8_t fx_xor; /* type^seq^len^payload 的滚动异或 */
static uint8_t fx_bad; /* 本帧已废（len 超限）：只计数、不落缓冲 */
static uint8_t fx_buf[ESP_FRAME_MAX];

volatile uint32_t esp_frame_ok, esp_frame_bad, esp_frame_over;
volatile uint32_t esp_frame_defer_line;

volatile uint32_t esp_rx_bytes, esp_rx_overflows, esp_rx_errors;

volatile uint8_t esp_rx_seen = 0;
volatile uint32_t esp_last_rx_ms = 0;

void ESP_UART_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    USART_InitTypeDef USART_InitStruct;
    NVIC_InitTypeDef NVIC_InitStruct;

    esp_rx_head = esp_rx_tail = 0;
    rx_fault = rx_discard = line_discard = 0;
    text_utf8_left = 0;
    rx_used = 0;
    esp_rx_seen = 0;
    esp_last_rx_ms = 0;
    esp_rx_bytes = esp_rx_overflows = esp_rx_errors = 0;
    fx_state = FX_SYNC1;
    fx_type = 0;
    fx_seq = fx_len = fx_got = 0;
    fx_xor = 0;
    fx_bad = 0;
    esp_frame_ok = esp_frame_bad = esp_frame_over = 0;
    esp_frame_defer_line = 0;

    RCC_AHB1PeriphClockCmd(ESP_TX_CLK | ESP_RX_CLK, ENABLE);
    ESP_USART_CLK_CMD(ESP_USART_CLK, ENABLE);

    GPIO_InitStruct.GPIO_Pin = ESP_TX_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(ESP_TX_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = ESP_RX_PIN;
    GPIO_Init(ESP_RX_PORT, &GPIO_InitStruct);

    GPIO_PinAFConfig(ESP_TX_PORT, ESP_TX_SOURCE, ESP_TX_AF);
    GPIO_PinAFConfig(ESP_RX_PORT, ESP_RX_SOURCE, ESP_RX_AF);

    USART_InitStruct.USART_BaudRate = ESP_BAUD;
    USART_InitStruct.USART_WordLength = USART_WordLength_8b;
    USART_InitStruct.USART_StopBits = USART_StopBits_1;
    USART_InitStruct.USART_Parity = USART_Parity_No;
    USART_InitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStruct.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(ESP_USART, &USART_InitStruct);

    USART_ITConfig(ESP_USART, USART_IT_RXNE, ENABLE);

    NVIC_InitStruct.NVIC_IRQChannel = ESP_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    USART_Cmd(ESP_USART, ENABLE);
}

void ESP_UART_SendByte(uint8_t byte)
{
    USART_SendData(ESP_USART, byte);
    while (USART_GetFlagStatus(ESP_USART, USART_FLAG_TXE) == RESET)
        ;
}

void ESP_UART_SendStr(const char *str)
{
    while (*str)
        ESP_UART_SendByte((uint8_t)*str++);
}

/* 发一帧：0xA5 0x5A | type | seq(LE) | len(LE) | payload | xor
 * xor = type..payload 逐字节异或（含 seq/len 那 4 字节，不含两个同步字节）。
 *  逐字节忙等 —— 921600 下 10.85 µs/字节 ⇒ 2048 B 载荷占线约 22 ms。
 *   麦克风 DMA 环一共 64 ms，单帧挤得下；**别在流式期间干慢任务**（见 esp_uart.h）。 */
void ESP_UART_SendFrame(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t x = type;
    uint16_t i;

    ESP_UART_SendByte(0xA5u);
    ESP_UART_SendByte(0x5Au);
    ESP_UART_SendByte(type);
    ESP_UART_SendByte((uint8_t)(seq & 0xFFu));
    ESP_UART_SendByte((uint8_t)(seq >> 8));
    ESP_UART_SendByte((uint8_t)(len & 0xFFu));
    ESP_UART_SendByte((uint8_t)(len >> 8));
    x ^= (uint8_t)(seq & 0xFFu);
    x ^= (uint8_t)(seq >> 8);
    x ^= (uint8_t)(len & 0xFFu);
    x ^= (uint8_t)(len >> 8);

    for (i = 0; i < len; i++)
    {
        uint8_t b = payload[i];
        ESP_UART_SendByte(b);
        x ^= b;
    }
    ESP_UART_SendByte(x);
}

/* 帧解析器：吃一个字节。1 = 这一帧齐了、m 已填好；0 = 还没齐。 */
static uint8_t fx_step(uint8_t ch, esp_msg_t *m)
{
    switch (fx_state)
    {
    /* 起始：第二个同步字节必须为 5A；遇到 A5 保留为新帧头候选。 */
    case FX_SYNC2:
        if (ch == 0x5Au)
            fx_state = FX_TYPE;
        else if (ch == 0xA5u)
            fx_state = FX_SYNC2; /* 可能就是下一个同步头 */
        else
        {
            esp_frame_bad++; /* 看着像帧头却不是 */
            fx_state = FX_SYNC1;
        }
        break;

    /* 类型与序号：同步完成后才开始累计 XOR，序号按小端重组。 */
    case FX_TYPE:
        fx_type = ch;
        fx_xor = ch;
        fx_state = FX_SEQL;
        break;

    case FX_SEQL:
        fx_seq = ch;
        fx_xor ^= ch;
        fx_state = FX_SEQH;
        break;

    case FX_SEQH:
        fx_seq |= (uint16_t)((uint16_t)ch << 8);
        fx_xor ^= ch;
        fx_state = FX_LENL;
        break;

    /* 长度：先收齐 len16，超限帧只消费不落缓冲，避免写越界。 */
    case FX_LENL:
        fx_len = ch;
        fx_xor ^= ch;
        fx_state = FX_LENH;
        break;

    case FX_LENH:
        fx_len |= (uint16_t)((uint16_t)ch << 8);
        fx_xor ^= ch;
        fx_got = 0;
        /* 超限的帧**不能往 fx_buf 里灌**：只计数、把 fx_len 个字节走完再丢 */
        fx_bad = (fx_len > ESP_FRAME_MAX) ? 1u : 0u;
        if (fx_bad)
            esp_frame_over++;
        fx_state = (fx_len == 0u) ? FX_XOR : FX_PAYLOAD;
        break;

    /* 数据：只写合法长度的负载，计数达到声明长度后转校验态。 */
    case FX_PAYLOAD:
        if (!fx_bad)
        {
            fx_buf[fx_got] = ch;
            fx_xor ^= ch;
        }
        fx_got++;
        if (fx_got >= fx_len) /* fx_got 最大到 65535，不会回绕 */
            fx_state = FX_XOR;
        break;

    /* 校验与交付：先复位解析状态，校验成功再发布指针；下次解析可能覆盖该缓冲。 */
    case FX_XOR:
        fx_state = FX_SYNC1;
        if (fx_bad)
            break; /* over 已经记过，不要记一次 bad */
        if (ch != fx_xor)
        {
            esp_frame_bad++;
            break;
        }
        esp_frame_ok++;
        m->kind = ESP_MSG_FRAME;
        m->type = fx_type;
        m->seq = fx_seq;
        m->len = fx_len;
        m->data = fx_buf;
        return 1u;

    default:
        fx_state = FX_SYNC1; /* 不该到这儿：对齐了重来 */
        break;
    }
    return 0u;
}

/* ==================================================================
 * 取文核心：文本行 + 二进制帧**共用一个前台解析器**
 * ------------------------------------------------------------------
 * allow_frame = 1（GetMsg）  ：帧和行都收。
 * allow_frame = 0（老 GetLine）：碰到帧**一个字节都不消费**、直接让出（返回 0），
 *   把字节留在环里等下一次 GetMsg 来取 ⇒ 帧不丢、也不用第二块 2KB 缓冲。
 *   （代价：若调用方**永远只调 GetLine**，帧会一直压着 ⇒ 环满 ⇒ esp_rx_overflows
 *     会涨。这是有意的：宁可看得见，也不要偷偷把帧吃掉。）
 * ================================================================== */
static int msg_poll(esp_msg_t *m, uint8_t allow_frame)
{
    uint8_t ch;
    uint32_t mask;

    if (m == 0)
        return 0;

    m->kind = ESP_MSG_TEXT;
    m->type = 0;
    m->seq = 0;
    m->len = 0;
    m->data = 0;

    for (;;)
    {
        /* An unknown gap invalidates the pending burst. Flush atomically;
         * the ISR discards the damaged line through its next delimiter.
         *  半帧也要一起丢：帧边界已经不可信了。 */
        mask = __get_PRIMASK();
        __disable_irq();
        if (rx_fault)
        {
            esp_rx_tail = esp_rx_head;
            rx_fault = 0;
            rx_used = 0;
            line_discard = 0;
            text_utf8_left = 0;
            fx_state = FX_SYNC1;
            fx_bad = 0;
            __set_PRIMASK(mask);
            return -1;
        }
        /* Silence may recover from an overflow with a lost delimiter.
         * Never execute an incomplete line on an idle timeout. */
        if (rx_discard && (int32_t)(GetTick_ms() - esp_last_rx_ms) >= 150)
            rx_discard = 0;
        if (esp_rx_tail == esp_rx_head)
        {
            __set_PRIMASK(mask);
            break;
        }
        /* GetLine 模式撞上帧同步头：**不消费**，让出去 */
        if (!allow_frame && !text_utf8_left && (fx_state == FX_SYNC1) &&
            (esp_rx_buf[esp_rx_tail] == 0xA5u))
        {
            __set_PRIMASK(mask);
            esp_frame_defer_line++;
            break;
        }
        ch = esp_rx_buf[esp_rx_tail];
        esp_rx_tail = (uint16_t)((esp_rx_tail + 1u) & (ESP_RX_BUF_SIZE - 1u));
        __set_PRIMASK(mask);

        /* 帧解析态：字节全给解析器，一个都不许落进行逻辑 */
        if (fx_state != FX_SYNC1)
        {
            if (fx_step(ch, m) != 0u)
                return 1;
            continue;
        }
        if (ch == 0xA5u && text_utf8_left == 0u)
        {
            /* 帧同步头。 半截文本行被帧打断 ⇒ 那半行本来就废了，直接扔；
             *  顺手清 rx_discard：帧同步比 \n 可信，而且**音频流是连发的
             *   二进制、10 秒里可能一个 \n 都没有** —— 只等 150ms 静默会
             *   把整段流吃掉。 */
            rx_used = 0;
            line_discard = 0;
            rx_discard = 0;
            text_utf8_left = 0;
            fx_state = FX_SYNC2;
            continue;
        }
        if (ch == '\n' || ch == '\r')
        {
            int result = 0;
            text_utf8_left = 0;

            if (line_discard)
                result = -1;
            else if (rx_used)
            {
                m->kind = ESP_MSG_TEXT;
                m->len = rx_used;
                /*  必须补这个 0：调用方（ailink.c 的 handle_esp_line）是按
                 * **C 串**用的（strlen / strncpy），而 rx_line 里 rx_used 之后
                 * 是**上一行的残留**。老的 GetLine 是拷贝时补的 0，改成 GetMsg
                 * 直接把 rx_line 交出去就漏了这一手 ⇒ 会读到上一行的尾巴。
                 * 下标安全：rx_used 到 ESP_LINE_MAX-1 就被判 line_discard 了。 */
                rx_line[rx_used] = 0;
                m->data = (const uint8_t *)rx_line;
                result = 1;
            }
            rx_used = 0;
            line_discard = 0;
            if (result)
                return result;
        }
        else if (ch == 0 || rx_used >= ESP_LINE_MAX - 1u)
            line_discard = 1;
        else if (!line_discard)
        {
            rx_line[rx_used++] = (char)ch;
            if (text_utf8_left && (ch & 0xC0u) == 0x80u)
                text_utf8_left--;
            else if (ch >= 0xC2u && ch <= 0xDFu)
                text_utf8_left = 1u;
            else if (ch >= 0xE0u && ch <= 0xEFu)
                text_utf8_left = 2u;
            else if (ch >= 0xF0u && ch <= 0xF4u)
                text_utf8_left = 3u;
            else
                text_utf8_left = 0u;
        }
    }
    return 0;
}

int ESP_UART_GetMsg(esp_msg_t *m)
{
    return msg_poll(m, 1u);
}

int ESP_UART_GetLine(char *out, uint16_t out_size)
{
    esp_msg_t m;
    int rc;

    if (out == 0 || out_size < 2)
        return 0;

    rc = msg_poll(&m, 0u);
    if (rc <= 0)
        return rc; /* 0 = 没有；-1 = 坏行/坏批 */

    /* rc == 1 且 allow_frame==0 ⇒ 只可能是文本行 */
    if (m.len >= out_size)
        return -1; /* 跟老语义一致：装不下 = 整行作废 */
    memcpy(out, m.data, m.len);
    out[m.len] = '\0';
    return 1;
}

void ESP_IRQHandler(void)
{
    /* STM32F4 clears ORE/FE/NE/PE by reading SR followed by DR. */
    uint32_t status = ESP_USART->SR;
    if (status & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE))
    {
        uint8_t data = (uint8_t)USART_ReceiveData(ESP_USART);
        uint16_t next;
        esp_rx_seen = 1;
        esp_last_rx_ms = GetTick_ms();
        esp_rx_bytes++;
        if (status & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE))
        {
            esp_rx_errors++;
            rx_fault = 1;
            rx_discard = 1;
            return;
        }
        if (rx_discard)
        {
            if (data == '\n' || data == '\r')
                rx_discard = 0;
            return;
        }
        next = (uint16_t)((esp_rx_head + 1u) & (ESP_RX_BUF_SIZE - 1u));
        if (next == esp_rx_tail)
        {
            esp_rx_overflows++;
            rx_fault = 1;
            rx_discard = (data != '\n' && data != '\r');
            return;
        }
        esp_rx_buf[esp_rx_head] = data;
        esp_rx_head = next;
    }
}
