#ifndef __PCA9685_H
#define __PCA9685_H

#include "stm32f4xx.h"

/* ==================================================================
 * PCA9685 16 路 PWM 模块（I2C1，50Hz，驱动舵机）
 * ------------------------------------------------------------------
 * 本模块从 User/main.c 里搬出来的（原来 I2C1_Init / PCA9685_* /
 * Servo_SetAngle 全都直接写在 main.c 里，没有模块）。
 * 引脚/参数（移植到其他板子只改下面的宏，pca9685.c 不用动）：
 *   SCL = PB6 → I2C1_SCL（AF4）
 *   SDA = PB7 → I2C1_SDA（AF4）
 *   地址 0x40（A0~A5 全接地）；OE 接 GND = 常开输出，不占 IO
 *   VCC = 逻辑电源 3.3V；V+ = 舵机电源，必须独立供电并与主板共地
 *
 * 【通道号口径】★ 2026-09-18 改口径，别再看老注释（老注释说的是反的）：
 *   · **协议 / 云端 / 串口命令 / 你接线看到的口头编号** 一律用
 *     **id 1~16，等于模块丝印号**；
 *   · **本驱动的 PCA9685_SetAngle/SetPWM 收的是下标 0~15**，与寄存器一致；
 *   · 两者之间只在 PCA_ID_TO_CH() 一处换算。
 *   ⚠️ 所以 main.c 里写 PCA9685_SetAngle(1, ...) 驱动的是**丝印 2** 那一路。
 *      老固件那句 "驱动 1/3/5" 就是这么来的（对应丝印 2/4/6），
 *      是新旧口径不同，不是 bug。★ 接线时以**丝印号**为准，别再自己减 1。
 *
 * 【时基】等待用 GetTick_ms() ⇒ **必须先调用 TIM6_Init()**。
 *   ⚠️ 但"超时哨兵"刻意用**循环计数**而不是 GetTick_ms()：
 *      TIM6 万一没跑，tick 永不前进，用 tick 当超时的话永远不触发——
 *      恰好在你最需要它兜底的时候失效。
 *
 * 【为什么加超时】原 main.c 的 I2C 全是死等（标准库 I2C_CheckEvent
 *   只做单次检查、本身没有超时）。PCA9685 没接/没上拉/线断了时，
 *   上电就卡死在 while 里出不来，表现为"整机一动不动、串口一个字不吐"，
 *   极难查。现在改成超时后返回错误码，串口能看见、程序继续跑。
 * ================================================================== */

/* ---------- 外设 / 引脚 / 时钟 ---------- */
#define PCA9685_I2C          I2C1
#define PCA9685_I2C_CLK      RCC_APB1Periph_I2C1
#define PCA9685_GPIO_PORT    GPIOB
#define PCA9685_GPIO_CLK     RCC_AHB1Periph_GPIOB
#define PCA9685_SCL_PIN      GPIO_Pin_6
#define PCA9685_SDA_PIN      GPIO_Pin_7
#define PCA9685_SCL_SRC      GPIO_PinSource6
#define PCA9685_SDA_SRC      GPIO_PinSource7
#define PCA9685_AF           GPIO_AF_I2C1

/* ---------- 器件参数 ---------- */
#define PCA9685_ADDR         0x40    /* 7 位地址（A0~A5 全接地） */
#define PCA9685_I2C_SPEED    100000  /* 100kHz */

/* 通道数（对外合法入参 0 ~ PCA9685_CH_NUM-1） */
#define PCA9685_CH_NUM       16

/* ---------- 寄存器 ---------- */
#define PCA9685_REG_MODE1        0x00
#define PCA9685_REG_MODE2        0x01
#define PCA9685_REG_LED0_ON_L    0x06   /* 通道 n 的基址 = 0x06 + 4*n */
#define PCA9685_REG_PRESCALE     0xFE

/* ---------- 初始化参数 ---------- */
#define PCA9685_PWM_FREQ     50      /* Hz，舵机用 50Hz（周期 20ms） */
/* PRESCALE = round(25MHz / (4096 × freq)) - 1；25MHz = 内部 25MHz 振荡器 */
#define PCA9685_OSC_MHZ      25

