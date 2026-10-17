/**
 * @file w25q_selftest.c
 * 用途：W25Q 的在线诊断和受控读写自测入口。
 * 使用：由启动自检或命令调用，只读诊断先检查芯片 ID/状态/数据；写测需明确选择测试区域。
 * 时序与边界：不能把诊断函数当格式化操作；保留字库/图片区域，区分只读检查与破坏性写测。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "w25q.h"
#include <stdio.h>

/* ------------------------------------------------------------------
 * 容量代码 → 型号名。**只用于打印**，认不出返回 "?"。
 * 容量 = 2^代码 字节，代码就在 JEDEC ID 的第 3 个字节里。
 * ------------------------------------------------------------------ */
static const char *w25q_part_name(uint8_t code)
{
    switch (code)
    {
    case 0x12u:
        return "W25Q20 / 256KB";
    case 0x13u:
        return "W25Q40 / 512KB";
    case 0x14u:
        return "W25Q80 / 1MB";
    case 0x15u:
        return "W25Q16 / 2MB";
    case 0x16u:
        return "W25Q32 / 4MB";
    case 0x17u:
        return "W25Q64 / 8MB";
    case 0x18u:
        return "W25Q128 / 16MB";
    default:
        return "?（容量代码不在常见范围）";
    }
}

/* ------------------------------------------------------------------
 * 上电自检：**只读**。返回 W25Q_OK 表示芯片在线且 ID 认得出，否则是错误码。
 *
 * 这一段顺带回答了本项目悬了很久的两个问题：
 *    板载 W25Q 到底多大 —— 直接打出来（`规划部分/计划蓝图.md` 附录 B #2）；
 *    SPI1 这条通路到底通不通 —— 这是**全工程第一次**用 SPI1，
 *      在此之前一行 SPI 代码都没有。
 * ------------------------------------------------------------------ */
uint8_t W25Q_SelfTest(void)
{
    const uint8_t *pid;
    uint8_t sr1 = 0u;
    uint8_t sr2 = 0u;
    uint8_t sr3 = 0u;
    uint8_t dump[16];
    uint8_t rc;
    uint32_t cap;
    uint32_t i;

    rc = W25Q_Init();
    pid = W25Q_ID();
    cap = W25Q_CapacityBytes();

    printf("[W25Q] JEDEC ID = %02X %02X %02X", pid[0], pid[1], pid[2]);

    if (rc != W25Q_OK)
    {
        if (rc == W25Q_ERR_NOCHIP)
        {
            printf("  → 没认到 Winbond 器件（第 1 字节期望 EF）\r\n");
        }
        else if (rc == W25Q_ERR_TIMEOUT)
        {
            printf("  → SPI 等待超时\r\n");
        }
        else if (rc == W25Q_ERR_BUSY)
        {
            printf("  → 本模块正被别处占用（重入被拒）\r\n");
        }
        else
        {
            printf("  → 初始化失败 rc=%u\r\n", (unsigned)rc);
        }
        /* 这三条按"先查最可能的"排：
         *   ID 全 00 / 全 FF 最常见的原因**不是**器件坏，而是 PB3/PB4 没切到 AF5
         *   （复位后它们是 AF0=JTAG），或者 CS 不在 PB0 / 空闲不是高。 */
        printf("[W25Q] 排查顺序：① PB3/PB4 有没有切到 AF5（w25q.c 的 w25q_gpio_af_init，"
               "必须三次单独调 GPIO_PinAFConfig）；② CS 是不是 PB0、空闲为高；"
               "③ VCC 是不是 3.3V\r\n");
        return rc;
    }

    printf("  = %s，容量 %u KB\r\n", w25q_part_name(pid[2]), (unsigned)(cap / 1024u));

    /* ---- 状态寄存器（只读） ---- */
    if (W25Q_ReadStatus(1u, &sr1) == W25Q_OK)
    {
        printf("[W25Q] SR1=0x%02X（BUSY=%u WEL=%u）", sr1, (unsigned)((sr1 >> 0) & 1u),
               (unsigned)((sr1 >> 1) & 1u));
        if ((sr1 & 0x01u) != 0u)
        {
            /* 刚上电、又没人写过，BUSY 不该是 1 —— 出现就是异常信号。 */
            printf("  ← 注意：刚上电 BUSY 不该置位，可能是复位残留或命令被误触发\r\n");
        }
        else
        {
            printf("\r\n");
        }
    }
    else
    {
        printf("[W25Q] SR1 读不到（通信有问题的另一种表现）\r\n");
    }

    if ((W25Q_ReadStatus(2u, &sr2) == W25Q_OK) && (W25Q_ReadStatus(3u, &sr3) == W25Q_OK))
    {
        printf("[W25Q] SR2=0x%02X SR3=0x%02X\r\n", sr2, sr3);
    }

    /* ---- dump 开头 16 字节：只是为了肉眼确认"读"这条通路是活的 ---- */
    rc = W25Q_Read(0u, dump, (uint32_t)sizeof(dump));
    if (rc == W25Q_OK)
    {
        printf("[W25Q] 0x000000 起 %u 字节：", (unsigned)sizeof(dump));
        for (i = 0u; i < (uint32_t)sizeof(dump); i++)
        {
            printf("%02X ", dump[i]);
        }
        printf("\r\n");
    }
    else
    {
        printf("[W25Q] 读 0x000000 失败 rc=%u\r\n", (unsigned)rc);
        return rc;
    }

    printf("[W25Q] 在线（本条自检**只读**，没擦没写）\r\n");
    return W25Q_OK;
}

