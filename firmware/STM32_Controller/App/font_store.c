/**
 * @file font_store.c
 * 用途：在 W25Q SPI Flash 中定位、校验和查询扩展字库。
 * 使用：W25Q 初始化后调用字库初始化；绘字时按 Unicode 查升序索引，再读取对应点阵。
 * 时序与边界：头字段、字形大小和容量不满足约束时拒绝启用；索引缓存属于本模块，不与音频缓存共用。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "font_store.h"
#include "w25q.h"
#include "w25q_img.h"

/* ---------- 内部状态 ---------- */
static uint8_t s_ready = 0u;
static uint16_t s_count = 0u;
static uint32_t s_len = 0u; /* 已安装镜像的实际长度 */
static uint32_t s_crc = 0u; /* 标记里记的 CRC32（不校验它本身，只用来比对） */
static uint8_t s_err = FONTSTORE_OK;
/* 状态代号，`FontStore_Gen()` 的原因见 font_store.h。只加不减，回绕不处理
 * （2^32 次装卸 ≈ 不可能，且回绕的后果只是"多查一次 SPI"，不是错）。 */
static uint32_t s_gen = 0u;

/*  索引常驻 RAM：8192 B。查字因此从"13 次 SPI 事务"降到**命中 1 次 /
 *   未命中 0 次**，顺带白拿一道"索引严格升序"的完整性校验。
 *  必须 static —— 栈只有 2KB（`startup_stm32f40_41xxx.s:41`）。 */
static uint16_t s_cp[FONTSTORE_MAX_GLYPHS];

/* ---------- 小端读写（本工程是 ARM Cortex-M4，小端） ---------- */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* 一整块都是同一个字节？用来判"全 0x00 / 全 0xFF"。 */
static uint8_t all_byte(const uint8_t *p, uint32_t len, uint8_t v)
{
    uint32_t i;
    for (i = 0u; i < len; i++)
    {
        if (p[i] != v)
        {
            return 0u;
        }
    }
    return 1u;
}

/* ==================================================================
 * 头部校验（纯函数，不碰 Flash）
 * ------------------------------------------------------------------
 * 6 个字段**逐项校**，不只校 magic。
 * 四条内容判据（count 上下界、len 式子、镜像区上限）也在这儿。
 * ================================================================== */
uint8_t FontStore_CheckHeader(const uint8_t *hdr16, uint32_t len)
{
    uint32_t n;

    if (hdr16 == 0)
    {
        return FONTSTORE_ERR_PARAM;
    }

    if ((hdr16[0] != 'F') || (hdr16[1] != '1') || (hdr16[2] != '6') || (hdr16[3] != 'B'))
    {
        return FONTSTORE_ERR_HEADER;
    }

    if (rd16(&hdr16[4]) != FONTSTORE_VERSION)
    {
        return FONTSTORE_ERR_HEADER;
    }
    if (hdr16[6] != (uint8_t)FONTSTORE_W)
    {
        return FONTSTORE_ERR_HEADER;
    }
    if (hdr16[7] != (uint8_t)FONTSTORE_H)
    {
        return FONTSTORE_ERR_HEADER;
    }
    if (rd32(&hdr16[12]) != FONTSTORE_GLYPH_BYTES)
    {
        return FONTSTORE_ERR_HEADER;
    }

    n = rd32(&hdr16[8]);
    /*  这一条是 RAM 数组的越界闸门，必须在任何拷贝之前 —— 见文件头第 3 条。 */
    if ((n == 0u) || (n > FONTSTORE_MAX_GLYPHS))
    {
        return FONTSTORE_ERR_COUNT;
    }
    if (len != FONTSTORE_IMAGE_BYTES(n))
    {
        return FONTSTORE_ERR_LEN;
    }
    if (len > FONTSTORE_MAX_BYTES)
    {
        return FONTSTORE_ERR_LEN;
    }

    return FONTSTORE_OK;
}

/* ==================================================================
 * 上电 / 刚灌完 —— 装载
 * ================================================================== */
