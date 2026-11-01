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
