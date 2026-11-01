/* ==================================================================
 * w25q_img.c —— W25Q 镜像写内核（页拆分 + 回读比对 + WEL 判因）
 * ------------------------------------------------------------------
 * 接口语义、返回码、以及"为什么不自己碰 SPI"，全在 w25q_img.h 里。
 * 这里只把三件容易写错的事说透：
 *
 * 【1. 为什么每写完一小段就要读回整整一页】
 *   本项目踩过的那个坑（`问题日志_AI速查.md` 硬件段）长得是这样的：
 *     WEL 没置上 ⇒ 页编程被器件**静默丢弃** ⇒ 什么都不写 ⇒ 读回来还是 0xFF
 *     ⇒ 而所有 API 都返回 0（成功）⇒ 全程零报错。
 *   ⇒ "写完读回来对一遍"是**唯一**能把它揪出来的动作。不抽样、逐字节：
 *     这个坑的形态是"整片 0xFF / 整片按位与"，抽样正好会踩空。
 *
 * 【2. WEL 是"判因"，不是"判据"】★ 顺序很要紧，别改成先查 WEL
 *   器件在一个写周期**结束时自动清 WEL**。所以"刚写完、WEL 还是 1"意味着
 *   这次编程**压根没执行**。但反过来不成立：
 *     · 万一遇到 WEL 时序有怪癖的兼容片，"写成功但 WEL=1"是有可能的
 *       ⇒ 那时若先查 WEL 就会**误报失败**，比不查还坏。
 *   ⇒ 所以这里先回读比对，**只有数据不对时**才去问 WEL，用它把失败分成两种：
 *       数据不对 + WEL=1  ⇒ `ERR_WEL`     （编程没执行：查 CS 时序 / 供电）
 *       数据不对 + WEL=0  ⇒ `ERR_VERIFY`  （写进去了但值不对：多半是没擦干净）
 *   ⇒ 报错时**别只看返回码**，这两个码指向的原因完全不同。
 *
 * 【3. 绝不跨页】按 `n = min(剩余, 256 - (addr % 256))` 拆。
 *   `w25q.h:140-143` 写得很清楚：跨页会被器件**回卷到页首覆盖**，且**不报错**。
 *   （顺带一提：哪怕拆分逻辑写错了，第 1 条的回读比对也会当场发现 ——
 *     回卷会把数据放到页首，我们要比的那一段读回来就对不上了。）
 * ================================================================== */
#include "w25q_img.h"
#include "w25q.h"

/* 都从 w25q.h 取，不写死数字 */
#define IMG_PAGE    W25Q_PAGE_SIZE        /* 256  */
#define IMG_SECTOR  W25Q_SECTOR_SIZE      /* 4096 */
#define IMG_BLOCK   W25Q_BLOCK_SIZE       /* 65536 */

/* ------------------------------------------------------------------
 * 两块 256 字节的 static 缓冲。
 * ★ **必须是 static，不能放栈上** —— 本工程栈只有 2KB
 *   （`ST32/AIAssistant/Core/startup_stm32f40_41xxx.s:41`），栈上放个 256 字节数组
 *   在调用链深一点的地方就是**静默爆栈**（现象是随机跑飞，极难查）。
 *
 * `s_page`：回读比对 / 逐字节校验 / CRC 流式读，三处**复用**同一块
 *   （这三件事不会互相嵌套，且都不调用对方）。
 * `s_chunk`：只有 WriteRange 用它从数据源拉一块出来，**不能**和 s_page
 *   合并 —— WriteRange 会拿着 s_chunk 去调 WriteChunk，而后者要用 s_page。
 * ------------------------------------------------------------------ */
static uint8_t s_page[IMG_PAGE];
static uint8_t s_chunk[IMG_PAGE];

/* ==================================================================
 * 页面拆分 + 页编程 + 回读比对 + WEL 判因
 * ================================================================== */