uint8_t FontStore_Init(void)
{
    uint8_t mk[FONTSTORE_MARK_BYTES];
    uint8_t hdr[FONTSTORE_HDR_BYTES];
    uint8_t probe[FONTSTORE_GLYPH_BYTES];
    uint32_t mlen;
    uint32_t gbase;
    uint32_t i;
    uint8_t rc;

    s_ready = 0u;
    s_count = 0u;
    s_len = 0u;
    s_crc = 0u;
    s_err = FONTSTORE_OK;
    /*  放在最前面：从这里往下无论走哪条 return，字库状态都"动过了"
     *   （成功是换了内容，失败是退成了不可用）⇒ 查表缓存一律作废。 */
    s_gen++;

    /*  W25Q 在线？不在线就别往下问（`w25q.h:134`，认不出容量返回 0）。 */
    if (W25Q_CapacityBytes() == 0u)
    {
        return (s_err = FONTSTORE_ERR_SPI);
    }

    /*  完成标记 —— 两阶段提交的"第二刀"。没有它 = 没装过 / 灌一半断过电。 */
    rc = W25Q_Read(FONTSTORE_MARK_ADDR, mk, FONTSTORE_MARK_BYTES);
    if (rc != W25Q_OK)
    {
        return (s_err = FONTSTORE_ERR_SPI);
    }
    if ((mk[0] != 'F') || (mk[1] != '1') || (mk[2] != '6') || (mk[3] != 'R'))
    {
        return (s_err = FONTSTORE_ERR_NO_MARK);
    }
    if (rd16(&mk[4]) != FONTSTORE_VERSION)
    {
        return (s_err = FONTSTORE_ERR_MARK_HDR);
    }
    if (rd16(&mk[6]) != 0u)
    {
        return (s_err = FONTSTORE_ERR_MARK_HDR);
    }

    mlen = rd32(&mk[8]);

    /*  镜像头部 */
    rc = W25Q_Read(FONTSTORE_BASE, hdr, FONTSTORE_HDR_BYTES);
    if (rc != W25Q_OK)
    {
        return (s_err = FONTSTORE_ERR_SPI);
    }

    rc = FontStore_CheckHeader(hdr, mlen);
    if (rc != FONTSTORE_OK)
    {
        return (s_err = rc);
    }

    s_count = (uint16_t)rd32(&hdr[8]);
    s_len = mlen;
    s_crc = rd32(&mk[12]);

    /*  索引读进 RAM。小端 uint16 连续存放 ⇒ 直接按字节读进 uint16 数组即可。 */
    rc = W25Q_Read(FONTSTORE_BASE + FONTSTORE_HDR_BYTES, (uint8_t *)s_cp,
                   (uint32_t)s_count * FONTSTORE_INDEX_BYTES);
    if (rc != W25Q_OK)
    {
        return (s_err = FONTSTORE_ERR_SPI);
    }

    /*  索引必须**严格**升序（重复也算坏）：这是白捡的一道完整性校验，
     *    也是二分查找成立的前提。 */
    for (i = 1u; i < (uint32_t)s_count; i++)
    {
        if (s_cp[i] <= s_cp[i - 1u])
        {
            return (s_err = FONTSTORE_ERR_INDEX);
        }
    }

    /*  探首、末两个字模 —— 抓"索引写了、字模区没写"那种半死状态。 */
    gbase = FONTSTORE_BASE + FONTSTORE_HDR_BYTES + (uint32_t)s_count * FONTSTORE_INDEX_BYTES;

    rc = W25Q_Read(gbase, probe, FONTSTORE_GLYPH_BYTES);
    if (rc != W25Q_OK)
    {
        return (s_err = FONTSTORE_ERR_SPI);
    }
    if ((all_byte(probe, FONTSTORE_GLYPH_BYTES, 0x00u) != 0u) ||
        (all_byte(probe, FONTSTORE_GLYPH_BYTES, 0xFFu) != 0u))
    {
        return (s_err = FONTSTORE_ERR_GLYPH);
    }

    rc = W25Q_Read(gbase + FONTSTORE_GLYPH_BYTES * ((uint32_t)s_count - 1u), probe,
                   FONTSTORE_GLYPH_BYTES);
    if (rc != W25Q_OK)
    {
        return (s_err = FONTSTORE_ERR_SPI);
    }
    if ((all_byte(probe, FONTSTORE_GLYPH_BYTES, 0x00u) != 0u) ||
        (all_byte(probe, FONTSTORE_GLYPH_BYTES, 0xFFu) != 0u))
    {
        return (s_err = FONTSTORE_ERR_GLYPH);
    }

    s_ready = 1u;
    return FONTSTORE_OK;
}

/* ==================================================================
 * 状态查询
 * ================================================================== */
uint8_t FontStore_Ready(void)
{
    return s_ready;
}
uint16_t FontStore_Count(void)
{
    return s_ready ? s_count : 0u;
}
uint8_t FontStore_LastError(void)
{
    return s_err;
}
uint32_t FontStore_Gen(void)
{
    return s_gen;
}

uint32_t FontStore_Base(void)
{
    return FONTSTORE_BASE;
}
uint32_t FontStore_MarkAddr(void)
{
    return FONTSTORE_MARK_ADDR;
}
uint32_t FontStore_ImageBytes(void)
{
    return s_ready ? s_len : 0u;
}

