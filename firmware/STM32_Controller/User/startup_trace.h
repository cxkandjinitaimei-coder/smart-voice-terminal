/**
 * @file startup_trace.h
 * 用途：C 运行库初始化前的最小串口启动诊断标记接口。
 * 使用：供 system_stm32f4xx.c 使用，帮助区分时钟、向量迁移与进入 main 前的故障。
 * 时序与边界：此阶段不能依赖已初始化的全局变量或复杂标准库；保持诊断实现最小化。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef STARTUP_TRACE_H
#define STARTUP_TRACE_H
#include "stm32f4xx.h"
/* Polling USART2/PA2 diagnostics. No RAM globals, DMA, timer or printf needed.
 * Init is only for early startup/fatal faults, never normal DMA operation. */
void StartupTrace_Init(void);
void StartupTrace_Print(const char *text);
void StartupTrace_Hex(uint32_t value);
#endif
