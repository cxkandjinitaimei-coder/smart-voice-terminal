/* ==================================================================
 * font_upload.c —— 字库的串口传输层（/font 一族）
 * ------------------------------------------------------------------
 * 协议、两处比方案原文收紧的地方、两个阻塞窗口，全在 font_upload.h 里。
 * 这里只补三条实现约定：
 *
 * 【1. 本文件的 printf 格式串**一律 ASCII**，中文只进注释】
 *   这些字符串是**协议的一部分**（PC 端按它们解析），必须与码页无关；
 *   而且新文件里少一个编码变量，就少一次 `#870-D` 的机会
 *   （`media.c` 那条"禁止汉字字面量"的规矩就是这么来的）。
 *
 * 【2. 每个 chunk 写完立刻回读比对（在 w25q_img.c 里做）】
 *   所以"写失败静默"这种坑最多只能活 112 字节 —— 报错时还带得出
 *   `fail_off`，能直接定位到镜像里的第几个字节。
 *
 * 【3. 严格顺序 + 每条回复都带"下一个期望偏移"】
 *   丢包、重复、超时重发都靠 `SEQ <next>` 自纠，PC 端**不需要自己记账**。
 *   这是让 PC 脚本变傻、从而不容易写错的关键。
 * ================================================================== */
#include "font_upload.h"
#include "font_store.h"
#include "font.h"       /* Font_Invalidate —— 见下面两处调用 */
#include "w25q_img.h"
#include "w25q.h"
#include "time.h"       /* GetTick_ms —— 只用来报"这次灌了多久" */
#include <stdio.h>      /* printf（fputc 重定向在 uart.c） */

/* 每行携带的字节数。
 * 行长 = "/font d " (8) + "0x" + 5 位 hex (7) + 空格 (1) + 224 = 240 < 255。
 * ★ 255 是 uart_rx 的硬上限，**改这个数之前先重算行长**。 */
#define FONTUP_CHUNK      112u

/* ---------- 命令层错误码（会出现在 `ERR <code>` 里，值是协议的一部分） ---------- */
#define FONTUP_ERR_NOSESSION  1u    /* 没有在跑的上传会话（缺 begin，或已经 end/abort） */
#define FONTUP_ERR_PARAM      2u    /* 数字解析失败 / len 超范围 */
#define FONTUP_ERR_NOSPI      3u    /* W25Q 不在线（容量读不出来） */
#define FONTUP_ERR_BUSY       4u    /* 已经有一笔在跑，先 /font abort */
#define FONTUP_ERR_ERASE      5u    /* 擦除失败 */
#define FONTUP_ERR_DATA       6u    /* hex 数据非法（非十六进制字符 / 奇数位 / 超过 112 字节） */
#define FONTUP_ERR_RANGE      7u    /* off + 本块长度 超出了 begin 报的总长 */
#define FONTUP_ERR_WRITE      8u    /* 写失败（页编程本身报错） */
#define FONTUP_ERR_INCOMPLETE 9u    /* end 时数据没收全 */
#define FONTUP_ERR_HEADER     10u   /* begin 带的镜像头不合法（**没擦任何东西**） */
#define FONTUP_ERR_WEL        11u   /* 编程没执行：数据不对且 WEL 仍为 1 */
#define FONTUP_ERR_VERIFY     12u   /* 写进去了但值不对（多半是没擦干净） */
#define FONTUP_ERR_STORE      13u   /* CRC 正确，但字库装载/索引校验失败 */

/* ---------- 会话状态 ---------- */
static uint8_t  s_active = 0u;
static uint32_t s_len    = 0u;   /* begin 报的总长 */
static uint32_t s_crc    = 0u;   /* begin 报的 CRC32 */
static uint32_t s_next   = 0u;   /* 下一个期望的偏移（严格顺序） */
static uint32_t s_t0     = 0u;   /* begin 的时刻，用来报总耗时 */
static uint8_t  s_hex[FONTUP_CHUNK];

/* ==================================================================
 * 小工具
 * ================================================================== */

/* 十六进制数字 → 值；不是十六进制返回 0xFF */
static uint8_t hexval(char c)
{
    if ((c >= '0') && (c <= '9')) { return (uint8_t)(c - '0'); }
    if ((c >= 'a') && (c <= 'f')) { return (uint8_t)(c - 'a' + 10); }
    if ((c >= 'A') && (c <= 'F')) { return (uint8_t)(c - 'A' + 10); }
    return 0xFFu;
}

