/**
 * @file font.c
 * 用途：UTF-8 解码、ASCII 与汉字点阵检索以及文本绘制。
 * 使用：媒体绘制调用字体接口；优先使用初始化成功的外置字库，找不到的字符显示替代符。
 * 时序与边界：字符索引与字模位序必须匹配生成工具；UTF-8 字节数与屏幕字符宽度不是同一单位。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "font.h"
#include "font_data.h"
#include "font_store.h"
#include <string.h> /* memcpy */

/* 上一个查到的字模。 必须 static —— 本工程栈只有 2KB
 * （`firmware/STM32_Controller/Core/startup_stm32f40_41xxx.s:41`），栈上放 32 字节虽然不至于爆，
 * 但这个变量本来就该常驻。 */
static uint8_t s_glyph[FONT_GLYPH_BYTES];
static uint16_t s_last_cp = 0u;
static uint8_t s_last_ok = 0u;
/*  缓存里那个字来自**哪一版**字库。字库被换/卸（`FontStore_Gen()` 变了），
 *   缓存里的字就是旧字库的字 —— 画出来是错的而且不报错。
 *   拿代号当判据 ⇒ **调用方不需要记得**清缓存，这类 bug 结构上不成立。 */
static uint32_t s_gen = 0xFFFFFFFFu;

/* ------------------------------------------------------------------
 * 片内 32 字里二分。返回下标；没找到返回 0xFFFF。
 * ------------------------------------------------------------------ */
static uint16_t in_flash_index(uint16_t cp)
{
    uint16_t lo = 0u, hi = FONT_DATA_COUNT, mid;

    while (lo < hi)
    {
        mid = (uint16_t)(lo + ((uint16_t)(hi - lo) >> 1)); /* 不用 (lo+hi)/2：
                                                                16 位上两者等价，
                                                                但这么写不会溢出 */
        if (FONT_DATA_CP[mid] == cp)
        {
            return mid;
        }
        if (FONT_DATA_CP[mid] < cp)
        {
            lo = (uint16_t)(mid + 1u);
        }
        else
        {
            hi = mid;
        }
    }
    return 0xFFFFu;
}

/* ------------------------------------------------------------------
 * 让单字缓存失效。字库被换掉 / 卸掉之后**必须**调 —— 见 font.h 的说明。
 * ------------------------------------------------------------------ */
void Font_Invalidate(void)
{
    s_last_ok = 0u;
    s_last_cp = 0u;
}

/* ------------------------------------------------------------------
 * 查字模 —— 唯一的对外入口，也是**热路径**
 * ------------------------------------------------------------------ */
const uint8_t *Font_Glyph16(uint16_t cp)
{
    uint16_t i;

    /*  单字缓存 —— 0 次 SPI。代号对不上就当没缓存（字库换过了）。 */
    if ((s_last_ok != 0u) && (cp == s_last_cp) && (s_gen == FontStore_Gen()))
    {
        return s_glyph;
    }

    /*  板载 W25Q 扩展字库 —— 命中 1 次 SPI（32 字节）；未命中 0 次 */
    if (FontStore_Ready() != 0u)
    {
        if (FontStore_Glyph(cp, s_glyph) != 0u)
        {
            s_last_cp = cp;
            s_last_ok = 1u;
            s_gen = FontStore_Gen();
            return s_glyph;
        }
    }

    /*  片内基础 32 字兜底 */
    i = in_flash_index(cp);
    if (i != 0xFFFFu)
    {
        memcpy(s_glyph, &FONT_DATA_GLYPH[(uint32_t)i * FONT_GLYPH_BYTES], FONT_GLYPH_BYTES);
        s_last_cp = cp;
        s_last_ok = 1u;
        s_gen = FontStore_Gen();
        return s_glyph;
    }

    /*  两边都没有 —— 调用方去画 '?'（`media.c:380`）。
     *  失败**不进缓存**：没查到的那次本来一次 SPI 都没发，缓存它省不下东西，
     *   反而多一个"字库换了之后还在吃旧结果"的状态。 */
    s_last_ok = 0u;
    return 0;
}

/* ------------------------------------------------------------------
 * 抄一份出来 —— 要留存字模的调用方走这个
 * ------------------------------------------------------------------ */
uint8_t Font_Glyph16Copy(uint16_t cp, uint8_t *out32)
{
    const uint8_t *g;

    if (out32 == 0)
    {
        return 0u;
    }

    g = Font_Glyph16(cp);
    if (g == 0)
    {
        return 0u;
    }

    memcpy(out32, g, FONT_GLYPH_BYTES);
    return 1u;
}

/* ------------------------------------------------------------------
 * 当前**有效**的字数（屏上 FONT: 那一行、启动日志、/help 都用它）
 * ------------------------------------------------------------------ */
uint16_t Font_Count(void)
{
    if (FontStore_Ready() != 0u)
    {
        return FontStore_Count();
    }
    return FONT_DATA_COUNT;
}

/* 0 = 片内 32 字；1 = 板载 W25Q 扩展字库 */
uint8_t Font_Source(void)
{
    return (FontStore_Ready() != 0u) ? 1u : 0u;
}