uint8_t W25QImg_WriteChunk(uint32_t addr, const uint8_t *buf, uint32_t len,
                           uint32_t *fail_off)
{
    uint32_t done = 0u;
    uint8_t  rc;

    if ((buf == 0) || (len == 0u))                     { return W25QIMG_ERR_PARAM; }
    if ((addr + len) < addr)                           { return W25QIMG_ERR_PARAM; } /* 回绕 */
    if ((addr + len) > W25Q_ADDR_MAX)                  { return W25QIMG_ERR_PARAM; }

    while (done < len)
    {
        uint32_t here  = addr + done;
        uint32_t room  = IMG_PAGE - (here % IMG_PAGE);  /* 到本页末还剩几个字节 */
        uint32_t page0 = here - (here % IMG_PAGE);      /* 本页首地址（回读用） */
        uint32_t n     = len - done;
        uint32_t i;
        uint8_t  sr1   = 0u;

        if (n > room)     { n = room; }      /* ★ 这一句就是"绝不跨页" */
        if (n > IMG_PAGE) { n = IMG_PAGE; }  /* 到不了，兜底 */

        rc = W25Q_PageProgram(here, &buf[done], (uint16_t)n);
        if (rc != W25Q_OK)
        {
            if (fail_off != 0) { *fail_off = here; }
            return W25QIMG_ERR_PROGRAM;
        }

        /* ---- 回读：读**整页**，比**刚写的那一段** ---- */
        rc = W25Q_Read(page0, s_page, IMG_PAGE);
        if (rc != W25Q_OK)
        {
            if (fail_off != 0) { *fail_off = here; }
            return W25QIMG_ERR_SPI;
        }

        for (i = 0u; i < n; i++)
        {
            if (s_page[(here - page0) + i] != buf[done + i]) { break; }
        }

        if (i < n)
        {
            /* ---- 数据不对：再问一句 WEL，把失败分成两种 ---- */
            uint8_t wel = 0u;

            if (fail_off != 0) { *fail_off = here + i; }

            if (W25Q_ReadStatus(1u, &sr1) == W25Q_OK)
            {
                wel = (uint8_t)((sr1 >> 1) & 1u);
            }
            /* wel 读不到时按 0 走 ⇒ 报 ERR_VERIFY（保守：不冤枉成 WEL 问题） */
            return (wel != 0u) ? W25QIMG_ERR_WEL : W25QIMG_ERR_VERIFY;
        }

        done += n;
    }

    return W25QIMG_OK;
}

/* ==================================================================
 * 从回调数据源拉数据写（建在 WriteChunk 之上）
 * ================================================================== */
uint8_t W25QImg_WriteRange(uint32_t addr, uint32_t len, W25QImg_Source src,
                           void *ctx, uint8_t skip_if_equal, uint32_t *fail_off)
{
    uint32_t done = 0u;
    uint8_t  rc;

    if ((src == 0) || (len == 0u))          { return W25QIMG_ERR_PARAM; }
    if ((addr + len) < addr)                { return W25QIMG_ERR_PARAM; }
    if ((addr + len) > W25Q_ADDR_MAX)       { return W25QIMG_ERR_PARAM; }

    while (done < len)
    {
        uint32_t want = len - done;
        uint32_t got;

        if (want > IMG_PAGE) { want = IMG_PAGE; }

        got = src(ctx, done, s_chunk, want);
        if (got == 0u)
        {
            if (fail_off != 0) { *fail_off = addr + done; }
            return W25QIMG_ERR_SRC;
        }
        if (got > want) { got = want; }   /* 回调不许给多 */

        if (skip_if_equal != 0u)
        {
            /* 已经对的就不编程 —— 省的是**擦写寿命**，不只是时间。
             * ⚠️ 判据是"跟源一致"，不是"不是 0xFF"，别搞混。 */
            if (W25QImg_VerifyRange(addr + done, s_chunk, got, 0) == W25QIMG_OK)
            {
                done += got;
                continue;
            }
        }

        rc = W25QImg_WriteChunk(addr + done, s_chunk, got, fail_off);
        if (rc != W25QIMG_OK) { return rc; }

        done += got;
    }

    return W25QIMG_OK;
}