static void skip_spaces(const char **pp)
{
    while (**pp == ' ') { (*pp)++; }
}

/* 解析一个数：带 `0x` 前缀按十六进制，否则按十进制。
 * 返回 1 = 至少吃到一位有效数字且没溢出；失败时**不前进** *pp。 */
static uint8_t parse_num(const char **pp, uint32_t *out)
{
    const char *p   = *pp;
    uint32_t    v   = 0u;
    uint8_t     any = 0u;
    uint8_t     ovf = 0u;

    if ((p[0] == '0') && ((p[1] == 'x') || (p[1] == 'X')))
    {
        p += 2;
        while (hexval(*p) != 0xFFu)
        {
            uint32_t nv = (v << 4) | (uint32_t)hexval(*p);
            if (v > 0x0FFFFFFFu) { ovf = 1u; }
            v   = nv;
            any = 1u;
            p++;
        }
    }
    else
    {
        while ((*p >= '0') && (*p <= '9'))
        {
            uint32_t nv = v * 10u + (uint32_t)(*p - '0');
            if (v > (0xFFFFFFFFu - (uint32_t)(*p - '0')) / 10u) { ovf = 1u; }
            v   = nv;
            any = 1u;
            p++;
        }
    }

    if ((any == 0u) || (ovf != 0u)) { return 0u; }

    *pp  = p;
    *out = v;
    return 1u;
}

/* 读完一个词，后面必须是结尾或空格（这样 "endpoint" 不算 "end"） */
static uint8_t word_is(const char *p, const char *w)
{
    while (*w != '\0')
    {
        if (*p != *w) { return 0u; }
        p++;
        w++;
    }
    return ((*p == '\0') || (*p == ' ')) ? 1u : 0u;
}

/* W25QImg 的错误码 → 命令层错误码（让串口上那个数字指向真正的原因） */
static uint8_t map_img_err(uint8_t rc)
{
    switch (rc)
    {
        case W25QIMG_ERR_WEL:    return FONTUP_ERR_WEL;
        case W25QIMG_ERR_VERIFY: return FONTUP_ERR_VERIFY;
        default:                 return FONTUP_ERR_WRITE;
    }
}

/* 偏移一律打成 5 位 hex（0x20000 以内够用），PC 端按同一格式回填 */
static void fu_reply_off(const char *kw, uint32_t off)
{
    printf("[FONT] %s %05X\r\n", kw, (unsigned)off);
}

/* ==================================================================
 * /font begin <len> <crc32> <hdr32>
 * ================================================================== */
