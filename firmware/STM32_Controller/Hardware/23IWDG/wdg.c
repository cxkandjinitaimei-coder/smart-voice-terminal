/* ============================================================================
 * wdg.c —— 独立看门狗（IWDG）实现
 *   ★ 设计依据、超时值来历、bootloader 为什么不能装 —— 全在 wdg.h 头注释里，
 *     改任何一处之前**先读完那个头**。
 * ========================================================================= */
#include "wdg.h"
#include <stdio.h>

/* ----------------------------------------------------------------------------
 * IWDG 的 KEYS（RCC 那些宏本工程没有，IWDG 这几个有，但为了"不依赖 Lib 的 .c"
 * 一律写成裸寄存器 + 字面量，语义写清楚。见 iap_app.c 的同类做法）
 * ------------------------------------------------------------------------- */
#define WDG_KEY_UNLOCK        0x5555u   /* 允许写 PR / RLR（= IWDG_WriteAccess_Enable） */
#define WDG_KEY_RELOAD        0xAAAAu   /* 重装计数器（喂狗） */
#define WDG_KEY_START         0xCCCCu   /* 启动（★ 启动后只能靠复位关掉） */

#define WDG_SR_PVU            0x0001u   /* 预分频更新中 */
#define WDG_SR_RVU            0x0002u   /* 重装值更新中 */

/* ----------------------------------------------------------------------------
 * RCC->CSR 的复位标志位号
 * ★ 本工程的 StdPeriph 副本里 **stm32f4xx_rcc.h 没有定义 RCC_CSR_* 宏**
 *   （我核过：该头文件里 CSR / RSTF 零命中）⇒ 只能照手册自己写位号。
 *   位号取自 STM32F4 参考手册 RCC_CSR 寄存器。**编不过就说明位号/字段名错了，
 *   别顺手改成猜的。**
 * ------------------------------------------------------------------------- */
#define WDG_CSR_LPWRRSTF      0x80000000u   /* bit31 低功耗复位 */
#define WDG_CSR_WWDGRSTF      0x40000000u   /* bit30 窗口看门狗复位 */
#define WDG_CSR_IWDGRSTF      0x20000000u   /* bit29 独立看门狗复位 ★本模块关心的 */
#define WDG_CSR_SFTRSTF       0x10000000u   /* bit28 软件复位 */
#define WDG_CSR_PORRSTF       0x08000000u   /* bit27 上电/掉电复位 */
#define WDG_CSR_PINRSTF       0x04000000u   /* bit26 NRST 引脚复位 */
#define WDG_CSR_BORRSTF       0x02000000u   /* bit25 欠压复位 */
#define WDG_CSR_RMVF          0x01000000u   /* bit24 清所有复位标志 */

/* 狗到底启没启。★ 存在的意义：让"装了没装"是**能打出来验的**，不是靠相信。 */
static uint8_t s_started = 0u;

/* ============================================================================
 * ① 初始化 —— App 初始化序列里调一次
 * ========================================================================= */
void WDG_Init(void)
{
    s_started = 0u;

#if (WDG_ENABLE != 0)
    /* ---- ① 调试期友好：核 halted 时让 IWDG 停表 ----
     * 不设这一位，用调试器单步两秒板子就复位。
     * ★ 直接写 DBGMCU 寄存器，不走 DBGMCU_Config()：那个函数在
     *   Lib/src/stm32f4xx_dbgmcu.c 里，而该文件**不在本工程编译列表**里
     *   ⇒ 调它会变成"链接期未定义符号"。本模块的规矩是**只依赖寄存器**。 */
    DBGMCU->APB1FZ |= DBGMCU_IWDG_STOP;

    /* ---- ② 设预分频和重装值，并等两个更新标志落定 ----
     * ★ 必须等 PVU/RVU 清零：不等就启动，可能拿着还没生效的旧值计时。 */
    IWDG->KR  = WDG_KEY_UNLOCK;
    IWDG->PR  = (uint32_t)WDG_PR_VALUE;
    IWDG->RLR = (uint32_t)WDG_RELOAD_VALUE;
    while ((IWDG->SR & WDG_SR_PVU) != 0u) { }
    while ((IWDG->SR & WDG_SR_RVU) != 0u) { }

    /* ---- ③ 先喂一次，再启动 ----
     * ★ 顺序不能反：先启动后喂的话，中间那几微秒也在计时（虽然不影响 16 s，
     *   但"启动了却没喂过"是个坏习惯，别留在模板里）。 */
    IWDG->KR = WDG_KEY_RELOAD;
    IWDG->KR = WDG_KEY_START;
    s_started = 1u;

    printf("[WDG] IWDG 已启动：预分频=256 重装=%u ⇒ 标称 %u ms（LSI ±50%%，最坏约 10.9 s）\r\n",
           (unsigned)WDG_RELOAD_VALUE,
           (unsigned)((WDG_RELOAD_VALUE * 256u) / 32u));   /* /32 即为 ms（32 kHz）*/
#else
    printf("[WDG] 未启用（WDG_ENABLE=%u）—— 本模块不产生任何机器码\r\n",
           (unsigned)WDG_ENABLE);
#endif
}