/* ---------- 舵机角度 → 计数 ---------- *
 * 50Hz 下 4096 计数 = 20000µs ⇒ 1 计数 ≈ 4.88µs
 *   0.5ms（0°）  ≈ 102 计数
 *   2.5ms（180°）≈ 512 计数
 * 与 main.c 原来的映射逐字一致（MG995/SG90 通用） */
#define PCA9685_ANGLE_MIN_DEG   0
#define PCA9685_ANGLE_MAX_DEG   180
#define PCA9685_CNT_AT_0DEG     102
#define PCA9685_CNT_SPAN        410   /* 180° 对应 410 个计数 */

/* ---------- 超时哨兵（循环计数，与 tick 无关） ----------
 * 每次循环约几十个时钟周期，200000 次 ≈ 几十 ms 量级 @168MHz。
 * 只是"卡死兜底"，不是精确计时，别拿它当延时用。 */
#define PCA9685_TIMEOUT_LOOP    200000u

/* ==================== 函数声明 ==================== */

/* 初始化：I2C1 外设 + GPIO(AF4) → 复位 → 设 50Hz → 推挽输出 → 唤醒。
 * 返回 0 = 成功；1 = I2C 没应答（模块没接 / 没上拉 / 线断 / 地址不对） */
uint8_t PCA9685_Init(void);

/* 写一个寄存器。返回 0 = 成功；1 = 超时 */
uint8_t PCA9685_WriteByte(uint8_t reg, uint8_t data);

/* 读一个寄存器（用于回读 MODE1/PRESCALE 验证 I2C 真的活着）。
 * 返回 0 = 成功；1 = 超时 */
uint8_t PCA9685_ReadByte(uint8_t reg, uint8_t *out);

/* 设某个通道的 PWM 比较值（脉宽计数）。
 *   ch  : 0~15（下标，不是丝印号）
 *   off : 0 ~ 4095 */
void PCA9685_SetPWM(uint8_t ch, uint16_t off);

/* Same limits, but reports I2C failure (0 = success, 1 = error).
 * ON_L..OFF_H are sent in ONE auto-increment transaction; MODE2.OCH=0
 * makes the new pulse width visible together at STOP. */
uint8_t PCA9685_TrySetPWM(uint8_t ch, uint16_t off);

/* 设舵机角度。
 *   ch    : 0~15（下标，不是丝印号）
 *   angle : 0~180（度），超范围会被夹到 0~180 */
void PCA9685_SetAngle(uint8_t ch, uint16_t angle);
/* Cached successful PWM write, not physical shaft feedback. No I2C read here. */
uint8_t PCA9685_GetAngleEstimate(uint8_t ch,uint16_t *angle);

/* ==================== 命令层（实现在 pca9685_cmd.c）==================== */

/* 协议 id（1~16，= 模块丝印号）↔ 模块下标（0~15，= 寄存器口径）。
 * ★ 全工程只有这一处做这个换算，改这里就换了映射规则。 */
#define PCA_ID_TO_CH(id)   ((uint8_t)((id) - 1u))
#define PCA_CH_TO_ID(ch)   ((uint8_t)((ch) + 1u))

/* 串口 /pwm 命令族。返回 1 = 是我的命令（已处理）；0 = 不是，交回调用者。
 *   /pwm                列全部 16 路（角度由寄存器回读反算）
 *   /pwm <id>           只列这一路
 *   /pwm <id> <0~180>   设角度
 *   /pwm <id> off       真正关掉这一路输出（写 OFF_H 的 FULL OFF 位）
 *   /pwm all <0~180>    16 路一起设
 *   /pwm all off        16 路一起关
 * ⚠️ 必须在 Media_Command() **之前**调用：Media_Command 对任何不认识的
 *    "/xxx" 都会回一句 Unknown command 并把整行吃掉。 */
uint8_t PCA9685_Command(const char *line);

#endif /* __PCA9685_H */