/* ==================================================================
 * 逐字节比对（RAM 里的 expect vs Flash）
 * ================================================================== */
uint8_t W25QImg_VerifyRange(uint32_t addr, const uint8_t *expect, uint32_t len,
                            uint32_t *fail_off)
{
    uint32_t done = 0u;
    uint8_t  rc;

    if ((expect == 0) || (len == 0u))  { return W25QIMG_ERR_PARAM; }
    if ((addr + len) < addr)           { return W25QIMG_ERR_PARAM; }
    if ((addr + len) > W25Q_ADDR_MAX)  { return W25QIMG_ERR_PARAM; }

    while (done < len)
    {
        uint32_t n = len - done;
        uint32_t i;

        if (n > IMG_PAGE) { n = IMG_PAGE; }

        rc = W25Q_Read(addr + done, s_page, n);
        if (rc != W25Q_OK)
        {
            if (fail_off != 0) { *fail_off = addr + done; }
            return W25QIMG_ERR_SPI;
        }

        for (i = 0u; i < n; i++)
        {
            if (s_page[i] != expect[done + i])
            {
                if (fail_off != 0) { *fail_off = addr + done + i; }
                return W25QIMG_ERR_VERIFY;
            }
        }

        done += n;
    }

    return W25QIMG_OK;
}

/* ==================================================================
 * 擦除 —— 自动对齐到扇区；能整块的走块擦
 * ================================================================== */
uint8_t W25QImg_EraseRange(uint32_t addr, uint32_t len, uint32_t *fail_addr)
{
    uint32_t a;
    uint32_t end;
    uint8_t  rc;

    if (len == 0u)         { return W25QIMG_ERR_PARAM; }
    if ((addr + len) < addr) { return W25QIMG_ERR_PARAM; }

    /* ★ 擦除的粒度就是**整个 4KB 扇区**，没有"只擦一半"这回事
     *   ⇒ 只能起点向下、终点向上各凑一个扇区（`项目地图.md:811` 记过
     *     另一个跟它同源的事故：元数据"两份互备"就是被这条吃掉的）。 */
    a   = addr & ~(IMG_SECTOR - 1u);
    end = (addr + len + (IMG_SECTOR - 1u)) & ~(IMG_SECTOR - 1u);

    if (end < a)                      { return W25QIMG_ERR_PARAM; }  /* 对齐时又回绕了 */
    if (end > W25Q_ADDR_MAX)          { return W25QIMG_ERR_PARAM; }

    /* ① 头部：a 还没落到 64KB 边界上的那几个扇区 */
    while ((a < end) && ((a & (IMG_BLOCK - 1u)) != 0u))
    {
        rc = W25Q_SectorErase(a);
        if (rc != W25Q_OK) { if (fail_addr != 0) { *fail_addr = a; } return W25QIMG_ERR_ERASE; }
        a += IMG_SECTOR;
    }

    /* ② 中间：整块整块擦 —— 这一段就是"比 16 次扇区擦快 5 倍"的来源。
     *    ⚠️ 一次块擦最长 800ms，**期间 CPU 取指会被 Flash 控制器停住**，
     *       所以这是整个上传流程里第一个真·阻塞窗口。 */
    while ((a + IMG_BLOCK) <= end)
    {
        rc = W25Q_BlockErase(a);
        if (rc != W25Q_OK) { if (fail_addr != 0) { *fail_addr = a; } return W25QIMG_ERR_ERASE; }
        a += IMG_BLOCK;
    }

    /* ③ 尾巴：剩下的扇区 */
    while (a < end)
    {
        rc = W25Q_SectorErase(a);
        if (rc != W25Q_OK) { if (fail_addr != 0) { *fail_addr = a; } return W25QIMG_ERR_ERASE; }
        a += IMG_SECTOR;
    }

    return W25QIMG_OK;
}

/* ==================================================================
 * 全 0xFF（= 已擦）检查
 * ================================================================== */