/* ------------------------------------------------------------------
 * 读写自检：擦 0 号扇区 → 写 16 字节花样 → 回读比对。
 * 返回 W25Q_OK = 全对；否则是错误码（回读不符返回 W25Q_ERR_VERIFY）。
 *
 *  **会破坏 0x000000 所在的那个 4KB 扇区里的内容。**
 *    只在串口手敲时调，不要放进上电自检。
 *  跑完之后 0 号扇区里留着这 16 字节花样（不是 0xFF）。
 *    等真要往里放东西（字库）之前，先自己擦一次。
 * ------------------------------------------------------------------ */
uint8_t W25Q_SelfTestRW(void)
{
    uint8_t pat[16];
    uint8_t back[16];
    uint8_t rc;
    uint32_t i;

    printf("[W25Q] === 读写自检：将擦除 0x000000 所在的 4KB 扇区 ===\r\n");

    for (i = 0u; i < (uint32_t)sizeof(pat); i++)
    {
        pat[i] = (uint8_t)(0xA5u ^ (uint8_t)i);
    }

    rc = W25Q_SectorErase(0u);
    if (rc != W25Q_OK)
    {
        printf("[W25Q] 扇区擦除失败 rc=%u\r\n", (unsigned)rc);
        return rc;
    }

    rc = W25Q_PageProgram(0u, pat, (uint16_t)sizeof(pat));
    if (rc != W25Q_OK)
    {
        printf("[W25Q] 页编程失败 rc=%u\r\n", (unsigned)rc);
        return rc;
    }

    for (i = 0u; i < (uint32_t)sizeof(back); i++)
    {
        back[i] = 0u;
    }
    rc = W25Q_Read(0u, back, (uint32_t)sizeof(back));
    if (rc != W25Q_OK)
    {
        printf("[W25Q] 回读失败 rc=%u\r\n", (unsigned)rc);
        return rc;
    }

    for (i = 0u; i < (uint32_t)sizeof(back); i++)
    {
        if (back[i] != pat[i])
        {
            /*  分两种报法，因为指向的原因完全不同：
             *   读回全 FF ⇒ 写没生效（多半是 WEL 没置上 / 命令被截断）；
             *   读回全 00 ⇒ 读通路不通（MISO 这一根没通）。 */
            printf("[W25Q] 第 %u 字节不符：写 0x%02X、读回 0x%02X\r\n", (unsigned)i, pat[i],
                   back[i]);
            printf("[W25Q] 若整段都是 FF → 写没生效（查 WEL / CS 时序）；"
                   "整段都是 00 → 读通路不通（查 MISO）\r\n");
            return W25Q_ERR_VERIFY;
        }
    }

    printf("[W25Q] 读写自检通过：擦 → 写 → 回读 16 字节全对\r\n");
    return W25Q_OK;
}