static void fu_begin(const char *p)
{
    uint32_t len = 0u;
    uint32_t crc = 0u;
    uint8_t  hdr[FONTSTORE_HDR_BYTES];
    uint32_t n;
    uint32_t i;
    uint32_t bad = 0u;
    uint8_t  rc;

    if (s_active != 0u)
    {
        printf("[FONT] ERR %u (busy; send /font abort first)\r\n", (unsigned)FONTUP_ERR_BUSY);
        return;
    }

    /* ---- ① 解析三个参数 ---- */
    if (parse_num(&p, &len) == 0u) { printf("[FONT] ERR %u (len)\r\n", (unsigned)FONTUP_ERR_PARAM); return; }
    skip_spaces(&p);
    if (parse_num(&p, &crc) == 0u) { printf("[FONT] ERR %u (crc32)\r\n", (unsigned)FONTUP_ERR_PARAM); return; }
    skip_spaces(&p);

    for (i = 0u; i < FONTSTORE_HDR_BYTES; i++)
    {
        uint8_t hi = hexval(p[0]);
        uint8_t lo;
        if (hi == 0xFFu) { printf("[FONT] ERR %u (hdr hex)\r\n", (unsigned)FONTUP_ERR_PARAM); return; }
        p++;
        lo = hexval(p[0]);
        if (lo == 0xFFu) { printf("[FONT] ERR %u (hdr hex odd)\r\n", (unsigned)FONTUP_ERR_PARAM); return; }
        p++;
        hdr[i] = (uint8_t)((hi << 4) | lo);
    }
    if (*p != '\0') { printf("[FONT] ERR %u (hdr hex extra)\r\n", (unsigned)FONTUP_ERR_PARAM); return; }

    /* ---- ② 校镜像头 —— ★ 这一步在**擦任何东西之前**。
     *         坏文件绝不能先毁掉现有字库（`font_store.h` 的两阶段提交第 ① 步）。 */
    rc = FontStore_CheckHeader(hdr, len);
    if (rc != FONTSTORE_OK)
    {
        printf("[FONT] ERR %u (header rc=%u %s; nothing erased)\r\n",
               (unsigned)FONTUP_ERR_HEADER, (unsigned)rc, FontStore_ErrName(rc));
        return;
    }
    n = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);

    /* ---- ③ W25Q 在线？ ---- */
    if (W25Q_CapacityBytes() == 0u)
    {
        printf("[FONT] ERR %u (W25Q not online; run the power-on selftest first)\r\n",
               (unsigned)FONTUP_ERR_NOSPI);
        return;
    }

    /* ---- ④ 先擦旧标记 —— ★ 两阶段提交的"第一刀"。
     *         从这一刻起，哪怕立刻断电，下次上电也只会干净地退回片内 32 字，
     *         绝不会拿着一份**半新半旧**的镜像当字库用。 ---- */
    printf("[FONT] erasing marker, then %u bytes of image (up to ~4s, no command is served meanwhile)\r\n",
           (unsigned)len);

    rc = FontStore_MarkErase();
    if (rc != FONTSTORE_OK)
    {
        printf("[FONT] ERR %u (marker erase)\r\n", (unsigned)FONTUP_ERR_ERASE);
        return;
    }

    /* ---- ⑥ ★ 就地清掉 RAM 里那份"旧字库可用"的状态 ----
     *   标记刚被擦掉 ⇒ Init 必然回 NO_MARK。不调这一下的后果见 font_upload.h。 */
    rc = FontStore_Init();

    /* ---- ⑦ 顺手把 font.c 的单字缓存也清掉 ----
     *   `Init()` 清的是**字库层**的状态，`Font_Invalidate()` 清的是**查表层**的。
     *   ★ `FontStore_Gen()` 已经能兜住这件事（`Init()` 会 +1 ⇒ 缓存自动不命中），
     *     这一句是**显式说明意图**用的，不是唯一防线；两种写法并存不冲突。 */
    Font_Invalidate();

    /* ---- ⑤ 擦镜像区 ---- */
    if (W25QImg_EraseRange(FONTSTORE_BASE, len, &bad) != W25QIMG_OK)
    {
        printf("[FONT] ERR %u at 0x%08X\r\n", (unsigned)FONTUP_ERR_ERASE, (unsigned)bad);
        return;
    }

    s_active = 1u;
    s_len    = len;
    s_crc    = crc;
    s_next   = 0u;
    s_t0     = GetTick_ms();

    printf("[FONT] old store invalidated (rc=%u %s); expect %u glyphs\r\n",
           (unsigned)rc, FontStore_ErrName(rc), (unsigned)n);
    printf("[FONT] OK\r\n");
}

/* ==================================================================
 * /font d <off> <hex>
 * ================================================================== */
static void fu_data(const char *p)
{
    uint32_t off = 0u;
    uint32_t n   = 0u;
    uint32_t bad = 0u;
    uint8_t  rc;

    if (s_active == 0u)
    {
        printf("[FONT] ERR %u (no session)\r\n", (unsigned)FONTUP_ERR_NOSESSION);
        return;
    }

    if (parse_num(&p, &off) == 0u) { printf("[FONT] ERR %u (off)\r\n", (unsigned)FONTUP_ERR_PARAM); return; }
    skip_spaces(&p);

    /* ---- hex → 字节。**严格**：只认十六进制字符，见到别的就整块拒掉。
     *      被改坏的行宁可重发，也不能把垃圾写进 Flash。 ---- */
    while (*p != '\0')
    {
        uint8_t hi = hexval(*p);
        uint8_t lo;
        if (hi == 0xFFu) { printf("[FONT] ERR %u (bad hex)\r\n", (unsigned)FONTUP_ERR_DATA); return; }
        p++;
        lo = hexval(*p);
        if (lo == 0xFFu) { printf("[FONT] ERR %u (odd hex)\r\n", (unsigned)FONTUP_ERR_DATA); return; }
        p++;
        if (n >= FONTUP_CHUNK) { printf("[FONT] ERR %u (chunk too long)\r\n", (unsigned)FONTUP_ERR_DATA); return; }
        s_hex[n] = (uint8_t)((hi << 4) | lo);
        n++;
    }

    if (n == 0u) { printf("[FONT] ERR %u (empty)\r\n", (unsigned)FONTUP_ERR_DATA); return; }

    /* ---- 严格顺序：不是"下一个期望的偏移"就一律回 SEQ，**不写任何东西**。
     *      重复（off < next）和跳号（off > next）都走这一条 ——
     *      PC 端只要永远从回复里的 next 续传，就能自纠。 ---- */
    if (off != s_next)
    {
        fu_reply_off("SEQ", s_next);
        return;
    }

    if ((off + n) > s_len)
    {
        printf("[FONT] ERR %u %05X\r\n", (unsigned)FONTUP_ERR_RANGE, (unsigned)off);
        return;
    }

    rc = W25QImg_WriteChunk(FONTSTORE_BASE + off, s_hex, n, &bad);
    if (rc != W25QIMG_OK)
    {
        printf("[FONT] ERR %u %05X\r\n",
               (unsigned)map_img_err(rc), (unsigned)(bad - FONTSTORE_BASE));
        return;
    }

    s_next += n;
    fu_reply_off("OK", s_next);
}

