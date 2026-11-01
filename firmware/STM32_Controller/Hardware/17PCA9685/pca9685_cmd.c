/**
 * @file    pca9685_cmd.c
 * @brief   PCA9685 串口命令层 —— /pwm 一族 + 协议 id ↔ 通道下标换算
 * ------------------------------------------------------------------
 * 为什么单独一个文件、而不是塞进 pca9685.c：
 *   pca9685.c 是**已经上机验证过**的驱动（I2C 超时哨兵、PRESCALE、
 *   MODE2 推挽那几处都调好了）。命令层是另一件事，分开放以后加命令
 *   也不容易碰坏驱动。
 *
 * 【和 main.c 的接法】必须排在 MicRec_Command 之后、Media_Command **之前**：
 *   if (!MicRec_Command(line))
 *       if (!PCA9685_Command(line))
 *           request = Media_Command(line);
 *   ⚠️ 顺序反了就被 Media_Command 吃掉 —— 它对任何不认识的 "/xxx"
 *      都会回一句 "Unknown command" 并**整行吞掉**（media.c 的兜底）。
 *
 * 【口径】命令里收的 id 一律 **1~16，等于模块丝印号**（跟云端协议一致）；
 *   模块内部的 PCA9685_SetAngle/SetPWM 收的是**下标 0~15**。
 *   换算只在 pca9685.h 的 PCA_ID_TO_CH 一处做。
 * ================================================================== */

#include "pca9685.h"
#include <stdio.h>     /* printf（fputc 重定向在 uart.c） */
#include <string.h>    /* strncmp */

/* ------------------------------------------------------------------
 * 真正"关掉"一路输出。
 *
 * ⚠️⚠️ 不能用 PCA9685_SetPWM(ch, 4096) 来代替 —— 它内部有
 *    `if (off > 4095u) off = 4095u;`，会把 4096 **夹成 4095**，
 *    那是 4095/4096 的**几乎全开**脉宽：舵机会被顶到一端、
 *    持续憋着嗡嗡响，既不是"关"，还费电发热。
 *
 * PCA9685 的"全关"只有一种写法：OFF_H 寄存器的 **bit4 = FULL OFF**（0x10）。
 * 置上之后该路恒为低电平，与 ON 计数无关。
 * ------------------------------------------------------------------ */
static void pca_ch_off(uint8_t ch)
{
    uint8_t base = (uint8_t)(PCA9685_REG_LED0_ON_L + 4u * ch);

    PCA9685_WriteByte(base,                  0x00u);   /* ON_L  = 0 */
    PCA9685_WriteByte((uint8_t)(base + 1u),  0x00u);   /* ON_H  ：不设 FULL ON */
    PCA9685_WriteByte((uint8_t)(base + 2u),  0x00u);   /* OFF_L = 0 */
    PCA9685_WriteByte((uint8_t)(base + 3u),  0x10u);   /* OFF_H ：bit4 = FULL OFF */
}

/* ------------------------------------------------------------------
 * 回读一路的 OFF 寄存器。
 * 返回 0 = 读成功（此时 *cnt / *is_off 有效）；1 = I2C 没应答
 * ------------------------------------------------------------------ */
static uint8_t pca_ch_read(uint8_t ch, uint16_t *cnt, uint8_t *is_off)
{
    uint8_t base = (uint8_t)(PCA9685_REG_LED0_ON_L + 4u * ch);
    uint8_t ol = 0u;
    uint8_t oh = 0u;

    if (PCA9685_ReadByte((uint8_t)(base + 2u), &ol) != 0u) { return 1u; }
    if (PCA9685_ReadByte((uint8_t)(base + 3u), &oh) != 0u) { return 1u; }

    *is_off = ((oh & 0x10u) != 0u) ? 1u : 0u;
    *cnt    = (uint16_t)(((uint16_t)(oh & 0x0Fu) << 8) | (uint16_t)ol);
    return 0u;
}

/* ------------------------------------------------------------------
 * 计数 → 角度。是 PCA9685_SetAngle() 那个映射**反着来**，超范围夹到 0~180。
 * 102 计数 ≈ 0°（0.5ms），512 计数 ≈ 180°（2.5ms）。
 * ------------------------------------------------------------------ */
