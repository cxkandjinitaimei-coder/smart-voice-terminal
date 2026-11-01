/* ==================================================================
 * font.c —— 汉字字库查表（三层：单字缓存 → W25Q 扩展 → 片内 32 字）
 * ------------------------------------------------------------------
 * 片内那 32 个字在 `App/font_data.h`，由 `Tools/font_gen/gen.py` 生成；
 * W25Q 上那 3755 个字由 `Tools/font_gen/gen_bin.py` 生成、`Tools/font_xfer/` 灌进去。
 * 「返回值只在下一次调用前有效」「不可重入」两条，**写在 font.h 的注释里**。
 *
 * 【为什么是这三层，顺序为什么不能换】
 *   · 单字缓存放最前：画一整页时同一个字常常连着出现，命中就 **0 次 SPI**。
 *   · W25Q 扩展字库第二：它是"更全的那一份"，所以优先。
 *     查不到时**一次 SPI 都不发** —— 索引 7510 B 已经在上电时进 RAM 了
 *     （`font_store.c`），二分全在 RAM 里走。
 *   · 片内 32 字兜底：W25Q 没装 / 校验没过 / 那个字就是不在扩展表里
 *     —— 三种情况都退回这里，**热路径零 SPI**。
 *
 * 【★ 两条路径都 memcpy 进同一个 s_glyph —— 这不是为了省事】
 *   如果片内命中返回"稳定的 Flash 指针"、W25Q 命中返回"会被覆盖的 scratch"，
 *   同一个函数就有了**两种指针有效期**：`a = Glyph(x); b = Glyph(y);` 之后
 *   再用 a，有时候对、有时候错，而且**不报错**。统一成一种，这类 bug 就没了。
 *
 * 【为什么不把片内 32 字也搬到 W25Q 那一套里去】它是"永远在"的那个兜底。
 *   要是连它也依赖 W25Q，W25Q 一坏就什么都不剩了。
 *
 * 【为什么不用 mktable + switch】字表涨到几千条时 switch 会生成一大坨 case，
 *   而二分查找的代码只有十几行、也不用改 .uvprojx 里的优化选项。
 * ================================================================== */
#include "font.h"
#include "font_data.h"
#include "font_store.h"
#include <string.h>      /* memcpy */

/* 上一个查到的字模。★ 必须 static —— 本工程栈只有 2KB
 * （`ST32/AIAssistant/Core/startup_stm32f40_41xxx.s:41`），栈上放 32 字节虽然不至于爆，
 * 但这个变量本来就该常驻。 */
static uint8_t  s_glyph[FONT_GLYPH_BYTES];
static uint16_t s_last_cp = 0u;
static uint8_t  s_last_ok = 0u;
/* ★ 缓存里那个字来自**哪一版**字库。字库被换/卸（`FontStore_Gen()` 变了），
 *   缓存里的字就是旧字库的字 —— 画出来是错的而且不报错。
 *   拿代号当判据 ⇒ **调用方不需要记得**清缓存，这类 bug 结构上不成立。 */
static uint32_t s_gen = 0xFFFFFFFFu;

/* ------------------------------------------------------------------
 * 片内 32 字里二分。返回下标；没找到返回 0xFFFF。
 * ------------------------------------------------------------------ */
static uint16_t in_flash_index(uint16_t cp)
{
    uint16_t lo = 0u, hi = FONT_DATA_COUNT, mid;

    while(lo < hi)
    {
        mid = (uint16_t)(lo + ((uint16_t)(hi - lo) >> 1));   /* 不用 (lo+hi)/2：
                                                                16 位上两者等价，
                                                                但这么写不会溢出 */
        if(FONT_DATA_CP[mid] == cp) { return mid; }
        if(FONT_DATA_CP[mid] < cp)  { lo = (uint16_t)(mid + 1u); }
        else                        { hi = mid; }
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

    /* ① 单字缓存 —— 0 次 SPI。代号对不上就当没缓存（字库换过了）。 */
    if((s_last_ok != 0u) && (cp == s_last_cp) && (s_gen == FontStore_Gen())) { return s_glyph; }

    /* ② 板载 W25Q 扩展字库 —— 命中 1 次 SPI（32 字节）；未命中 0 次 */
    if(FontStore_Ready() != 0u)
    {
        if(FontStore_Glyph(cp, s_glyph) != 0u)
        {
            s_last_cp = cp;
            s_last_ok = 1u;
            s_gen    = FontStore_Gen();
            return s_glyph;
        }
    }

    /* ③ 片内基础 32 字兜底 */
    i = in_flash_index(cp);
    if(i != 0xFFFFu)
    {
        memcpy(s_glyph, &FONT_DATA_GLYPH[(uint32_t)i * FONT_GLYPH_BYTES], FONT_GLYPH_BYTES);
        s_last_cp = cp;
        s_last_ok = 1u;
        s_gen    = FontStore_Gen();
        return s_glyph;
    }

    /* ④ 两边都没有 —— 调用方去画 '?'（`media.c:380`）。
     * ★ 失败**不进缓存**：没查到的那次本来一次 SPI 都没发，缓存它省不下东西，
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

    if(out32 == 0) { return 0u; }

    g = Font_Glyph16(cp);
    if(g == 0) { return 0u; }

    memcpy(out32, g, FONT_GLYPH_BYTES);
    return 1u;
}

/* ------------------------------------------------------------------
 * 当前**有效**的字数（屏上 FONT: 那一行、启动日志、/help 都用它）
 * ------------------------------------------------------------------ */
uint16_t Font_Count(void)
{
    if(FontStore_Ready() != 0u) { return FontStore_Count(); }
    return FONT_DATA_COUNT;
}

/* 0 = 片内 32 字；1 = 板载 W25Q 扩展字库 */
uint8_t Font_Source(void)
{
    return (FontStore_Ready() != 0u) ? 1u : 0u;
}
