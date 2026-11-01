#include "pca9685.h"
#include "time.h"      /* GetTick_ms —— 等待用；必须先 TIM6_Init() */
#include <string.h>
#include <stdio.h>     /* printf（fputc 重定向在 06USART/uart.c） */

/* ==================================================================
 * PCA9685 实现（只写逻辑，所有引脚/参数见 pca9685.h）
 *
 * 与原来写在 main.c 里的版本相比，只改了三处（其余逐字保留）：
 *   1) 每个 I2C 等待都加了**循环计数超时**。标准库的 I2C_CheckEvent()
 *      只做单次检查、本身没有超时，所以原来的 while 是纯死循环 ——
 *      PCA9685 没接时上电就卡死，串口一个字都吐不出来。
 *   2) delay_us()（直接改写 SysTick->CTRL/LOAD/VAL）换成基于
 *      GetTick_ms() 的等待，并带一个循环计数兜底。
 *   3) 补写 MODE2 的 OUTDRV=1（推挽）。原来没配，默认是**开漏**输出。
 * ================================================================== */

/* Cache actual ACKed PWM output for all callers, including /pwm. */
static uint16_t output_count[16];
static uint16_t output_valid;
uint8_t PCA9685_GetAngleEstimate(uint8_t ch,uint16_t *angle)
{
    uint16_t count;
    if(ch>=16u || !angle || !(output_valid&(1u<<ch)))return 0u;
    count=output_count[ch];
    if(count<PCA9685_CNT_AT_0DEG || count>PCA9685_CNT_AT_0DEG+PCA9685_CNT_SPAN)return 0u;
    *angle=(uint16_t)(((uint32_t)(count-PCA9685_CNT_AT_0DEG)*180u+PCA9685_CNT_SPAN/2u)/PCA9685_CNT_SPAN);
    return 1u;
}

/* ---------- 超时哨兵：循环计数，刻意不用 GetTick_ms() ----------
 * 理由：TIM6 万一没跑，tick 永不前进，拿 tick 当超时的话永远不触发，
 *       恰好在你最需要它兜底的时候失效。 */
#define PCA9685_GUARD_PER_MS   40000u   /* 空心循环的兜底系数，恒大于真实毫秒数 */

/* ------------------------------------------------------------------
 * 等一个 I2C 事件。返回 0 = 等到；1 = 超时
 * ------------------------------------------------------------------ */
static uint8_t pca_wait_event(uint32_t event)
{
    uint32_t guard = PCA9685_TIMEOUT_LOOP;

    while (I2C_CheckEvent(PCA9685_I2C, event) != SUCCESS)
    {
        /* A missing device NACKs immediately: do not burn the whole timeout. */
        if (I2C_GetFlagStatus(PCA9685_I2C, I2C_FLAG_AF) != RESET ||
            I2C_GetFlagStatus(PCA9685_I2C, I2C_FLAG_BERR) != RESET ||
            I2C_GetFlagStatus(PCA9685_I2C, I2C_FLAG_ARLO) != RESET)
            return 1u;
        if (--guard == 0u)
        {
            return 1u;
        }
    }
    return 0u;
}

/* ------------------------------------------------------------------
 * 等总线空闲。返回 0 = 空闲；1 = 超时（总线被占死）
 * ------------------------------------------------------------------ */
static uint8_t pca_wait_idle(void)
{
    uint32_t guard = PCA9685_TIMEOUT_LOOP;

    while (I2C_GetFlagStatus(PCA9685_I2C, I2C_FLAG_BUSY) != RESET)
    {
        if (--guard == 0u)
        {
            return 1u;
        }
    }
    return 0u;
}

/* ------------------------------------------------------------------
 * 毫秒级等待：以 GetTick_ms() 为准，另加循环计数兜底
 * （TIM6 没跑时 tick 不前进，兜底保证不会死等）
 * ------------------------------------------------------------------ */
static void pca_wait_ms(uint16_t ms)
{
    uint32_t          t0    = GetTick_ms();
    volatile uint32_t guard = (uint32_t)ms * PCA9685_GUARD_PER_MS;

    while ((GetTick_ms() - t0) < (uint32_t)ms)
    {
        if (--guard == 0u)
        {
            break;
        }
    }
}

/* ------------------------------------------------------------------
 * GPIO + I2C1 外设初始化（PB6=SCL / PB7=SDA，AF4，100kHz，开漏）
 * ------------------------------------------------------------------ */