/* ============================================================================
 * ② 喂狗 —— 主循环每圈调一次
 * ========================================================================= */
void WDG_Feed(void)
{
#if (WDG_ENABLE != 0)
    IWDG->KR = WDG_KEY_RELOAD;
#endif
}

/* ============================================================================
 * ③ WDG_FeedLong —— ★ 这个函数存在的意义是**把语义固定下来**
 * ============================================================================
 * 语义 = "我要开始一段长操作了"。现在它就是喂一次（和 WDG_Feed 完全等价）。
 * ★ 那为什么要单独留一个名字？因为**唯一喂不进去的地方（IAP 擦除）将来若要
 *   改成"临时把超时放长"，实现就改在这里，所有调用点一个字都不用动。**
 *   ⇒ 现在就把调用点写成 WDG_FeedLong()，将来升级是改一个实现，不是改 N 个调用点。
 *
 * ⚠️ 但**别指望它现在能救长阻塞**：它只是把"喂"这个动作提前一点，
 *   真正拉长的（字库 11 s、录音 13~14 s）必须在**自己的循环里**反复喂。
 * ========================================================================= */
void WDG_FeedLong(void)
{
#if (WDG_ENABLE != 0)
    IWDG->KR = WDG_KEY_RELOAD;
#endif
}

/* ============================================================================
 * ④ 查询 / 复位原因
 * ========================================================================= */
uint8_t WDG_Started(void)
{
    return s_started;
}

uint8_t WDG_LastResetWasWatchdog(void)
{
    /* 只读，不清标志 —— 谁想清谁自己调 WDG_ClearResetFlags()，不做隐藏副作用 */
    return (uint8_t)(((RCC->CSR & (WDG_CSR_IWDGRSTF | WDG_CSR_WWDGRSTF)) != 0u) ? 1u : 0u);
}

void WDG_ClearResetFlags(void)
{
    RCC->CSR |= WDG_CSR_RMVF;
}

void WDG_PrintResetCause(void)
{
    uint32_t csr = RCC->CSR;

    /* 一行够用：先报最重要的那一件（狗咬没咬），再补其余。 */
    printf("[WDG] 上次复位原因：%s",
           ((csr & WDG_CSR_IWDGRSTF) != 0u) ? "★ 独立看门狗(IWDG)咬的" : "");

    if ((csr & WDG_CSR_WWDGRSTF) != 0u) { printf("窗口看门狗 "); }
    if ((csr & WDG_CSR_LPWRRSTF) != 0u) { printf("低功耗 "); }
    if ((csr & WDG_CSR_SFTRSTF)  != 0u) { printf("软件复位 "); }
    if ((csr & WDG_CSR_PORRSTF)  != 0u) { printf("上电/掉电 "); }
    if ((csr & WDG_CSR_PINRSTF)  != 0u) { printf("NRST 引脚 "); }
    if ((csr & WDG_CSR_BORRSTF)  != 0u) { printf("欠压 "); }
    printf("\r\n");

    /* ★ 清掉，否则下次开机读到的还是这一次的现场，永远分不清是新咬的还是旧的 */
    WDG_ClearResetFlags();
}
