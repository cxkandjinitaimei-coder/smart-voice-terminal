#ifndef STARTUP_TRACE_H
#define STARTUP_TRACE_H
#include "stm32f4xx.h"
/* Polling USART2/PA2 diagnostics. No RAM globals, DMA, timer or printf needed.
 * Init is only for early startup/fatal faults, never normal DMA operation. */
void StartupTrace_Init(void);
void StartupTrace_Print(const char *text);
void StartupTrace_Hex(uint32_t value);
#endif
