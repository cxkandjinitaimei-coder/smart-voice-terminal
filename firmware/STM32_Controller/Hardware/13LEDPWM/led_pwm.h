/**
 * @file led_pwm.h
 * 用途：TIM3 通道的 LED PWM 亮度控制。
 * 使用：初始化 PA6/PA7 及定时器，再由命令层设置占空比。
 * 时序与边界：通道和计数周期固定在驱动中；不要与其他使用 TIM3 的外设重复初始化。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __LED_PWM_H
#define __LED_PWM_H

#include "stm32f4xx.h"

/* ==================================================================
 * 板载双 LED PWM 调光（PA6/PA7，低电平点亮）
 * 用途：亮度随蓝牙下发的角度变化 —— LED1(PA6)↔角度1，LED2(PA7)↔角度2，
 *       角度 0~180° 线性映射到 CCR 0~999。
 *
 * 引脚/定时器（PA6/PA7 唯一的 PWM 复用是 TIM3_CH1/CH2，AF2）：
 *   LED1 = PA6 = TIM3_CH1（AF2）
 *   LED2 = PA7 = TIM3_CH2（AF2）
 *   TIM3 = APB1(84MHz)，PSC=83 → 1MHz，ARR=999 → PWM 1kHz（无频闪）。
 *
 *  TIM3 原本被超声波 hcsr04(PC8 输入捕获)占用；本功能改作 LED PWM，
 *    两者同时使用会抢定时器，**不要再调用 HCSR04_Init**。
 *  本板 LED 低电平点亮 → 极性 Low，CCR 越大低电平占空比越高 = 越亮。
 *    若实物方向相反（角度越大反而越暗），把 LED_PWM_POLARITY 改为
 *    TIM_OCPolarity_High 即可，不用动 .c。
 * ================================================================== */

/* ---------- 定时器 ---------- */
#define LED_PWM_TIM TIM3
#define LED_PWM_TIM_CLK RCC_APB1Periph_TIM3

/* ---------- PWM 参数 ---------- */
#define LED_PWM_PSC 83                      /* 84MHz / 84 = 1MHz（1µs/次） */
#define LED_PWM_ARR 999                     /* 1000µs → PWM 1kHz */
#define LED_PWM_POLARITY TIM_OCPolarity_Low /* 低电平点亮：CCR 越大越亮 */

/* ---------- LED1：PA6 = TIM3_CH1（AF2） ---------- */
#define LED_PWM1_PIN GPIO_Pin_6
#define LED_PWM1_PORT GPIOA
#define LED_PWM1_GPIO_CLK RCC_AHB1Periph_GPIOA
#define LED_PWM1_PIN_SRC GPIO_PinSource6
#define LED_PWM1_AF GPIO_AF_TIM3
#define LED_PWM1_CH 1

/* ---------- LED2：PA7 = TIM3_CH2（AF2） ---------- */
#define LED_PWM2_PIN GPIO_Pin_7
#define LED_PWM2_PORT GPIOA
#define LED_PWM2_GPIO_CLK RCC_AHB1Periph_GPIOA
#define LED_PWM2_PIN_SRC GPIO_PinSource7
#define LED_PWM2_AF GPIO_AF_TIM3
#define LED_PWM2_CH 2

/* ---------- 函数声明 ---------- */
void LED_PWM_Init(void); /* 初始化 PA6/PA7 双通道 PWM，初始全灭 */
void LED_PWM_SetBrightness(uint8_t ch, uint16_t level); /* ch=1/2，level 0~999 */

#endif /* __LED_PWM_H */
