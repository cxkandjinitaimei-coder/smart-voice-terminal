#include "key.h"
#include <stdio.h>

static volatile uint8_t g_ready;
static uint8_t g_raw, g_stable_ms, g_released;
/* 单生产者(ISR)/单消费者(main)计数；主循环不清 ISR 的计数，避免读清竞态。 */
static volatile uint32_t g_produced;
static uint32_t g_consumed;
static volatile uint8_t g_ota_ready;
static uint8_t g_ota_raw, g_ota_stable_ms, g_ota_released;
static volatile uint32_t g_ota_produced;
static uint32_t g_ota_consumed;

uint8_t KEY_UP_Level(void)
{
    return GPIO_ReadInputDataBit(KEY_UP_PORT, KEY_UP_PIN) == Bit_SET ? 1u : 0u;
}

uint8_t KEY_UP_Ready(void) { return g_ready; }

void KEY_UP_Init(void)
{
    GPIO_InitTypeDef gi;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    g_ready = 0u;
    RCC_AHB1PeriphClockCmd(KEY_UP_CLK, ENABLE);
    gi.GPIO_Pin = KEY_UP_PIN;
    gi.GPIO_Mode = GPIO_Mode_IN;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    gi.GPIO_PuPd = KEY_UP_ACTIVE_LEVEL ? GPIO_PuPd_DOWN : GPIO_PuPd_UP;
    GPIO_Init(KEY_UP_PORT, &gi);
    g_raw = KEY_UP_Level();
    g_stable_ms = 0u;
    g_released = 0u;
    g_produced = g_consumed = 0u;
    g_ready = 1u;
    __set_PRIMASK(mask);
    printf("[KEY] PA0 active=%u pull=%s raw=%u; sample=1ms debounce=%ums\r\n",
           (unsigned)KEY_UP_ACTIVE_LEVEL, KEY_UP_ACTIVE_LEVEL ? "DOWN" : "UP",
           (unsigned)KEY_UP_Level(), (unsigned)KEY_UP_DEBOUNCE_MS);
    printf("[KEY] Release before first press; hold does not repeat\r\n");
}

void KEY_UP_Tick1ms(void)
{
    uint8_t lv;
    if(!g_ready) return;
    lv = KEY_UP_Level();
    if(lv != g_raw)
    {
        g_raw = lv;
        g_stable_ms = 0u;
    }
    if(g_stable_ms >= KEY_UP_DEBOUNCE_MS) return;
    if(++g_stable_ms < KEY_UP_DEBOUNCE_MS) return;
    if(lv != KEY_UP_ACTIVE_LEVEL)
        g_released = 1u; /* 只有稳定松手才能重新武装，短毛刺不能制造第二次按下。 */
    else if(g_released)
    {
        g_released = 0u;
        g_produced++;
    }
}

uint8_t KEY_UP_Scan(void)
{
    if(!g_ready || g_consumed == g_produced) return 0u;
    g_consumed++;
    return 1u;
}

void KEY_OTA_Init(void)
{
    GPIO_InitTypeDef gi;
    RCC_AHB1PeriphClockCmd(KEY_OTA_CLK, ENABLE);
    gi.GPIO_Pin = KEY_OTA_PIN;
    gi.GPIO_Mode = GPIO_Mode_IN;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    gi.GPIO_PuPd = KEY_OTA_ACTIVE_LEVEL ? GPIO_PuPd_DOWN : GPIO_PuPd_UP;
    GPIO_Init(KEY_OTA_PORT, &gi);
    g_ota_raw = GPIO_ReadInputDataBit(KEY_OTA_PORT, KEY_OTA_PIN) == Bit_SET ? 1u : 0u;
    g_ota_stable_ms = 0u; g_ota_released = 0u;
    g_ota_produced = g_ota_consumed = 0u;
    g_ota_ready=1u;
    printf("[KEY] PE3 extra key active=%u; release before first press\r\n",(unsigned)KEY_OTA_ACTIVE_LEVEL);
}

void KEY_OTA_Tick1ms(void)
{
    uint8_t lv;
    if(!g_ota_ready)return;
    lv = GPIO_ReadInputDataBit(KEY_OTA_PORT, KEY_OTA_PIN) == Bit_SET ? 1u : 0u;
    if (lv != g_ota_raw) { g_ota_raw = lv; g_ota_stable_ms = 0u; }
    if (g_ota_stable_ms >= KEY_UP_DEBOUNCE_MS) return;
    if (++g_ota_stable_ms < KEY_UP_DEBOUNCE_MS) return;
    if (lv != KEY_OTA_ACTIVE_LEVEL) g_ota_released = 1u;
    else if (g_ota_released) { g_ota_released = 0u; g_ota_produced++; }
}

uint8_t KEY_OTA_Scan(void)
{
    if (!g_ota_ready || g_ota_consumed == g_ota_produced) return 0u;
    g_ota_consumed++;
    return 1u;
}
