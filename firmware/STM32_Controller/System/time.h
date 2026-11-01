////#ifndef _TIME_H_
////#define _TIME_H_

////#include "stm32f4xx.h"

/////* ==================================================================
//// * 系统时基模块（TIM6 → 1ms 更新中断）
//// * 配置（移植到其他芯片，只改下面的宏，.c 不用动）：
//// *   TIM6 在 APB1 总线（84MHz），PSC+1=84 → 计数时钟 1MHz，
//// *   ARR=999 → 更新中断周期 = 1000 × 1µs = 1ms。
//// *   ⚠️ 中断服务函数名 TIM6_DAC_IRQHandler 由启动文件固定，不能改。
//// * ================================================================== */
////#define SYS_TIM       TIM6                  /* 时基定时器 */
////#define SYS_TIM_CLK   RCC_APB1Periph_TIM6   /* 定时器时钟使能宏 */
////#define SYS_TIM_IRQn  TIM6_DAC_IRQn         /* NVIC 中断号 */
////#define SYS_TICK_PSC  83                    /* 84MHz / 84 = 1MHz（1µs/次） */
////#define SYS_TICK_ARR  999                   /* 1000 × 1µs = 1ms */

/////* 系统毫秒计数器（由 TIM6 更新中断累加，1ms 时基） */
////extern volatile uint32_t timer_ms;

/////* 系统时基（TIM6）：全工程唯一的时间源
//// * 任何硬件模块需要非阻塞定时时，调用 GetTick_ms() 自行管理状态即可。 */
////void TIM6_Init(void);        /* 初始化 TIM6：产生 1ms 更新中断 */
////uint32_t GetTick_ms(void);   /* 获取当前系统毫秒数 */

////#endif
//#ifndef __SERVO_H
//#define __SERVO_H

//#include "stm32f4xx.h"

///* ==================================================================
// * 舵机模块（PWM 控制，50Hz）
// * 引脚/定时器（移植到其他板子/芯片，只改下面的宏，.c 不用动）：
// *   信号线 PA2 = TIM2_CH3（AF1），GPIOA + TIM2 均在 APB1。
// *   计数时钟 1MHz（84MHz / PSC+1 = 84），ARR=19999 → 周期 20ms → 50Hz。
// *   CCR = 脉宽（µs）：0.5ms→0°、1.5ms→90°、2.5ms→180°（SG90）。
// *
// * ⚠️ 供电：舵机用 5V 单独供电（电流大，别从板子 3.3V 取电），
// *    信号线接 PA2，舵机 GND 与板子共地。
// * ================================================================== */

///* ---------- 引脚 / 定时器 ---------- */
//#define SERVO_PIN        GPIO_Pin_2
//#define SERVO_PORT       GPIOA
//#define SERVO_GPIO_CLK   RCC_AHB1Periph_GPIOA
//#define SERVO_PIN_SRC    GPIO_PinSource2
//#define SERVO_AF         GPIO_AF_TIM2

//#define SERVO_TIM        TIM2
//#define SERVO_TIM_CLK    RCC_APB1Periph_TIM2
//#define SERVO_CH         3              /* 通道号（CH3） */

///* ---------- PWM 参数 ---------- */
//#define SERVO_PSC        83             /* 84MHz / 84 = 1MHz（1µs/次） */
//#define SERVO_ARR        19999          /* 20ms → 50Hz */
//#define SERVO_INIT_US    1500           /* 初始脉宽 1.5ms = 90°（中位） */

///* ---------- 角度 → 脉宽映射 ---------- */
//#define SERVO_MAX_DEG    180
//#define SERVO_PULSE_MIN_US   500        /* 0° 对应脉宽（µs） */
//#define SERVO_PULSE_MAX_US   2500       /* 180° 对应脉宽（µs） */

///* ---------- 备用功能参数 ---------- */
///* 0°/90°/180° 三态循环切换（封装备用，暂不在 main 中调用）：
//   主循环里周期调用 Servo_Angle3_Update()，每 SERVO_ANGLE3_INTERVAL_MS
//   切换一次角度：0°→90°→180°→0°→… 循环。与连续扫描互斥。 */
//#define SERVO_ANGLE3_INTERVAL_MS   1000   /* 每个状态停留 1s */

///* 连续往返扫描 0°↔180°（封装备用，暂不在 main 中调用）：
//   主循环里周期调用 Servo_Sweep_Update()，每 SERVO_SWEEP_STEP_MS 挪
//   SERVO_SWEEP_STEP_DEG，从 0° 扫到 180° 再扫回来。与三态切换互斥。 */
//#define SERVO_SWEEP_STEP_DEG    1    /* 每步角度 */
//#define SERVO_SWEEP_STEP_MS     20   /* 每步间隔（ms） */

///* ---------- 函数声明 ---------- */
//void Servo_Init(void);                /* 初始化 PA2 = TIM2_CH3，50Hz PWM，初始 90°（中位） */
//void Servo_SetAngle(uint16_t angle);  /* 设角度 0~180°，线性映射到脉宽 */
//void Servo_Angle3_Update(void);       /* 备用：0°/90°/180° 三态循环（非阻塞，需周期调用） */
//void Servo_Sweep_Update(void);        /* 备用：连续往返扫描（非阻塞，需周期调用） */

//#endif



#ifndef _TIME_H_
#define _TIME_H_

#include "stm32f4xx.h"

/* ==================================================================
 * 系统时基模块（TIM6 → 1ms 更新中断）
 * 配置（移植到其他芯片，只改下面的宏，.c 不用动）：
 *   TIM6 在 APB1 总线（84MHz），PSC+1=84 → 计数时钟 1MHz，
 *   ARR=999 → 更新中断周期 = 1000 × 1µs = 1ms。
 *   ⚠️ 中断服务函数名 TIM6_DAC_IRQHandler 由启动文件固定，不能改。
 * ================================================================== */
#define SYS_TIM       TIM6                  /* 时基定时器 */
#define SYS_TIM_CLK   RCC_APB1Periph_TIM6   /* 定时器时钟使能宏 */
#define SYS_TIM_IRQn  TIM6_DAC_IRQn         /* NVIC 中断号 */
#define SYS_TICK_PSC  83                    /* 84MHz / 84 = 1MHz（1µs/次） */
#define SYS_TICK_ARR  999                   /* 1000 × 1µs = 1ms */

/* 系统毫秒计数器（由 TIM6 更新中断累加，1ms 时基） */
extern volatile uint32_t timer_ms;

/* 系统时基（TIM6）：全工程唯一的时间源
 * 任何硬件模块需要非阻塞定时时，调用 GetTick_ms() 自行管理状态即可。 */
void TIM6_Init(void);        /* 初始化 TIM6：产生 1ms 更新中断 */
uint32_t GetTick_ms(void);   /* 获取当前系统毫秒数 */

/* 每 1ms 调用的应用钩子；默认空实现。禁止阻塞、打印、刷屏。 */
void System_TickHook(void);

#endif

