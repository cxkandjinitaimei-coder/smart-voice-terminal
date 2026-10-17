/**
 * @file time.h
 * 用途：TIM6 毫秒全局时基及初始化/延时辅助接口。
 * 使用：系统时钟配置后初始化；主循环以 GetTick_ms 决定各任务是否到期。
 * 时序与边界：无 RTOS，不提供抢占；比较时间应使用无符号差或带符号截止差处理回绕。忙等延时仍占前台。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef _TIME_H_
#define _TIME_H_

#include "stm32f4xx.h"

/* ==================================================================
 * 系统时基模块（TIM6 → 1ms 更新中断）
 * 配置（移植到其他芯片，只改下面的宏，.c 不用动）：
 *   TIM6 在 APB1 总线（84MHz），PSC+1=84 → 计数时钟 1MHz，
 *   ARR=999 → 更新中断周期 = 1000 × 1µs = 1ms。
 *    中断服务函数名 TIM6_DAC_IRQHandler 由启动文件固定，不能改。
 * ================================================================== */
#define SYS_TIM TIM6                    /* 时基定时器 */
#define SYS_TIM_CLK RCC_APB1Periph_TIM6 /* 定时器时钟使能宏 */
#define SYS_TIM_IRQn TIM6_DAC_IRQn      /* NVIC 中断号 */
#define SYS_TICK_PSC 83                 /* 84MHz / 84 = 1MHz（1µs/次） */
#define SYS_TICK_ARR 999                /* 1000 × 1µs = 1ms */

/* 系统毫秒计数器（由 TIM6 更新中断累加，1ms 时基） */
extern volatile uint32_t timer_ms;

/* 系统时基（TIM6）：全工程唯一的时间源
 * 任何硬件模块需要非阻塞定时时，调用 GetTick_ms() 自行管理状态即可。 */
void TIM6_Init(void);      /* 初始化 TIM6：产生 1ms 更新中断 */
uint32_t GetTick_ms(void); /* 获取当前系统毫秒数 */

/* 每 1ms 调用的应用钩子；默认空实现。禁止阻塞、打印、刷屏。 */
void System_TickHook(void);

#endif