/* ==================================================================
 * /font end —— 回读校验 → 写完成标记（两阶段提交的"第二刀"）
 * ================================================================== */
static void fu_end(void)
{
    uint32_t got = 0u;
    uint8_t  rc;

    if (s_active == 0u)
    {
        printf("[FONT] ERR %u (no session)\r\n", (unsigned)FONTUP_ERR_NOSESSION);
        return;
    }

    if (s_next != s_len)
    {
        printf("[FONT] ERR %u %05X (need %05X)\r\n",
               (unsigned)FONTUP_ERR_INCOMPLETE, (unsigned)s_next, (unsigned)s_len);
        return;
    }

    /* ★ 阻塞窗口 ②：回读 127686 字节约 0.13s + 算 CRC 约 30ms */
    printf("[FONT] verifying %u bytes by read-back CRC32 (~0.2s)...\r\n", (unsigned)s_len);

    if (W25QImg_Crc32Range(FONTSTORE_BASE, s_len, &got) != W25QIMG_OK)
    {
        printf("[FONT] ERR %u (read-back)\r\n", (unsigned)FONTUP_ERR_WRITE);
        return;
    }

    if (got != s_crc)
    {
        /* ★ 校验不过 ⇒ **绝不写标记**。镜像虽然在 Flash 里，但没有标记
         *   ⇒ 上电一定会退回片内 32 字，屏上不会出现半新半旧的字。 */
        s_active = 0u;
        printf("[FONT] CRC bad %08X (expected %08X)\r\n", (unsigned)got, (unsigned)s_crc);
        return;
    }

    rc = FontStore_MarkWrite(s_len, got);
    if (rc != FONTSTORE_OK)
    {
        printf("[FONT] ERR %u (marker write)\r\n", (unsigned)FONTUP_ERR_WRITE);
        return;
    }

    s_active = 0u;

    /* 装完立刻可用，**不用复位** */
    rc = FontStore_Init();

    /* 换了一整套字模 ⇒ 单字缓存必须失效，否则第一次画的那个字还是旧字库的字。 */
    Font_Invalidate();

    if (rc != FONTSTORE_OK)
    {
        printf("[FONT] ERR %u (store rc=%u %s)\r\n",
               (unsigned)FONTUP_ERR_STORE, (unsigned)rc, FontStore_ErrName(rc));
        return;
    }

    printf("[FONT] done: %u bytes in %u ms, CRC32=0x%08X, %u glyphs, store rc=%u %s\r\n",
           (unsigned)s_len, (unsigned)(GetTick_ms() - s_t0), (unsigned)got,
           (unsigned)FontStore_Count(), (unsigned)rc, FontStore_ErrName(rc));
    printf("[FONT] CRC ok\r\n");
}

/* ==================================================================
 * /font abort —— 清会话，**不动 flash**
 * ------------------------------------------------------------------
 * ⚠️ 注意它**不会**把旧字库救回来：`begin` 那一刻标记就已经擦掉了。
 *    这是有意的 —— "反悔"的正规做法是重新灌一遍，不是 abort。
 * ================================================================== */
static void fu_abort(void)
{
    s_active = 0u;
    s_next   = 0u;
    printf("[FONT] OK (session cleared; flash untouched)\r\n");
}