static void pca_i2c_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    I2C_InitTypeDef  I2C_InitStruct;

    RCC_AHB1PeriphClockCmd(PCA9685_GPIO_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(PCA9685_I2C_CLK, ENABLE);

    GPIO_InitStruct.GPIO_Pin   = PCA9685_SCL_PIN | PCA9685_SDA_PIN;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_OD;      /* I2C 必须开漏 */
    GPIO_InitStruct.GPIO_PuPd  = GPIO_PuPd_UP;       /* 内部弱上拉补强 */
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_Init(PCA9685_GPIO_PORT, &GPIO_InitStruct);

    GPIO_PinAFConfig(PCA9685_GPIO_PORT, PCA9685_SCL_SRC, PCA9685_AF);
    GPIO_PinAFConfig(PCA9685_GPIO_PORT, PCA9685_SDA_SRC, PCA9685_AF);

    I2C_InitStruct.I2C_Mode                = I2C_Mode_I2C;
    I2C_InitStruct.I2C_DutyCycle           = I2C_DutyCycle_2;
    I2C_InitStruct.I2C_OwnAddress1         = 0x00;
    I2C_InitStruct.I2C_Ack                 = I2C_Ack_Enable;
    I2C_InitStruct.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_InitStruct.I2C_ClockSpeed          = PCA9685_I2C_SPEED;
    I2C_Init(PCA9685_I2C, &I2C_InitStruct);

    I2C_Cmd(PCA9685_I2C, ENABLE);
}

/* ==================== 对外接口 ==================== */

/* MODE1.AI=1 and MODE2.OCH=0 are configured in PCA9685_Init(). */
static uint8_t pca_write_regs(uint8_t reg, const uint8_t *data, uint8_t len)
{
    uint8_t i;
    if (pca_wait_idle() != 0u)
    {
        printf("[PCA9685] 写超时：总线一直 BUSY（SCL/SDA 被拉死？）reg=0x%02X\r\n",
               (unsigned)reg);
        return 1u;
    }

    I2C_GenerateSTART(PCA9685_I2C, ENABLE);
    if (pca_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != 0u)
    {
        goto err;
    }

    I2C_Send7bitAddress(PCA9685_I2C, (uint8_t)(PCA9685_ADDR << 1),
                        I2C_Direction_Transmitter);
    if (pca_wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) != 0u)
    {
        /* 这里超时最常见的原因就是**模块没接 / 没上拉 / 地址不对**（没有 ACK） */
        goto err;
    }

    I2C_SendData(PCA9685_I2C, reg);
    if (pca_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTING) != 0u)
    {
        goto err;
    }

    for (i = 0u; i < len; i++)
    {
        I2C_SendData(PCA9685_I2C, data[i]);
        /* BTF: the byte was ACKed, including the final high byte before STOP. */
        if (pca_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != 0u)
            goto err;
    }

    I2C_GenerateSTOP(PCA9685_I2C, ENABLE);
    return 0u;

err:
    I2C_GenerateSTOP(PCA9685_I2C, ENABLE);
    I2C_ClearFlag(PCA9685_I2C, I2C_FLAG_AF | I2C_FLAG_BERR | I2C_FLAG_ARLO);
    printf("[PCA9685] 写超时：reg=0x%02X（模块没接？没上拉？地址不是 0x%02X？）\r\n",
           (unsigned)reg, (unsigned)PCA9685_ADDR);
    return 1u;
}

uint8_t PCA9685_WriteByte(uint8_t reg, uint8_t data)
{
    uint8_t result=pca_write_regs(reg, &data, 1u);
    if(result==0u) {
        if(reg>=PCA9685_REG_LED0_ON_L && reg<PCA9685_REG_LED0_ON_L+64u)
            output_valid&=(uint16_t)~(1u<<((reg-PCA9685_REG_LED0_ON_L)/4u));
        else if(reg==PCA9685_REG_MODE1 || reg==PCA9685_REG_PRESCALE)output_valid=0u;
    }
    return result;
}

uint8_t PCA9685_ReadByte(uint8_t reg, uint8_t *out)
{
    if (out == 0)
    {
        return 1u;
    }

    if (pca_wait_idle() != 0u)
    {
        return 1u;
    }

    /* ---- 1) 伪写：只发寄存器地址，不发 STOP ---- */
    I2C_GenerateSTART(PCA9685_I2C, ENABLE);
    if (pca_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != 0u)
    {
        goto err;
    }

    I2C_Send7bitAddress(PCA9685_I2C, (uint8_t)(PCA9685_ADDR << 1),
                        I2C_Direction_Transmitter);
    if (pca_wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) != 0u)
    {
        goto err;
    }

    I2C_SendData(PCA9685_I2C, reg);
    if (pca_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != 0u)
    {
        goto err;
    }

    /* ---- 2) 重复起始 + 读地址 ---- */
    I2C_GenerateSTART(PCA9685_I2C, ENABLE);
    if (pca_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != 0u)
    {
        goto err;
    }

    I2C_Send7bitAddress(PCA9685_I2C, (uint8_t)(PCA9685_ADDR << 1),
                        I2C_Direction_Receiver);
    if (pca_wait_event(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED) != 0u)
    {
        goto err;
    }

    /* ---- 3) 只读一个字节：读之前先 NACK + STOP ---- */
    I2C_AcknowledgeConfig(PCA9685_I2C, DISABLE);
    I2C_GenerateSTOP(PCA9685_I2C, ENABLE);

    if (pca_wait_event(I2C_EVENT_MASTER_BYTE_RECEIVED) != 0u)
    {
        goto err;
    }
    *out = I2C_ReceiveData(PCA9685_I2C);

    I2C_AcknowledgeConfig(PCA9685_I2C, ENABLE);
    return 0u;

