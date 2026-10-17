/**
 * @file dht11.h
 * 用途：PE0 单总线温湿度读取及应答/校验错误分类。
 * 使用：初始化引脚后按周期读取；成功时更新测量值，失败保留错误码供状态上报。
 * 时序与边界：起始脉冲与逐位采样依赖微秒延时；传感器读取不可过频，超时不应阻塞主循环无限等待。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __DHT11_H
#define __DHT11_H

#include "stm32f4xx.h"

/* ==================================================================
 * DHT11 温湿度（单总线）——  引脚已定：`PE0`（H1 右侧第 12 针），暂不启用
 * ------------------------------------------------------------------
 * 原来的 `GPIO_Pin_9 / GPIOG / RCC_AHB1Periph_GPIOG` 是**别的板子**的
 * 写法（144 脚的 ZGT6 那类才有 GPIOG）。本板是
 * **STM32F407VET6 / LQFP100 —— 没有 GPIOF、也没有 GPIOG**。
 *
 *  它一直能编译通过，只是因为标准库头文件里**无条件**定义了 GPIOG /
 *    RCC_AHB1Periph_GPIOG 这些宏；真跑起来 `GPIO_Init(GPIOG, ...)` 是
 *    **空写** —— 不报错、不告警、也**永远读不到数**。
 *    （已把同类"别的板子的驱动"整体归档到 `_archive\`，
 *      本模块因为要在保留清单里，改成下面这种"占位 + 空壳"的写法。）
 *
 * ⇒  **引脚已分配**（`PE0`），本版本已启用驱动：
 *    · 下面 3 个引脚宏已经是**真引脚**（`GPIOE` / `GPIO_Pin_0` / `RCC_AHB1Periph_GPIOE`）；
 *    · `DHT11_PIN_ASSIGNED` 已为 **1**，dht11.c 编译真实驱动；录音或 OTA 忙时主循环
 *      会跳过周期读取，避免 DHT11 的单总线时序影响麦克风 DMA 和 OTA 数据。
 *
 * 【启用方法】
 *   1)  **已完成**：`DHT11_PORT = GPIOE` / `DHT11_PIN = GPIO_Pin_0`
 *         / `DHT11_CLK = RCC_AHB1Periph_GPIOE` = **H1（右侧）第 12 针**
 *   2) 本版本已把 `DHT11_PIN_ASSIGNED` 设为 1；确认接线和上拉后即可读取。
 *   3)  确认这个脚在本板**真存在**，且没被下面这些占着
 *      （见「项目地图.md」§2 影响矩阵）：
 *        LCD/FSMC   PD0-PD1 PD4-PD5 PD7-PD10 PD13-PD15 PE7-PE15
 *        SD/SDIO    PC8-PC12 PD2
 *        PCA9685    PB6/PB7 (I2C1)
 *        调试串口   PA2(TX) / PA3(RX)
 *        ESP 链路   PC6/PC7 (USART6)     ←  从 PB10/PB11(USART3) 迁走
 *        麦克风     PB9/PB10/PC3 (I2S2)  ← PB10 现在完全归麦克风，别碰
 *        毫米波雷达 PC4 (仅 OUT， 已分配，H2 左侧第 31 针)
 *        按键/背光  PA0 (K-UP) / PB1 (LCD BL)
 *        板载 LED   PA6/PA7 (TIM3_CH1/CH2)
 *
 *     **DATA 脚 = `PE0`，定案**。选它的理由：
 *      `PE0` 在 **H1（右侧）第 12 针**，而 **H1 的 3V3 在 1~4 针、GND 在 7~10 针**
 *      ⇒ **VCC / DATA / GND 三根线全在同一排针上**，三脚模块直接插，不用跨排针飞线。
 *      复用也无冲突：PE0 的 AF 全是 FSMC_NBL0 / DCMI_D2 / TIM4_ETR，**本板一个都没用**。
 *       **裸传感器要自己补 4.7k~10k 上拉到 3V3**；带小板的模块一般已带，先量一下。
 *       **别和 `PE3`/`PE4` 搞混** —— 那两个是板载按键 2/3，在 **H2（左侧）第 12/13 针**。
 * ================================================================== */

/* ---------- 总开关：0 = 未分配引脚（空壳），1 = 已分配，真跑驱动 ---------- */
#define DHT11_PIN_ASSIGNED 1

/* ---------- DATA 引脚： 已分配 = `PE0`（H1 右侧第 12 针） ---------- */
#define DHT11_PIN GPIO_Pin_0 /* H1（右侧）第 12 针 */
#define DHT11_PORT GPIOE
#define DHT11_CLK RCC_AHB1Periph_GPIOE
/* 三行引脚宏已经是真值；DHT11_PIN_ASSIGNED=1。录音或 OTA 忙时主循环跳过读取。 */

/* ---------- 温湿度数据 ---------- */
typedef struct
{
    uint8_t humi_int; /* 湿度整数 */
    uint8_t humi_dec; /* 湿度小数 */
    uint8_t temp_int; /* 温度整数 */
    uint8_t temp_dec; /* 温度小数 */
    uint8_t checksum; /* 校验和 */
} DHT11_Data;

extern DHT11_Data dht11;

/* ---------- 函数（未分配引脚时全部是空壳，见 dht11.c） ---------- */
void DHT11_Init(void);    /* 未分配引脚 → 什么都不做 */
void DHT11_Diag(void);    /* 未分配引脚 → 只打一行"未分配引脚" */
uint8_t DHT11_Read(void); /* 未分配引脚 → 恒返回 1（无响应） */
void DHT11_Print(void);   /* 未分配引脚 → 只打一行"未分配引脚" */
void DHT11_sys(void);     /* 周期任务入口（真驱动里是每 2s 读一次并打印） */

/* Cached status only: LCD queries never perform single-wire transactions.
 * 255 = initialized but no read yet; 0 = checksum passed; 1/2/3 = read error. */
uint8_t DHT11_InitOK(void);
uint8_t DHT11_LastResult(void);

#endif /* __DHT11_H */
