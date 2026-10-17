/**
 * @file uart_rx.h
 * 用途：调试串口接收队列和逐行提取接口。
 * 使用：接收中断写入字节，前台取出完整命令；调用前先完成 uart 初始化。
 * 时序与边界：队列满或行过长应按接口返回值处理；不要在消费完成前把共享缓冲指针交给异步任务。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef UART_RX_H
#define UART_RX_H
#include <stdint.h>
#define UART_LINE_SIZE 256u
void UART_RxReset(void);
/* Single producer: USART RX ISR. bad != 0 marks a lost/corrupt byte. */
void UART_RxFeed(uint8_t byte, uint8_t bad, uint32_t now);
/* Single consumer: main. 1=line, 0=waiting, -1=discarded damaged/long line.
 * CR/LF/CRLF or 150ms idle terminates a line. cap includes the NUL byte. */
int UART_RxGetLine(char *out, uint16_t cap, uint32_t now);
#endif