err:
    I2C_GenerateSTOP(PCA9685_I2C, ENABLE);
    I2C_ClearFlag(PCA9685_I2C, I2C_FLAG_AF | I2C_FLAG_BERR | I2C_FLAG_ARLO);
    I2C_AcknowledgeConfig(PCA9685_I2C, ENABLE);
    return 1u;
}

uint8_t PCA9685_Init(void)
{
    uint8_t prescale;
    uint8_t mode1 = 0u;

    pca_i2c_init();

    /* ---- 先睡：PRESCALE 只在 SLEEP 下可写 ---- */
    if (PCA9685_WriteByte(PCA9685_REG_MODE1, 0x10) != 0u)
    {
        printf("[PCA9685] 初始化失败：I2C 无应答（模块没接 / 没上拉 / 线断）\r\n");
        printf("[PCA9685] 提示：SCL=PB6 SDA=PB7，地址 0x%02X，需要上拉电阻\r\n",
               (unsigned)PCA9685_ADDR);
        return 1u;
    }

    /* ---- 设 PWM 频率：PRESCALE = 25MHz/(4096×freq) - 1 ---- */
    prescale = (uint8_t)((PCA9685_OSC_MHZ * 1000000UL) /
                         (4096UL * (uint32_t)PCA9685_PWM_FREQ) - 1UL);
    if (PCA9685_WriteByte(PCA9685_REG_PRESCALE, prescale) != 0u)
    {
        return 1u;
    }
    pca_wait_ms(5);            /* 等新频率生效 */

    /* ---- MODE2：OUTDRV=1 推挽（漏配则默认开漏，上升沿靠外部上拉） ---- */
    if (PCA9685_WriteByte(PCA9685_REG_MODE2, 0x04) != 0u)
    {
        return 1u;
    }

    /* ---- 唤醒 + 自动递增（RESTART=1 让各路输出按新频率重启） ---- */
    if (PCA9685_WriteByte(PCA9685_REG_MODE1, 0xA0) != 0u)
    {
        return 1u;
    }
    pca_wait_ms(5);            /* 等内部 25MHz 振荡器稳定 */

    /* ---- 回读验证：能读回来才说明 I2C 真的双向通了 ----
     * 注意 RESTART 位读回来通常是 0（重启完成后硬件自动清），
     * 所以读到 0x20 左右是正常的，别以为配错了。 */
    if (PCA9685_ReadByte(PCA9685_REG_MODE1, &mode1) != 0u)
    {
        printf("[PCA9685] 初始化失败：回读 MODE1 超时\r\n");
        return 1u;
    }

    printf("[PCA9685] OK  MODE1=0x%02X  PRESCALE=%u（%uHz）\r\n",
           (unsigned)mode1, (unsigned)prescale, (unsigned)PCA9685_PWM_FREQ);
    return 0u;
}

uint8_t PCA9685_TrySetPWM(uint8_t ch, uint16_t off)
{
    uint8_t base;
    uint8_t data[4];

    /* ⚠️ 越界必须挡住：传 16 会算出 base = 0x06 + 64 = 0x46，
     *    直接写进 PCA9685 的保留寄存器区，器件不报错但行为未定义。 */
    if (ch >= PCA9685_CH_NUM)
    {
        printf("[PCA9685] 通道越界：%u（合法 0~%u，注意是下标不是丝印号）\r\n",
               (unsigned)ch, (unsigned)(PCA9685_CH_NUM - 1));
        return 1u;
    }

    if (off > 4095u)
    {
        off = 4095u;
    }

    base = (uint8_t)(PCA9685_REG_LED0_ON_L + 4u * ch);

    /* One STOP commits all four registers: no intermediate OFF_L/OFF_H pair
     * when the count crosses 255/256. ON stays zero, FULL ON/OFF are cleared. */
    data[0] = 0u;
    data[1] = 0u;
    data[2] = (uint8_t)(off & 0xFFu);
    data[3] = (uint8_t)(off >> 8);
    if(pca_write_regs(base, data, 4u)!=0u)return 1u;
    output_count[ch]=off;output_valid|=(uint16_t)(1u<<ch);
    return 0u;
}

void PCA9685_SetPWM(uint8_t ch, uint16_t off)
{
    (void)PCA9685_TrySetPWM(ch, off);
}

void PCA9685_SetAngle(uint8_t ch, uint16_t angle)
{
    uint16_t compare;

    if (angle > PCA9685_ANGLE_MAX_DEG)
    {
        angle = PCA9685_ANGLE_MAX_DEG;
    }

    /* 与 main.c 原映射逐字一致：0°→102 计数（≈0.5ms），180°→512 计数（≈2.5ms） */
    compare = (uint16_t)(PCA9685_CNT_AT_0DEG +
                         (angle * PCA9685_CNT_SPAN) / PCA9685_ANGLE_MAX_DEG);

    PCA9685_SetPWM(ch, compare);
}