const char *FontStore_ErrName(uint8_t rc)
{
    switch (rc)
    {
    case FONTSTORE_OK:
        return "OK";
    case FONTSTORE_ERR_NO_MARK:
        return "NO_MARK";
    case FONTSTORE_ERR_MARK_HDR:
        return "MARK_HDR";
    case FONTSTORE_ERR_HEADER:
        return "HEADER";
    case FONTSTORE_ERR_COUNT:
        return "COUNT";
    case FONTSTORE_ERR_LEN:
        return "LEN";
    case FONTSTORE_ERR_INDEX:
        return "INDEX";
    case FONTSTORE_ERR_GLYPH:
        return "GLYPH";
    case FONTSTORE_ERR_SPI:
        return "SPI";
    case FONTSTORE_ERR_NOTREADY:
        return "NOTREADY";
    case FONTSTORE_ERR_CRC:
        return "CRC";
    case FONTSTORE_ERR_PARAM:
        return "PARAM";
    default:
        return "?";
    }
}

/* ==================================================================
 * 查字 —— 热路径，**这里一行 printf 都没有**
 * ------------------------------------------------------------------
 * 二分全在 RAM 的 `s_cp[]` 上走 ⇒ **未命中一次 SPI 都不发**；
 * 命中才发 1 次 32 字节读。
 * ================================================================== */
uint8_t FontStore_Glyph(uint16_t cp, uint8_t *out32)
{
    uint16_t lo;
    uint16_t hi;
    uint16_t mid;
    uint32_t off;

    if ((s_ready == 0u) || (out32 == 0))
    {
        return 0u;
    }

    lo = 0u;
    hi = s_count; /* 半开区间 [lo, hi) */

    while (lo < hi)
    {
        /* 与 font.c 同一个写法：不用 (lo+hi)/2，16 位上两者等价但这么写不会溢出 */
        mid = (uint16_t)(lo + ((uint16_t)(hi - lo) >> 1));

        if (s_cp[mid] == cp)
        {
            off = FONTSTORE_BASE + FONTSTORE_HDR_BYTES + (uint32_t)s_count * FONTSTORE_INDEX_BYTES +
                  (uint32_t)mid * FONTSTORE_GLYPH_BYTES;

            if (W25Q_Read(off, out32, FONTSTORE_GLYPH_BYTES) != W25Q_OK)
            {
                return 0u;
            }
            return 1u;
        }

        if (s_cp[mid] < cp)
        {
            lo = (uint16_t)(mid + 1u);
        }
        else
        {
            hi = mid;
        }
    }

    return 0u;
}

/* ==================================================================
 * 标记（两阶段提交的那一半）
 * ================================================================== */
uint8_t FontStore_MarkErase(void)
{
    uint32_t bad = 0u;

    if (W25QImg_EraseRange(FONTSTORE_MARK_ADDR, FONTSTORE_MARK_BYTES, &bad) != W25QIMG_OK)
    {
        return (s_err = FONTSTORE_ERR_SPI);
    }
    return FONTSTORE_OK;
}

uint8_t FontStore_MarkWrite(uint32_t len, uint32_t crc)
{
    uint8_t rec[FONTSTORE_MARK_BYTES];

    rec[0] = 'F';
    rec[1] = '1';
    rec[2] = '6';
    rec[3] = 'R';
    wr16(&rec[4], (uint16_t)FONTSTORE_VERSION);
    wr16(&rec[6], 0u);
    wr32(&rec[8], len);
    wr32(&rec[12], crc);

    /*  走到这里说明镜像已经写完并且逐块校验过 —— 这一步**才是**"提交"。 */
    if (W25QImg_WriteRecord(FONTSTORE_MARK_ADDR, rec, FONTSTORE_MARK_BYTES) != W25QIMG_OK)
    {
        return (s_err = FONTSTORE_ERR_SPI);
    }
    return FONTSTORE_OK;
}

uint8_t FontStore_Verify(uint32_t *crc_out)
{
    uint32_t crc = 0u;

    if (s_ready == 0u)
    {
        return (s_err = FONTSTORE_ERR_NOTREADY);
    }

    if (W25QImg_Crc32Range(FONTSTORE_BASE, s_len, &crc) != W25QIMG_OK)
    {
        return (s_err = FONTSTORE_ERR_SPI);
    }

    if (crc_out != 0)
    {
        *crc_out = crc;
    }

    if (crc != s_crc)
    {
        return (s_err = FONTSTORE_ERR_CRC);
    }
    return FONTSTORE_OK;
}

uint8_t FontStore_Uninstall(void)
{
    uint8_t rc = FontStore_MarkErase();

    s_ready = 0u;
    s_count = 0u;
    s_len = 0u;
    s_crc = 0u;
    s_gen++; /* 查表缓存作废：现在起字库换成"没有字库" */
    /*  只清标记、**不动镜像**。
     *    但**别指望"再 Init 一次就回来了"** —— 标记正是 Init 的判据，
     *      擦掉它之后 Init 必然回 `NO_MARK`。镜像没被毁，只是不再被认可；
     *      想再装回来必须重新走一次安装（重灌 ≈30s），或者由知道 CRC 的
     *      一方重新 `MarkWrite()`。本轮没有提供"只补标记"的命令，
     *      这是有意的：少一条命令就少一个"把半截镜像认证成好的"的机会。 */
    return rc;
}
