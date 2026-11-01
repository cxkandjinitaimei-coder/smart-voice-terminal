#ifndef __KEY_H
#define __KEY_H
#include "stm32f4xx.h"

/* K-UP PA0 high-active; additional board PE3 low-active. Both use 25ms debounce. */
#define KEY_UP_PIN           GPIO_Pin_0
#define KEY_UP_PORT          GPIOA
#define KEY_UP_CLK           RCC_AHB1Periph_GPIOA
#ifndef KEY_UP_ACTIVE_LEVEL
#define KEY_UP_ACTIVE_LEVEL  1u
#endif
#if KEY_UP_ACTIVE_LEVEL != 0 && KEY_UP_ACTIVE_LEVEL != 1
#error KEY_UP_ACTIVE_LEVEL_must_be_0_or_1
#endif
#define KEY_UP_DEBOUNCE_MS   25u

/* Additional board key: PE3, default low when pressed.
 * It is initialized after LCD_Init(). This product only uses FSMC A18 on PD13
 * for the LCD register select; PE3 is available on this board. */
#define KEY_OTA_PIN          GPIO_Pin_3
#define KEY_OTA_PORT         GPIOE
#define KEY_OTA_CLK          RCC_AHB1Periph_GPIOE
#ifndef KEY_OTA_ACTIVE_LEVEL
#define KEY_OTA_ACTIVE_LEVEL 0u
#endif
#if KEY_OTA_ACTIVE_LEVEL != 0 && KEY_OTA_ACTIVE_LEVEL != 1
#error KEY_OTA_ACTIVE_LEVEL_must_be_0_or_1
#endif

/* UART 初始化后调用。上电按住不会触发，必须先稳定松开才接受下一次按下。 */
void KEY_UP_Init(void);
/* TIM6 的应用钩子每 1ms 调用；只采样和记事件，严禁在中断中刷屏/printf。 */
void KEY_UP_Tick1ms(void);
/* 主循环取出一次已消抖按下事件，刷屏/读卡期间发生的事件也会保留。 */
uint8_t KEY_UP_Scan(void);
uint8_t KEY_UP_Level(void);
uint8_t KEY_UP_Ready(void);
void KEY_OTA_Init(void);
void KEY_OTA_Tick1ms(void);
uint8_t KEY_OTA_Scan(void);
#endif