/* ==================================================================
 * /font verify —— 只回读算 CRC32，不写
 * ================================================================== */
static void fu_verify(void)
{
    uint32_t crc = 0u;
    uint8_t  rc;

    if (FontStore_Ready() == 0u)
    {
        printf("[FONT] ERR %u (no installed store)\r\n", (unsigned)FONTUP_ERR_NOSESSION);
        return;
    }

    printf("[FONT] read-back CRC32 over %u bytes...\r\n",
           (unsigned)FontStore_ImageBytes());

    rc = FontStore_Verify(&crc);

    if (rc == FONTSTORE_OK)          { printf("[FONT] CRC %08X\r\n", (unsigned)crc); }
    else if (rc == FONTSTORE_ERR_CRC){ printf("[FONT] CRC %08X MISMATCH\r\n", (unsigned)crc); }
    else                             { printf("[FONT] ERR %u (%s)\r\n",
                                              (unsigned)FONTUP_ERR_WRITE, FontStore_ErrName(rc)); }
}

/* ==================================================================
 * /font —— 状态一行
 * ================================================================== */
static void fu_status(void)
{
    uint32_t crc = 0u;
    uint8_t  rc  = FontStore_Verify(&crc);   /* 只读；顺便把"镜像现在还对不对"答了 */

    printf("[FONT] store ready=%u count=%u len=%u crc=0x%08X verify=%s | upload active=%u next=%05X\r\n",
           (unsigned)FontStore_Ready(),
           (unsigned)FontStore_Count(),
           (unsigned)FontStore_ImageBytes(),
           (unsigned)crc,
           (FontStore_Ready() != 0u) ? FontStore_ErrName(rc) : "n/a",
           (unsigned)s_active,
           (unsigned)s_next);
}

static void fu_usage(void)
{
    printf("[FONT] usage (all numbers: len decimal, others 0x-hex):\r\n");
    printf("      /font begin <len> <crc32> <hdr32>   start; hdr32 = 16-byte image header as hex\r\n");
    printf("      /font d <off> <hex>                 write one %u-byte chunk\r\n",
           (unsigned)FONTUP_CHUNK);
    printf("      /font end                           verify CRC32, then commit the marker\r\n");
    printf("      /font abort                         drop the session (flash untouched)\r\n");
    printf("      /font verify                        read back and recompute CRC32\r\n");
    printf("      /font                               one-line status\r\n");
    printf("      /font ?                             this text\r\n");
    printf("      PC side: Tools\\font_xfer\\send_font.bat\r\n");
}

/* ==================================================================
 * 命令入口。返回 1 = 是我的命令（已处理）；0 = 不是，交回调用者
 * ================================================================== */
uint8_t FontUpload_Command(const char *line)
{
    const char *p;

    /* 必须正好是 "/font"，"/fontx" 不算 —— 后面必须是结尾或空格 */
    if ((line[0] != '/') || (line[1] != 'f') || (line[2] != 'o') ||
        (line[3] != 'n') || (line[4] != 't'))                       { return 0u; }
    if ((line[5] != '\0') && (line[5] != ' '))                       { return 0u; }

    p = &line[5];
    skip_spaces(&p);

    /* ★ `word_is()` 只判断、不前进 —— 所以匹配上之后要自己把指针挪过这个词。
     *   顺序也不能变：`d` 必须排在 `begin` 之后无妨，但 `end` 一定不能排在
     *   任何以它开头的词前面（现在没有这种词，将来加 `endless` 之类就会踩）。 */
    if (*p == '\0')             { fu_status(); return 1u; }
    if (word_is(p, "begin"))    { p += 5; skip_spaces(&p); fu_begin(p); return 1u; }
    if (word_is(p, "d"))        { p += 1; skip_spaces(&p); fu_data(p);  return 1u; }
    if (word_is(p, "end"))      { fu_end();    return 1u; }
    if (word_is(p, "abort"))    { fu_abort();  return 1u; }
    if (word_is(p, "verify"))   { fu_verify(); return 1u; }
    if (word_is(p, "?"))        { fu_usage();  return 1u; }

    printf("[FONT] ERR %u (unknown sub-command; /font ? for usage)\r\n",
           (unsigned)FONTUP_ERR_PARAM);
    return 1u;
}

uint8_t FontUpload_Active(void)
{
    return s_active;
}
