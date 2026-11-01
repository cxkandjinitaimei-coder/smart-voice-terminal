#ifndef __MAX98357_H
#define __MAX98357_H

#include "stm32f4xx.h"

/*
 * MAX98357/MAX9835 是 I2S 数字功放，不接模拟 DAC。F407 通过 I2S2
 * 输出 16 kHz、16 bit、单声道 PCM；驱动把每个单声道样本复制到左右槽，
 * 这样常见的单线 DIN 模块都能正常收到声音。
 *
 * 接线：DIN=PB15(I2S2_SD/AF5)，BCLK=PB10，LRC/WS=PB9，VIN/GND 按模块
 * 规格连接。SPK+ 和 SPK- 是桥接输出，喇叭两端都不能接 GND。
 */
#define MAX98357_T_BEGIN 0x05u
#define MAX98357_T_PCM   0x06u
#define MAX98357_T_END   0x07u
#define MAX98357_T_ACK   0x08u

uint8_t  MAX98357_Init(void);
uint8_t  MAX98357_Begin(uint16_t session, const uint8_t *params, uint16_t len);
uint8_t  MAX98357_Pcm(uint16_t seq, const uint8_t *pcm, uint16_t bytes);
uint8_t  MAX98357_End(uint16_t session);
void     MAX98357_Stop(void);
void     MAX98357_Tick(void);
uint8_t  MAX98357_Busy(void);
uint32_t MAX98357_Underruns(void);
void     MAX98357_DMA_IRQHandler(void);

#endif