static uint16_t pca_cnt_to_deg(uint16_t cnt)
{
    if (cnt <= (uint16_t)PCA9685_CNT_AT_0DEG)
    {
        return 0u;
    }
    if (cnt >= (uint16_t)(PCA9685_CNT_AT_0DEG + PCA9685_CNT_SPAN))
    {
        return PCA9685_ANGLE_MAX_DEG;
    }
    return (uint16_t)(((uint32_t)(cnt - (uint16_t)PCA9685_CNT_AT_0DEG) *
                       (uint32_t)PCA9685_ANGLE_MAX_DEG) / (uint32_t)PCA9685_CNT_SPAN);
}

/* ------------------------------------------------------------------
 * 列出下标 ch_from ~ ch_to 这几路（想看一路就传同一个值两次）
 * 角度是**从寄存器读回来反算**的，不是我记的账 —— 所以它跟硬件真实状态一致。
 * ------------------------------------------------------------------ */
static void pca_list(uint8_t ch_from, uint8_t ch_to)
{
    uint8_t ch;
    uint8_t bad = 0u;

    for (ch = ch_from; ch <= ch_to; ch++)
    {
        uint16_t cnt = 0u;
        uint8_t  is_off = 0u;

        if (pca_ch_read(ch, &cnt, &is_off) != 0u)
        {
            bad++;
            continue;
        }

        if (is_off != 0u)
        {
            printf("[PWM] id %2u (ch %2u) : OFF   (FULL OFF 已置位)\r\n",
                   (unsigned)PCA_CH_TO_ID(ch), (unsigned)ch);
        }
        else
        {
            printf("[PWM] id %2u (ch %2u) : %3u 度   (OFF 计数 %u)\r\n",
                   (unsigned)PCA_CH_TO_ID(ch), (unsigned)ch,
                   (unsigned)pca_cnt_to_deg(cnt), (unsigned)cnt);
        }
    }

    if (bad != 0u)
    {
        printf("[PWM] 有 %u 路读不回来 —— I2C 无应答。这是**硬件**问题：\r\n",
               (unsigned)bad);
        printf("[PWM] 查 SCL=PB6 / SDA=PB7 接线、上拉电阻、模块供电、地址 0x40\r\n");
    }
}

static void pca_usage(void)
{
    printf("[PWM] 用法（id 一律 1~16，等于模块丝印号）：\r\n");
    printf("      /pwm                列全部 16 路\r\n");
    printf("      /pwm <id>           只列这一路\r\n");
    printf("      /pwm <id> <0~180>   设角度\r\n");
    printf("      /pwm <id> off       关这一路输出\r\n");
    printf("      /pwm all <0~180>    16 路一起设\r\n");
    printf("      /pwm all off        16 路一起关\r\n");
}

/* 解析一个无符号整数。*pp 前进到数字之后；返回 1 = 至少吃到一位数字 */
static uint8_t parse_u32(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    uint32_t    v = 0u;
    uint8_t     any = 0u;

    while ((*p >= '0') && (*p <= '9'))
    {
        if (v < 100000UL)          /* 饱和，免得越积越多把 uint32 绕回去 */
        {
            v = v * 10u + (uint32_t)(*p - '0');
        }
        any = 1u;
        p++;
    }

    *pp  = p;
    *out = v;
    return any;
}

static void skip_spaces(const char **pp)
{
    while (**pp == ' ')
    {
        (*pp)++;
    }
}

/* "off" / "OFF"，且后面必须是结尾或空格（这样 "offset" 不算） */
static uint8_t is_off_word(const char *p)
{
    if ((p[0] != 'o') && (p[0] != 'O')) { return 0u; }
    if ((p[1] != 'f') && (p[1] != 'F')) { return 0u; }
    if ((p[2] != 'f') && (p[2] != 'F')) { return 0u; }
    if ((p[3] != '\0') && (p[3] != ' ')) { return 0u; }
    return 1u;
}

/* ==================================================================
 * 命令入口。返回 1 = 是我的命令（已处理）；0 = 不是，交回调用者
 * ================================================================== */
