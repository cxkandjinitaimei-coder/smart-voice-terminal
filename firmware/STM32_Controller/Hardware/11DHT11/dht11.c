/**
 * @file dht11.c
 * 用途：PE0 单总线温湿度读取及应答/校验错误分类。
 * 使用：初始化引脚后按周期读取；成功时更新测量值，失败保留错误码供状态上报。
 * 时序与边界：起始脉冲与逐位采样依赖微秒延时；传感器读取不可过频，超时不应阻塞主循环无限等待。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "dht11.h"
#include <stdio.h>
#include "uart.h"
#include "../../System/time.h"
#include "esp_uart.h"

/* ==================================================================
 * DHT11 温湿度传感器驱动（单总线）
 * ------------------------------------------------------------------
 *  当前 `DHT11_PIN_ASSIGNED = 0`（**未分配引脚**，见 dht11.h 头部）：
 *    下面那段真驱动**整段不参与编译**，只编译最底下的空壳函数。
 *    两个理由：
 *       原来写的 **PG9/GPIOG 在本板（F407VET6 / 100 脚）没有这个端口**，
 *         留着它就是个"看着能跑、实际一个数都读不到"的坑；
 *       空壳能让调用方一眼看出"这功能还没接线"，而不是拿到乱跳的假数据。
 *    ⇒ 接线定下来后按 dht11.h 头部那 3 步开启即可，驱动逻辑不用重写。
 *
 * 真驱动（下面 #if 里那段）的要点，留着备用：
 *  - 开漏输出 + 上拉，发完起始信号直接读引脚，不用切模式；
 *  - 读全程关中断，防止 1ms 时基等中断打乱微秒级时序；
 *  - 所有等待循环都带超时，总线卡死也不会挂住程序；
 *  - 微秒延时用 DWT 周期计数器（168MHz 下够准）。
 * ================================================================== */

DHT11_Data dht11;
static uint8_t s_initialized;
static uint8_t s_last_result = 255u;
uint8_t DHT11_InitOK(void)
{
    return s_initialized;
}
uint8_t DHT11_LastResult(void)
{
    return s_last_result;
}

#if DHT11_PIN_ASSIGNED
/* ==================== 真驱动（分配引脚后才编译） ==================== */

/* ---------- 微秒延时（DWT 周期计数器） ---------- */
static void delay_us_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000);
    while ((DWT->CYCCNT - start) < ticks)
        ;
}

/* ---------- 引脚电平读取（开漏模式下直接读） ---------- */
static uint8_t dht11_read_level(void)
{
    return GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN);
}

/**
 * @brief  GPIO 初始化：DATA 脚配成开漏输出 + 上拉
 */
void DHT11_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    delay_us_init();

    RCC_AHB1PeriphClockCmd(DHT11_CLK, ENABLE);

    GPIO_InitStruct.GPIO_Pin = DHT11_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_OD; /* 开漏，可拉低也可读 */
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;   /* 上拉，空闲为高 */
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(DHT11_PORT, &GPIO_InitStruct);

    GPIO_SetBits(DHT11_PORT, DHT11_PIN); /* 总线空闲置高 */
    s_initialized = 1u;
    s_last_result = 255u;
}

/**
 * @brief  读取一次温湿度
 * @retval 0=成功  1=无响应  2=校验失败  3=超时
 */
static uint8_t dht11_read_raw(void)
{
    uint8_t i;
    uint8_t data[5] = {0};
    uint32_t t0;

    /* 1. 发送起始信号：拉低 30ms，再释放 20~40us。
     * 这段等待不需要关中断；否则会让 USART6 在 30ms 内无法收字节。 */
    GPIO_ResetBits(DHT11_PORT, DHT11_PIN);
    delay_us(30000); /* 30ms，对慢速模块更宽容 */
    GPIO_SetBits(DHT11_PORT, DHT11_PIN);
    delay_us(30); /* 20~40us */

    /* 从响应脉冲开始才屏蔽中断，保护后面的约 4ms 微秒采样窗口。 */
    __disable_irq();

    /* 2. 等待 DHT11 响应：先低 80us 再高 80us（1ms 超时） */
    t0 = DWT->CYCCNT;
    while (dht11_read_level() == SET) /* 等 DHT11 拉低（开始响应） */
    {
        if ((DWT->CYCCNT - t0) > (SystemCoreClock / 1000))
        {
            __enable_irq();
            return 1;
        } /* 1ms 无响应 */
    }
    t0 = DWT->CYCCNT;
    while (dht11_read_level() == RESET) /* 等响应低电平结束 */
    {
        if ((DWT->CYCCNT - t0) > (SystemCoreClock / 1000))
        {
            __enable_irq();
            return 3;
        }
    }
    t0 = DWT->CYCCNT;
    while (dht11_read_level() == SET) /* 等响应高电平结束 */
    {
        if ((DWT->CYCCNT - t0) > (SystemCoreClock / 1000))
        {
            __enable_irq();
            return 3;
        }
    }

    /* 3. 读取 40 位数据（每 8 位一组：湿度整/小、温度整/小、校验） */
    for (i = 0; i < 40; i++)
    {
        t0 = DWT->CYCCNT;
        while (dht11_read_level() == RESET) /* 每位起始的 50us 低电平 */
        {
            if ((DWT->CYCCNT - t0) > (SystemCoreClock / 1000))
            {
                __enable_irq();
                return 3;
            }
        }
        delay_us(40); /* 高电平保持 40us 后采样 */
        if (dht11_read_level() == SET)
        {
            data[i / 8] = (data[i / 8] << 1) | 0x01; /* '1': 高电平约 70us */
            t0 = DWT->CYCCNT;
            while (dht11_read_level() == SET) /* 等该位结束 */
            {
                if ((DWT->CYCCNT - t0) > (SystemCoreClock / 1000))
                {
                    __enable_irq();
                    return 3;
                }
            }
        }
        else
        {
            data[i / 8] = (data[i / 8] << 1); /* '0': 高电平约 26us */
        }
    }

    /* 读完重新开中断 */
    __enable_irq();

    /* 4. 校验：前 4 字节之和低 8 位 = 第 5 字节 */
    if (data[4] == (uint8_t)(data[0] + data[1] + data[2] + data[3]))
    {
        dht11.humi_int = data[0];
        dht11.humi_dec = data[1];
        dht11.temp_int = data[2];
        dht11.temp_dec = data[3];
        dht11.checksum = data[4];
        return 0;
    }
    return 2;
}

