/**
 * @file led_pwm.c
 * 用途：TIM3 通道的 LED PWM 亮度控制。
 * 使用：初始化 PA6/PA7 及定时器，再由命令层设置占空比。
 * 时序与边界：通道和计数周期固定在驱动中；不要与其他使用 TIM3 的外设重复初始化。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "led_pwm.h"

/* ==================================================================
 * 板载双 LED PWM 调光实现（只写逻辑，所有引脚/定时器配置见 led_pwm.h）
 * 两路共用 TIM3 时基（计数时钟 1MHz，ARR=999 → 1kHz PWM），
 * 各自 CCR 独立（CH1/CH2），写一个不影响另一个。
 * ================================================================== */

/* 两路通道信息（换引脚/定时器只改 led_pwm.h，这里不动） */
typedef struct
{
    uint16_t pin;       /* 引脚号 */
    GPIO_TypeDef *port; /* 引脚所在 GPIO */
    uint32_t gpio_clk;  /* GPIO 时钟使能宏 */
    uint8_t pin_src;    /* GPIO_PinAFConfig 的引脚源 */
    uint8_t af;         /* 复用功能编号 */
    uint8_t ch;         /* 定时器通道（1/2） */
} LED_PWM_Ch_t;

static const LED_PWM_Ch_t ch_tab[2] = {
    {LED_PWM1_PIN, LED_PWM1_PORT, LED_PWM1_GPIO_CLK, LED_PWM1_PIN_SRC, LED_PWM1_AF, LED_PWM1_CH},
    {LED_PWM2_PIN, LED_PWM2_PORT, LED_PWM2_GPIO_CLK, LED_PWM2_PIN_SRC, LED_PWM2_AF, LED_PWM2_CH},
};

/* 按通道号初始化输出比较 */
static void led_pwm_oc_init(const LED_PWM_Ch_t *ch, TIM_OCInitTypeDef *oc)
{
    switch (ch->ch)
    {
    case 1:
        TIM_OC1Init(LED_PWM_TIM, oc);
        break;
    case 2:
        TIM_OC2Init(LED_PWM_TIM, oc);
        break;
    default:
        break;
    }
}

/* 按通道号写占空比（CCR） */
static void led_pwm_set_ccr(const LED_PWM_Ch_t *ch, uint16_t ccr)
{
    switch (ch->ch)
    {
    case 1:
        TIM_SetCompare1(LED_PWM_TIM, ccr);
        break;
    case 2:
        TIM_SetCompare2(LED_PWM_TIM, ccr);
        break;
    default:
        break;
    }
}

/* ==================== 对外接口 ==================== */

void LED_PWM_Init(void)
{
    uint8_t i;
    GPIO_InitTypeDef GPIO_InitStruct;
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStruct;
    TIM_OCInitTypeDef TIM_OCInitStruct;

    /* 1) 两路 GPIO 复用为 TIM3 通道输出（推挽） */
    RCC_AHB1PeriphClockCmd(LED_PWM1_GPIO_CLK | LED_PWM2_GPIO_CLK, ENABLE);

    for (i = 0; i < 2; i++)
    {
        const LED_PWM_Ch_t *ch = &ch_tab[i];

        GPIO_PinAFConfig(ch->port, ch->pin_src, ch->af);

        GPIO_InitStruct.GPIO_Pin = ch->pin;
        GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
        GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
        GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
        GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init(ch->port, &GPIO_InitStruct);
    }

    /* 2) TIM3 时基：计数时钟 1MHz，ARR=LED_PWM_ARR → PWM 1kHz */
    RCC_APB1PeriphClockCmd(LED_PWM_TIM_CLK, ENABLE);

    TIM_TimeBaseStruct.TIM_Period = LED_PWM_ARR;
    TIM_TimeBaseStruct.TIM_Prescaler = LED_PWM_PSC;
    TIM_TimeBaseStruct.TIM_ClockDivision = 0;
    TIM_TimeBaseStruct.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(LED_PWM_TIM, &TIM_TimeBaseStruct);

    /* 3) 两通道 PWM1 输出，初始全灭（CCR=0） */
    TIM_OCInitStruct.TIM_OCMode = TIM_OCMode_PWM1;
    TIM_OCInitStruct.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStruct.TIM_Pulse = 0;
    TIM_OCInitStruct.TIM_OCPolarity = LED_PWM_POLARITY;
    for (i = 0; i < 2; i++)
        led_pwm_oc_init(&ch_tab[i], &TIM_OCInitStruct);

    /* 4) 启动 TIM3 */
    TIM_Cmd(LED_PWM_TIM, ENABLE);
}

/* 设亮度：ch=1/2，level 0~LED_PWM_ARR（0=灭，999=最亮） */
void LED_PWM_SetBrightness(uint8_t ch, uint16_t level)
{
    const LED_PWM_Ch_t *c;

    if (ch < 1 || ch > 2)
        return;
    if (level > LED_PWM_ARR)
        level = LED_PWM_ARR;

    c = &ch_tab[ch - 1];
    led_pwm_set_ccr(c, level);
}