uint8_t PCA9685_Command(const char *line)
{
    const char *p;
    uint32_t    id;
    uint32_t    ang;
    uint8_t     ch;

    if (line == 0) { return 0u; }

    /* 只认 "/pwm" 和 "/pwm ..."；"/pwmX" 不是本命令 */
    if (strncmp(line, "/pwm", 4) != 0)          { return 0u; }
    if ((line[4] != '\0') && (line[4] != ' '))  { return 0u; }

    /* ---------- /pwm ：列全部 16 路 ---------- */
    if (line[4] == '\0')
    {
        printf("[PWM] 16 路状态（角度由寄存器回读反算，不是记的账）\r\n");
        pca_list(0u, (uint8_t)(PCA9685_CH_NUM - 1u));
        return 1u;
    }

    p = line + 5;
    skip_spaces(&p);

    /* ---------- /pwm all ... ---------- */
    if ((p[0] == 'a') && (p[1] == 'l') && (p[2] == 'l') &&
        ((p[3] == '\0') || (p[3] == ' ')))
    {
        p += 3;
        skip_spaces(&p);

        if (*p == '\0')
        {
            printf("[PWM] 用法：/pwm all <0~180>  或  /pwm all off\r\n");
            return 1u;
        }

        if (is_off_word(p) != 0u)
        {
            for (ch = 0u; ch < PCA9685_CH_NUM; ch++)
            {
                pca_ch_off(ch);
            }
            printf("[PWM] 16 路全部关输出（FULL OFF）\r\n");
            return 1u;
        }

        if (parse_u32(&p, &ang) == 0u)
        {
            printf("[PWM] 角度只能是数字或 off\r\n");
            return 1u;
        }
        if (ang > (uint32_t)PCA9685_ANGLE_MAX_DEG)
        {
            printf("[PWM] 角度 %lu 超范围，夹到 %u\r\n",
                   (unsigned long)ang, (unsigned)PCA9685_ANGLE_MAX_DEG);
            ang = (uint32_t)PCA9685_ANGLE_MAX_DEG;
        }

        for (ch = 0u; ch < PCA9685_CH_NUM; ch++)
        {
            PCA9685_SetAngle(ch, (uint16_t)ang);
        }
        printf("[PWM] 16 路一起 -> %lu 度\r\n", (unsigned long)ang);
        return 1u;
    }

    /* ---------- /pwm <id> ... ---------- */
    if (parse_u32(&p, &id) == 0u)
    {
        pca_usage();
        return 1u;
    }
    if ((id < 1u) || (id > (uint32_t)PCA9685_CH_NUM))
    {
        printf("[PWM] id 必须是 1~%u（= 模块丝印号），收到 %lu\r\n",
               (unsigned)PCA9685_CH_NUM, (unsigned long)id);
        return 1u;
    }
    ch = PCA_ID_TO_CH(id);          /* ★ 全工程唯一的 id→下标 换算 */

    skip_spaces(&p);

    /* /pwm <id> ：只列这一路 */
    if (*p == '\0')
    {
        pca_list(ch, ch);
        return 1u;
    }

    if (is_off_word(p) != 0u)
    {
        pca_ch_off(ch);
        printf("[PWM] id %lu (ch %u) 关输出（FULL OFF）\r\n",
               (unsigned long)id, (unsigned)ch);
        return 1u;
    }

    if (parse_u32(&p, &ang) == 0u)
    {
        pca_usage();
        return 1u;
    }
    if (ang > (uint32_t)PCA9685_ANGLE_MAX_DEG)
    {
        printf("[PWM] 角度 %lu 超范围，夹到 %u\r\n",
               (unsigned long)ang, (unsigned)PCA9685_ANGLE_MAX_DEG);
        ang = (uint32_t)PCA9685_ANGLE_MAX_DEG;
    }

    PCA9685_SetAngle(ch, (uint16_t)ang);
    printf("[PWM] id %lu (ch %u) -> %lu 度\r\n",
           (unsigned long)id, (unsigned)ch, (unsigned long)ang);
    return 1u;
}