uint8_t DHT11_Read(void)
{
    s_last_result = dht11_read_raw();
    return s_last_result;
}

/**
 * @brief  硬件自检：测驱动与 DATA 线电平
 *
 * 正常结果：
 *   Drive LOW  -> read 0
 *   Drive HIGH -> read 1
 */
void DHT11_Diag(void)
{
    /* 1. 空闲电平（未发信号，线上应该被上拉拉高） */
    printf("DHT11 Diag: 空闲电平 = %d (期望 1)\r\n", GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN));

    /* 2. 拉低测试：开漏输出应该能把线拉低 */
    GPIO_ResetBits(DHT11_PORT, DHT11_PIN);
    delay_us(200);
    printf("DHT11 Diag: 驱动低 -> 读回 %d (期望 0)\r\n",
           GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN));

    /* 3. 释放测试：线上拉应该回高 */
    GPIO_SetBits(DHT11_PORT, DHT11_PIN);
    delay_us(200);
    printf("DHT11 Diag: 释放高 -> 读回 %d (期望 1)\r\n",
           GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN));
}

/**
 * @brief  打印温湿度到串口
 */
void DHT11_Print(void)
{
    printf("DHT11: 温度 %d.%d C, 湿度 %d.%d %%RH\r\n", dht11.temp_int, dht11.temp_dec,
           dht11.humi_int, dht11.humi_dec);

    /* 结构化上报给 ESP，ESP 会把整行放进下一次 /api/data 的 sensor 字段。
     * 只在校验通过后调用本函数，因此不会把半包或旧数据上传到云端。 */
    {
        char line[64];
        sprintf(line, "@SENSOR:温度:%u.%u,湿度:%u.%u\n", (unsigned)dht11.temp_int,
                (unsigned)dht11.temp_dec, (unsigned)dht11.humi_int, (unsigned)dht11.humi_dec);
        ESP_UART_SendStr(line);
    }
}

/* 每 2 秒读一次 DHT11 并打印到串口（DHT11 采样周期 1~2Hz） */
static uint32_t dht11_last = 0;

void DHT11_sys(void)
{
    if (GetTick_ms() - dht11_last >= 2000)
    {
        dht11_last = GetTick_ms();
        switch (DHT11_Read())
        {
        case 0:
            DHT11_Print();
            break;
        case 1:
            printf("DHT11: 无响应(ERR1)\r\n");
            break; /* 硬件问题 */
        case 2:
            printf("DHT11: 校验失败(ERR2)\r\n");
            break; /* 时序问题 */
        default:
            printf("DHT11: 超时(ERR3)\r\n");
            break; /* 总线卡住 */
        }
    }
}

#else
/* ============ 未分配引脚：空壳（不会误报、不给假数据） ============ */

void DHT11_Init(void)
{
    /* 未分配引脚 —— 什么都不做。见 dht11.h 头部说明。 */
}

void DHT11_Diag(void)
{
    printf("[DHT11] 未分配引脚：跳过自检。"
           "要启用请改 Hardware\\11DHT11\\dht11.h 头部那 3 步\r\n");
}

uint8_t DHT11_Read(void)
{
    return 1u; /* 1 = 无响应（等价于"没插/没接"） */
}

void DHT11_Print(void)
{
    printf("[DHT11] 未分配引脚：当前无数据\r\n");
}

void DHT11_sys(void)
{
    /* 空壳：不做周期读取，免得每 2 秒刷一行没用的日志 */
}

#endif /* DHT11_PIN_ASSIGNED */
