/**
 * @file uart_rx.c
 * 用途：调试串口接收队列和逐行提取接口。
 * 使用：接收中断写入字节，前台取出完整命令；调用前先完成 uart 初始化。
 * 时序与边界：队列满或行过长应按接口返回值处理；不要在消费完成前把共享缓冲指针交给异步任务。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "uart_rx.h"
#include <string.h>
#define RX_SIZE 1024u
#define RX_MASK (RX_SIZE - 1u)
#define RX_IDLE_MS 150u
static volatile uint8_t rx[RX_SIZE];
static volatile uint16_t head, tail;
static volatile uint32_t last_byte, faults;
static uint32_t seen_faults;
static char line[UART_LINE_SIZE];
static uint16_t used;
static uint8_t discard;

void UART_RxReset(void)
{
    head = tail = 0;
    last_byte = faults = seen_faults = 0;
    used = discard = 0;
}

void UART_RxFeed(uint8_t byte, uint8_t bad, uint32_t now)
{
    uint16_t next = (uint16_t)((head + 1u) & RX_MASK);
    last_byte = now;
    if (bad || next == tail)
    {
        faults++;
        return;
    }
    rx[head] = byte;
    head = next;
}

static int finish(char *out, uint16_t cap)
{
    int result = 0;
    if (discard || used >= cap)
        result = -1;
    else if (used)
    {
        memcpy(out, line, used);
        out[used] = 0;
        result = 1;
    }
    used = discard = 0;
    return result;
}

int UART_RxGetLine(char *out, uint16_t cap, uint32_t now)
{
    uint8_t ch;
    int result;
    if (!out || !cap)
        return -1;
    for (;;)
    {
        if (seen_faults != faults)
        {
            /* Buffered bytes have an unknown gap. Drop the burst through its
             * next boundary, never execute a truncated command. */
            seen_faults = faults;
            tail = head;
            used = 0;
            discard = 1;
        }
        if (tail == head)
            break;
        ch = rx[tail];
        tail = (uint16_t)((tail + 1u) & RX_MASK);
        if (ch == '\r' || ch == '\n')
        {
            result = finish(out, cap);
            if (result)
                return result;
        }
        else if (ch == 8u || ch == 127u)
        {
            if (used && !discard)
                used--;
        }
        else if (ch == 0u || used >= UART_LINE_SIZE - 1u)
            discard = 1;
        else if (!discard)
            line[used++] = (char)ch;
    }
    /* now may precede an ISR's last_byte by 1ms: signed ordering prevents
     * unsigned underflow being mistaken for a long idle interval. */
    if ((used || discard) && (int32_t)(now - last_byte) >= (int32_t)RX_IDLE_MS)
        return finish(out, cap);
    return 0;
}