uint8_t W25QImg_IsErased(uint32_t addr, uint32_t len, uint32_t *first_bad_off)
{
    uint32_t done = 0u;
    uint8_t  rc;

    if (len == 0u)                     { return W25QIMG_ERR_PARAM; }
    if ((addr + len) < addr)           { return W25QIMG_ERR_PARAM; }
    if ((addr + len) > W25Q_ADDR_MAX)  { return W25QIMG_ERR_PARAM; }

    while (done < len)
    {
        uint32_t n = len - done;
        uint32_t i;

        if (n > IMG_PAGE) { n = IMG_PAGE; }

        rc = W25Q_Read(addr + done, s_page, n);
        if (rc != W25Q_OK)
        {
            if (first_bad_off != 0) { *first_bad_off = addr + done; }
            return W25QIMG_ERR_SPI;
        }

        for (i = 0u; i < n; i++)
        {
            if (s_page[i] != 0xFFu)
            {
                if (first_bad_off != 0) { *first_bad_off = addr + done + i; }
                return W25QIMG_ERR_NOT_ERASED;
            }
        }

        done += n;
    }

    return W25QIMG_OK;
}

/* ==================================================================
 * 写一条定长小记录（完成标记用）
 * ------------------------------------------------------------------
 * ★ **不擦** —— 它只负责"把 1 写成 0"。目标必须是已经是 0xFF 的空扇区，
 *   标记独占一个扇区（`font_store.h` 的布局）就是为了让这件事永远成立。
 *   要改标记就先 `W25QImg_EraseRange` 那个扇区，别指望这里兜底。
 * ================================================================== */
uint8_t W25QImg_WriteRecord(uint32_t addr, const uint8_t *rec, uint32_t len)
{
    if (len > IMG_PAGE) { return W25QIMG_ERR_PARAM; }
    return W25QImg_WriteChunk(addr, rec, len, 0);
}

/* ==================================================================
 * CRC32 —— 与 PC 端 `zlib.crc32` 逐位同值
 * ------------------------------------------------------------------
 * 反射式多项式 0xEDB88320、初值 0xFFFFFFFF、末尾异或 0xFFFFFFFF。
 * 这三个数**就是 zlib 的定义**，改任何一个两边就对不上。
 * 不用查表（省 1KB 表 + 免了"表生成对不对"这类问题）：按位算，
 * 127686 字节约 30ms，在整个流程里可以忽略。
 * ================================================================== */
uint32_t W25QImg_Crc32Init(void)
{
    return 0xFFFFFFFFu;
}

uint32_t W25QImg_Crc32Update(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    uint8_t  b;

    if (buf == 0) { return crc; }

    for (i = 0u; i < len; i++)
    {
        crc ^= (uint32_t)buf[i];
        for (b = 0u; b < 8u; b++)
        {
            if ((crc & 1u) != 0u) { crc = (crc >> 1) ^ 0xEDB88320u; }
            else                  { crc = (crc >> 1); }
        }
    }
    return crc;
}

uint32_t W25QImg_Crc32Final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

uint8_t W25QImg_Crc32Range(uint32_t addr, uint32_t len, uint32_t *crc_out)
{
    uint32_t done = 0u;
    uint32_t crc;
    uint8_t  rc;

    if ((crc_out == 0) || (len == 0u)) { return W25QIMG_ERR_PARAM; }
    if ((addr + len) < addr)           { return W25QIMG_ERR_PARAM; }
    if ((addr + len) > W25Q_ADDR_MAX)  { return W25QIMG_ERR_PARAM; }

    crc = W25QImg_Crc32Init();

    while (done < len)
    {
        uint32_t n = len - done;

        if (n > IMG_PAGE) { n = IMG_PAGE; }

        rc = W25Q_Read(addr + done, s_page, n);
        if (rc != W25Q_OK) { return W25QIMG_ERR_SPI; }

        crc   = W25QImg_Crc32Update(crc, s_page, n);
        done += n;
    }

    *crc_out = W25QImg_Crc32Final(crc);
    return W25QIMG_OK;
}
