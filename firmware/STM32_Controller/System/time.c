//#include "time.h"

//// ========== 全局变量 ==========
//volatile uint32_t timer_ms = 0;   // 系统毫秒计数器（TIM6 更新中断累加）

//// ========== 时钟说明 ==========
//// 系统时钟 168MHz，AHB 预分频 1 → HCLK = 168MHz
//// APB1 预分频 4 → APB1 = 42MHz，APB1 定时器时钟 = 42MHz×2 = 84MHz（TIM6/TIM2/TIM3）
//// APB2 预分频 2 → APB2 = 84MHz，APB2 定时器时钟 = 84MHz×2 = 168MHz（TIM1/TIM8）

//// ==================================================================
//// TIM6：系统 1ms 时基（定时器/分频/重载值见 time.h 的宏）
//// ==================================================================
//// 定时器时钟 84MHz，预分频 83 → 计数时钟 1MHz（1µs/次）
//// 自动重载 999 → 中断周期 = 1000 × 1µs = 1ms
////
//// ⚠️ 本模块只做"时间"这一件事：任何硬件模块想基于时间做非阻塞逻辑
////   （闪烁/呼吸/测距节拍等），都调用 GetTick_ms()，各自管理自己的状态。
////   之前放在这里的 LED 闪烁定时器（TIM2/3/8）已移回 01LED 模块，
////   这里不再依赖任何 Hardware 文件。
//void TIM6_Init(void)
//{
//    TIM_TimeBaseInitTypeDef TIM_TimeBaseStruct;
//    NVIC_InitTypeDef NVIC_InitStruct;

//    // 使能 TIM6 时钟（APB1 总线）
//    RCC_APB1PeriphClockCmd(SYS_TIM_CLK, ENABLE);

//    TIM_TimeBaseStruct.TIM_Period = SYS_TICK_ARR;          // 自动重载值 → 1ms
//    TIM_TimeBaseStruct.TIM_Prescaler = SYS_TICK_PSC;       // 84MHz / 84 = 1MHz
//    TIM_TimeBaseStruct.TIM_ClockDivision = 0;
//    TIM_TimeBaseStruct.TIM_CounterMode = TIM_CounterMode_Up;
//    TIM_TimeBaseInit(SYS_TIM, &TIM_TimeBaseStruct);

//    // 使能更新中断
//    TIM_ITConfig(SYS_TIM, TIM_IT_Update, ENABLE);

//    // NVIC：优先级最高，作为全系统时基
//    NVIC_InitStruct.NVIC_IRQChannel = SYS_TIM_IRQn;
//    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 0;
//    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
//    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
//    NVIC_Init(&NVIC_InitStruct);

//    // 启动 TIM6
//    TIM_Cmd(SYS_TIM, ENABLE);
//}

//// 获取当前系统毫秒数
//uint32_t GetTick_ms(void)
//{
//    return timer_ms;
//}

//void TIM6_DAC_IRQHandler(void)
//{
//    if(TIM_GetITStatus(SYS_TIM, TIM_IT_Update) != RESET)
//    {
//        TIM_ClearITPendingBit(SYS_TIM, TIM_IT_Update);
//        timer_ms++;
//    }
//}


//void delay_ms(){}



#include "time.h"

// ========== 全局变量 ==========
volatile uint32_t timer_ms = 0;   // 系统毫秒计数器（TIM6 更新中断累加）

// ========== 时钟说明 ==========
// 系统时钟 168MHz，AHB 预分频 1 → HCLK = 168MHz
// APB1 预分频 4 → APB1 = 42MHz，APB1 定时器时钟 = 42MHz×2 = 84MHz（TIM6/TIM2/TIM3）
// APB2 预分频 2 → APB2 = 84MHz，APB2 定时器时钟 = 84MHz×2 = 168MHz（TIM1/TIM8）

// ==================================================================
// TIM6：系统 1ms 时基（定时器/分频/重载值见 time.h 的宏）
// ==================================================================
// 定时器时钟 84MHz，预分频 83 → 计数时钟 1MHz（1µs/次）
// 自动重载 999 → 中断周期 = 1000 × 1µs = 1ms
//
// ⚠️ 本模块只做"时间"这一件事：任何硬件模块想基于时间做非阻塞逻辑
//   （闪烁/呼吸/测距节拍等），都调用 GetTick_ms()，各自管理自己的状态。
//   之前放在这里的 LED 闪烁定时器（TIM2/3/8）已移回 01LED 模块，
//   这里不再依赖任何 Hardware 文件。
void TIM6_Init(void)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStruct;
    NVIC_InitTypeDef NVIC_InitStruct;

    // 使能 TIM6 时钟（APB1 总线）
    RCC_APB1PeriphClockCmd(SYS_TIM_CLK, ENABLE);

    TIM_TimeBaseStruct.TIM_Period = SYS_TICK_ARR;          // 自动重载值 → 1ms
    TIM_TimeBaseStruct.TIM_Prescaler = SYS_TICK_PSC;       // 84MHz / 84 = 1MHz
    TIM_TimeBaseStruct.TIM_ClockDivision = 0;
    TIM_TimeBaseStruct.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(SYS_TIM, &TIM_TimeBaseStruct);

    // 使能更新中断
    TIM_ITConfig(SYS_TIM, TIM_IT_Update, ENABLE);

    // NVIC：优先级最高，作为全系统时基
    NVIC_InitStruct.NVIC_IRQChannel = SYS_TIM_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    // 启动 TIM6
    TIM_Cmd(SYS_TIM, ENABLE);
}

// 获取当前系统毫秒数
uint32_t GetTick_ms(void)
{
    return timer_ms;
}

/* 应用可覆盖；中断内只允许短操作，时基模块不依赖具体硬件模块。 */
__weak void System_TickHook(void) {}

void TIM6_DAC_IRQHandler(void)
{
    if(TIM_GetITStatus(SYS_TIM, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(SYS_TIM, TIM_IT_Update);
        timer_ms++;
        System_TickHook();
    }
}


void delay_ms(){}
