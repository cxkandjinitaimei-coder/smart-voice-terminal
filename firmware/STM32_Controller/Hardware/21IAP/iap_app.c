/**
 * @file iap_app.c
 * 用途：应用侧 OTA 请求确认、下载槽写入、CRC 校验和提交状态机。
 * 使用：Init 后命令/帧入口接受请求，实体按键确认后允许 BEGIN；Tick 执行延后擦除及状态推进。
 * 时序与边界：只写 Slot B，完整 CRC 正确才写 READY 元数据；Bootloader 重启后搬到 A。协议回执需发 ESP 而非只 printf。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "iap_app.h"
#include "iap_flash.h"
#include "iap_meta.h"
#include "iap_image.h"
#include "time.h"

#define IAP_TX_BACKEND_PRINTF 0u
#define IAP_TX_BACKEND_ESP 1u
#define IAP_TX_BACKEND IAP_TX_BACKEND_ESP

#if (IAP_TX_BACKEND == IAP_TX_BACKEND_ESP)
#include "esp_uart.h"
#endif

/* ==========================================================================
 *  状态
 * ========================================================================== */
static uint8_t s_state;
static uint32_t s_total;      /* 整包字节数 */
static uint32_t s_crc_expect; /* 整包 CRC32（zlib 口径） */
static uint32_t s_got;        /* 已收字节 */
static uint16_t s_seq_expect; /* 下一块应有的序号（块内计数，0 起） */
static uint32_t s_erase_at;   /* 计划执行擦除的时刻 */
static uint32_t s_done_at;    /* 进 DONE 的时刻 */
static uint32_t s_last_err;
static uint32_t s_activity;
static uint8_t s_request_waiting; /* Remote request is harmless until K-UP confirms it. */
static uint32_t s_request_total;
static uint32_t s_request_crc;
static uint32_t s_request_at;

/* ==========================================================================
 *  小工具
 * ========================================================================== */
static uint8_t starts_with(const char *s, const char *p)
{
    return (uint8_t)(strncmp(s, p, strlen(p)) == 0);
}

static uint32_t rd32le(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ---- `@IAP:STATE` 里的 slot 字段：1 = Slot B（对外语义，不是扇区号）---- */
#define IAP_SLOT_ID_A 0u
#define IAP_SLOT_ID_B 1u

/*  "我到底被链接到哪个地址" —— 链接器符号，**从链接地址自己算出来，不在工程里再配一份**。
 *   `ER_IROM1` 是"工程没写 ScatterFile"时自动生成的区域名（App 自动生成；Boot 使用固定描述符 scatter）。
 *   同一个符号 `Image$$ER_IROM1$$Base` 已在 `User/system_stm32f4xx.c` 里用过、编译过
 *   ⇒ 这里复用，零新风险。 用它的好处：它**不会跟 `<IROM>` 配置漂移**。 */
extern uint32_t Image$$ER_IROM1$$Base;

uint32_t IAP_AppBase(void)
{
#ifdef IAP_TEST_LINK_BASE
    return IAP_TEST_LINK_BASE;
#else
    return (uint32_t)&Image$$ER_IROM1$$Base;
#endif
}

const char *IAP_StateName(void)
{
    switch (s_state)
    {
    case IAP_S_IDLE:
        return "IDLE";
    case IAP_S_ERASE:
        return "ERASE";
    case IAP_S_DATA:
        return "DATA";
    case IAP_S_VERIFY:
        return "VERIFY";
    case IAP_S_COMMIT:
        return "COMMIT";
    case IAP_S_DONE:
        return "DONE";
    default:
        return "?";
    }
}

uint32_t IAP_Got(void)
{
    return s_got;
}
uint8_t IAP_IsBusy(void)
{
    return (uint8_t)(s_state != IAP_S_IDLE);
}
uint8_t IAP_RequestWaiting(void)
{
    return s_request_waiting;
}

/*  实物实证：**协议帧必须也发到 ESP 那条线**（USART6）。
 *   原先 `@IAP:WAIT` / `@IAP:OK` 只用 printf 发 —— 而 printf 走的是 USART2(PA2/PA3)
 *   调试口（`uart.c` 的 fputc），ESP 一个字都收不到 ⇒ ESP 等满 1800 ms 超时，
 *   回 "F407 did not accept request"（而 F407 其实已经答应了，见 问题日志_AI速查 §二）。
 *    做法：格式串写进 `s_text`，调试口给人看、ESP 线给协议用，**两边都发**。
 *    static 而非局部：本项目栈只有 2 KB（日志里有实测）；定义在第一个用它的
 *     `IAP_ConfirmRequest` **之前**，免得 C90 隐式声明报警告。 */
static char s_text[48];
static void iap_send_text(void)
{
    printf("%s", s_text);
    ESP_UART_SendStr(s_text);
}

uint8_t IAP_ConfirmRequest(void)
{
    if (!s_request_waiting)
        return 0u;
    s_request_waiting = 0u;
    sprintf(s_text, "@IAP:OK,%u,%u#\r\n", (unsigned)s_request_total, (unsigned)s_request_crc);
    iap_send_text();
    printf("[IAP] user approved OTA request; waiting for image upload\r\n");
    return 1u;
}

/* ==========================================================================
 *  发送（**唯一出口**）
 * ========================================================================== */
static void iap_tx(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len)
{
#if (IAP_TX_BACKEND == IAP_TX_BACKEND_ESP)
    ESP_UART_SendFrame(type, seq, payload, len);
#else
    /* 摘要式回话：只打 type/seq/len 和头 4 个字节，避免淹没调试口 */
    printf("[IAP>] t=%02X seq=%u len=%u", (unsigned)type, (unsigned)seq, (unsigned)len);
    if ((payload != 0) && (len >= 4u))
    {
        printf(" p0=%02X%02X%02X%02X", payload[0], payload[1], payload[2], payload[3]);
    }
    printf("\r\n");
#endif
}

/* `@IAP:STATE,<stage>,<slot>,<ver>,<got>,<total>#`（`docs/flash-and-ota.md`） */
static void iap_send_state(void)
{
    uint8_t state[25];
    state[0] = s_state;
    wr32le(state + 1, s_got);
    wr32le(state + 5, s_total);
    wr32le(state + 9, IAP_AppBase());
    wr32le(state + 13, IAP_Crc32Flash(IAP_SLOT_A_BASE, IAP_FW_SIZE_MAX));
    wr32le(state + 17, IAP_FW_SIZE_MAX);
    wr32le(state + 21, IAP_BootLayoutValid() ? IAP_LAYOUT_ID : 0u);
    iap_tx(IAP_T_STATE, 0u, state, sizeof(state));
    printf("@IAP:STATE,%s,%u,%u,%u,%u#\r\n", IAP_StateName(), (unsigned)IAP_SLOT_ID_B,
           0u /* ver：现在还没有版本号字段，留 0 */, (unsigned)s_got, (unsigned)s_total);
}

static void iap_fail(uint32_t code)
{
    uint8_t error = (uint8_t)code;
    /* Keep the failure self describing on the F407 debug port.  The protocol
     * NAK only carries the numeric code, which used to leave a DATA-stage
     * failure indistinguishable from a bad request on the bench. */
    printf("[IAP] FAIL code=%u state=%s got=%u/%u seq_expect=%u\r\n", (unsigned)code,
           IAP_StateName(), (unsigned)s_got, (unsigned)s_total, (unsigned)s_seq_expect);
    iap_tx(IAP_T_NAK, s_seq_expect, &error, 1u);
    s_last_err = code;
    sprintf(s_text, "@IAP:ERR,%u#\r\n", (unsigned)code);
    iap_send_text();
    s_state = IAP_S_IDLE;
}

/* ==========================================================================
 *  各阶段
 * ========================================================================== */

/*  硬上限检查 —— `iap_flash.h` 说的「升级成功但变砖」就是漏了这一步：
 *    Slot B(256 KB) 比 Slot A(224 KB) 大，B→A 是逐字节拷贝不是截断
 *    ⇒ 110 KB 的固件能全绿下进 B，bootloader 往 A 拷到一半溢出。
 *    ⇒ **必须在 BEGIN 阶段就拒**，不能等收完再发现（那时 Slot B 已经满了）。 */
static uint8_t check_size(uint32_t total)
{
    if (total < 8u)
    {
        return IAP_EC_ARG;
    }
    if (total > IAP_FW_SIZE_MAX)
    {
        return IAP_EC_SIZE;
    }
    if (total > IAP_SLOT_B_SIZE)
    {
        return IAP_EC_SIZE;
    }
    return 0u;
}

static void do_begin(uint32_t total, uint32_t crc)
{
    uint32_t ec;
    uint8_t meta[IAP_META_PAYLOAD_LEN];
    iap_meta_t previous;

    if (s_state != IAP_S_IDLE)
    {
        iap_fail(IAP_EC_STATE);
        return;
    }
    if (IAP_AppBase() != IAP_SLOT_A_BASE || !IAP_BootLayoutValid())
    {
        iap_fail(IAP_EC_STATE);
        return;
    }
    if (IAP_MetaRead(&previous) && previous.state == IAP_ST_READY)
    {
        iap_fail(IAP_EC_STATE);
        return;
    } /* never overwrite a committed recovery image */

    ec = check_size(total);
    if (ec != 0u)
    {
        iap_fail(ec);
        return;
    }
    s_activity = GetTick_ms();

    s_total = total;
    s_crc_expect = crc;
    s_got = 0u;
    s_seq_expect = 0u;
    s_erase_at = 0u; /* 由 tick 填 */

    /* 先把"我要开始擦了"发出去 —— 擦除那 1~4 秒 CPU 停，什么都发不出去 */
    wr32le(meta, total);
    wr32le(meta + 4u, crc);
    meta[8] = (uint8_t)IAP_SLOT_B_FIRST_SECTOR;
    meta[9] = 0u;
    meta[10] = 0u;
    meta[11] = 0u;
    iap_tx(IAP_T_META, 0u, meta, IAP_META_PAYLOAD_LEN);

    printf("[IAP] BEGIN total=%u crc=%08X -> erase Slot B in %u ms\r\n", (unsigned)total,
           (unsigned)crc, (unsigned)IAP_ERASE_DELAY_MS);
    s_state = IAP_S_ERASE;
}

static void do_erase_now(void)
{
    uint32_t i;

    printf("[IAP] erasing Slot B (S%u, %u KB) ...\r\n", (unsigned)IAP_SLOT_B_FIRST_SECTOR,
           (unsigned)(IAP_SLOT_B_SIZE / 1024u));
    for (i = 0u; i < IAP_SLOT_B_SECTORS; i++)
    {
        if (IAP_EraseSector(IAP_SLOT_B_FIRST_SECTOR + i) != IAP_OK)
        {
            iap_fail(IAP_EC_FLASH);
            return;
        }
    }
    s_got = 0u;
    s_seq_expect = 0u;
    s_state = IAP_S_DATA;
    printf("[IAP] erased, ready for data\r\n");
    iap_send_state();
}

/* 收一块：写进 Slot B 的 s_got 偏移处。写失败/越界一律 ERR，不留半截状态。 */
static void do_chunk(uint16_t seq, const uint8_t *payload, uint16_t len)
{
    if (seq + 1u == s_seq_expect && s_got >= len && len > 0u && len <= IAP_CHUNK_MAX &&
        (s_state == IAP_S_DATA || s_state == IAP_S_VERIFY) &&
        IAP_MemEqualFlash(IAP_SLOT_B_BASE + s_got - len, payload, len))
    {
        s_activity = GetTick_ms();
        iap_tx(IAP_T_ACK, seq, 0, 0u);
        return;
    }
    if (s_state != IAP_S_DATA)
    {
        iap_tx(IAP_T_NAK, seq, 0, 0u);
        return;
    }

    if (seq != s_seq_expect) /* 错序 → NAK，让对端重发 */
    {
        printf("[IAP] seq mismatch got=%u want=%u -> NAK\r\n", (unsigned)seq,
               (unsigned)s_seq_expect);
        iap_tx(IAP_T_NAK, s_seq_expect, 0, 0u);
        return;
    }
    if (!len || len > IAP_CHUNK_MAX || (len != IAP_CHUNK_MAX && s_got + len != s_total))
    {
        iap_fail(IAP_EC_ARG);
        return;
    }
    if ((s_got + (uint32_t)len) > s_total)
    {
        iap_fail(IAP_EC_SIZE);
        return;
    }
    /*  F4 只能按"字"编程、且 `IAP_ProgramWords` 要求地址 4 字节对齐
     *   ⇒ 每块的写入偏移必须 4 对齐。块大小恒为 2 的幂时自然成立，
     *     但"对端换了块大小"或"续传偏移随意"就会破 —— 宁可在这里拒，也别写歪。 */
    if ((s_got % 4u) != 0u)
    {
        iap_fail(IAP_EC_ARG);
        return;
    }

    if (IAP_ProgramWords(IAP_SLOT_B_BASE + s_got, payload, (uint32_t)len) != IAP_OK)
    {
        iap_fail(IAP_EC_FLASH);
        return;
    }

    s_got += (uint32_t)len;
    s_activity = GetTick_ms();
    s_seq_expect = (uint16_t)(s_seq_expect + 1u);
    iap_tx(IAP_T_ACK, seq, 0, 0u);

    if (s_got == s_total)
    {
        s_state = IAP_S_VERIFY;
    }
}

/* 整包 CRC 比对：直接对 Flash 算，不必把整个镜像读进内存 */
static void do_verify(void)
{
    uint32_t crc;

    if ((s_state != IAP_S_VERIFY) && (s_state != IAP_S_DATA))
    {
        iap_fail(IAP_EC_STATE);
        return;
    }
    if (s_got != s_total ||
        !IAP_ImageVectorsOK(*(volatile uint32_t *)IAP_SLOT_B_BASE,
                            *(volatile uint32_t *)(IAP_SLOT_B_BASE + 4u), s_total))
    {
        iap_fail(IAP_EC_ARG);
        return;
    }

    printf("[IAP] verifying %u bytes ...\r\n", (unsigned)s_total);
    crc = IAP_Crc32Flash(IAP_SLOT_B_BASE, s_total);
    if (crc != s_crc_expect)
    {
        printf("[IAP] crc mismatch got=%08X want=%08X\r\n", (unsigned)crc, (unsigned)s_crc_expect);
        iap_tx(IAP_T_NAK, 0u, 0, 0u);
        iap_fail(IAP_EC_CRC);
        return;
    }
    s_state = IAP_S_COMMIT;
    s_activity = GetTick_ms();
    printf("[IAP] crc ok\r\n");
    iap_send_state();
}

/* 写元数据 = 对 bootloader 的承诺「下次复位请把 B 搬到 A」。**这里不跳转、不擦 Slot A。** */
static void do_commit(void)
{
    iap_meta_t m;

    if (s_state != IAP_S_COMMIT)
    {
        iap_fail(IAP_EC_STATE);
        return;
    }

    IAP_MetaFillEmpty(&m);
    m.state = IAP_ST_READY;
    m.fw_size = s_total;
    m.fw_crc = s_crc_expect;
    /*  a_crc = 搬运**前** Slot A 的 CRC（回滚判据）。
     *   填 0 = "未知"，bootloader 侧应当把 0 当作"没有回滚基线"。
     *    想填真值，需要 App 自己的镜像范围（`Image$$ER_IROM1$$Base` .. `$$Limit`）。
     *      `$$Base` 已在 `User/system_stm32f4xx.c` 用过（编译过），**`$$Limit` 还没试过**，
     *      所以这里**不冒险引用** —— 等第一次上板确认能链接了再填（见 iap_app.h 的 TODO）。 */
    m.a_crc = 0u;

    if (IAP_MetaWrite(&m) != 1u)
    {
        iap_fail(IAP_EC_FLASH);
        return;
    }

    s_done_at = 0u; /* 由 tick 填 */
    s_state = IAP_S_DONE;
    printf("[IAP] committed: META state=READY size=%u crc=%08X\r\n", (unsigned)s_total,
           (unsigned)s_crc_expect);
    printf("[IAP] reboot (or @IAP:JUMP#) -> bootloader copies B to A; Slot A untouched\r\n");
    iap_send_state();
}

/* 下载中止只撤销本轮状态；未提交 READY 的 Slot B 不会被 Bootloader 安装，Slot A 不动。 */
static void do_abort(void)
{
    s_request_waiting = 0u;
    s_request_total = 0u;
    s_request_crc = 0u;
    if (s_state == IAP_S_DONE)
    {
        iap_send_state();
        return;
    }
    printf("[IAP] ABORT at %s (got=%u/%u) -- Slot A untouched\r\n", IAP_StateName(),
           (unsigned)s_got, (unsigned)s_total);
    s_state = IAP_S_IDLE;
    /*  这里是**人工中止**，不是出错 ⇒ 只回 STATE，**不回 `@IAP:ERR`**
     *   （回 ERR 会让 PC 侧把它当故障报，而"用户主动取消"跟"升级失败"是两回事）。
     *   另外：**不回滚元数据** —— Slot B 里那半截永远不会被 bootloader 搬，
     *   因为只有 META=READY 才搬，而我们还没 COMMIT。 */
    iap_send_state();
}

static void do_jump(void)
{
    printf("[IAP] reset now -> bootloader will copy B to A\r\n");
    NVIC_SystemReset();
}

/* ==========================================================================
 *  文本控制帧 `@IAP:`（`docs/flash-and-ota.md`）
 * ========================================================================== */
uint8_t IAP_AppCommand(const char *line)
{
    uint32_t a, b;
    char *end;
    const char *p; /* C90：声明必须在块首，所以挪上来 */

    if (!starts_with(line, "@IAP:"))
    {
        return 0u;
    }

    if (starts_with(line, "@IAP:INFO"))
    {
        iap_send_state();
        printf("[IAP] stage=%s got=%u total=%u crc=%08X last_err=%u\r\n", IAP_StateName(),
               (unsigned)s_got, (unsigned)s_total, (unsigned)s_crc_expect, (unsigned)s_last_err);
        return 1u;
    }
    if (starts_with(line, "@IAP:ABORT"))
    {
        do_abort();
        return 1u;
    }
    if (starts_with(line, "@IAP:JUMP"))
    {
        if (s_state == IAP_S_DONE)
            do_jump();
        else
            iap_fail(IAP_EC_STATE);
        return 1u;
    }

    /* 人工敲 `@IAP:COMMIT#`：先验后提交。 验失败时状态已回 IDLE，
     * 所以必须判一下再提 —— 否则会紧接着再报一个"状态不对"的假错误。 */
    if (starts_with(line, "@IAP:COMMIT"))
    {
        do_verify();
        if (s_state == IAP_S_COMMIT)
        {
            do_commit();
        }
        return 1u;
    }

    if (starts_with(line, "@IAP:RESUME,"))
    {
        iap_fail(IAP_EC_ARG);
        return 1u;
    }

    /*  调试扩展（**不在 §5.3 那张表里**）：单板不接 ESP 也能把状态机走通 ——
     *   BEGIN + 尺寸/CRC 校验 + 擦 Slot B + ABORT 全程可测。
     *    我**故意不做** `@IAP:DATA:` 文本帧：那正是 §5.1 否掉的 hex 方案（带宽 2×），
     *     数据通路走二进制 0x10，归 01 的传输。这里只补控制面。 */
    if (starts_with(line, "@IAP:BEGIN,"))
    {
        p = strchr(line, ',');
        a = (uint32_t)strtoul(p + 1, &end, 0);
        if (*end != ',')
        {
            iap_fail(IAP_EC_ARG);
            return 1u;
        }
        b = (uint32_t)strtoul(end + 1, &end, 0);
        printf("[IAP] (debug ext) BEGIN %u %08X\r\n", (unsigned)a, (unsigned)b);
        do_begin(a, b);
        return 1u;
    }

    /* The network side may only ask. It cannot start an erase: a physical
     * K-UP press turns this request into a one-shot approval for the exact
     * total/CRC pair. */
    if (starts_with(line, "@IAP:REQUEST,"))
    {
        p = strchr(line, ',');
        a = (uint32_t)strtoul(p + 1, &end, 0);
        if (*end != ',')
        {
            iap_fail(IAP_EC_ARG);
            return 1u;
        }
        b = (uint32_t)strtoul(end + 1, &end, 0);
        if (s_state != IAP_S_IDLE || IAP_AppBase() != IAP_SLOT_A_BASE || !IAP_BootLayoutValid() ||
            check_size(a) != 0u)
        {
            iap_fail(IAP_EC_STATE);
            return 1u;
        }
        s_request_total = a;
        s_request_crc = b;
        s_request_at = GetTick_ms();
        s_request_waiting = 1u;
        sprintf(s_text, "@IAP:WAIT,%u,%u#\r\n", (unsigned)a, (unsigned)b);
        iap_send_text();
        printf("[IAP] OTA requested: press K-UP to approve\r\n");
        return 1u;
    }

    /* 认不出的 `@IAP:xxx` 也要让 ESP 那条线知道（别只打在调试口） */
    sprintf(s_text, "@IAP:ERR,%u#\r\n", (unsigned)IAP_EC_ARG);
    iap_send_text();
    return 1u;
}

/* ==========================================================================
 *  二进制帧入口（等 01 的接收原语来调）
 * ========================================================================== */
void IAP_OnFrame(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    /* META and CTRL are rare; print them (and only the first few DATA blocks)
     * so a legal but unexpected frame can be identified without slowing the
     * 921600-baud data stream. */
    if (type != IAP_T_DATA || s_got < 384u || seq != s_seq_expect)
    {
        printf("[IAP] RX type=%02X seq=%u len=%u state=%s got=%u/%u\r\n", (unsigned)type,
               (unsigned)seq, (unsigned)len, IAP_StateName(), (unsigned)s_got, (unsigned)s_total);
    }
    if (type == IAP_T_META)
    {
        if (len != IAP_META_PAYLOAD_LEN || payload[8] != IAP_SLOT_ID_B)
        {
            iap_fail(IAP_EC_ARG);
            return;
        }
        /* A multipart HTTP bridge can replay META after a short write or a
         * lost response.  Once the same image is already in DATA/VERIFY,
         * replaying it is harmless; report the current state instead of
         * turning a recoverable duplicate into ERR_STATE. */
        if ((s_state == IAP_S_ERASE || s_state == IAP_S_DATA || s_state == IAP_S_VERIFY) &&
            rd32le(payload) == s_total && rd32le(payload + 4u) == s_crc_expect)
        {
            printf("[IAP] duplicate META accepted for active image\r\n");
            iap_send_state();
            return;
        }
        if (s_request_waiting || rd32le(payload) != s_request_total ||
            rd32le(payload + 4u) != s_request_crc)
        {
            iap_fail(IAP_EC_STATE);
            return;
        }
        /* `IAP_ConfirmRequest()` clears waiting; a zero pair is impossible
         * because check_size rejects it, so consume the approval exactly once. */
        if (s_request_total == 0u)
        {
            iap_fail(IAP_EC_STATE);
            return;
        }
        s_request_total = 0u;
        s_request_crc = 0u;
        do_begin(rd32le(payload), rd32le(payload + 4u));
        return;
    }
    if (type == IAP_T_DATA)
    {
        do_chunk(seq, payload, len);
        return;
    }
    if (type == IAP_T_CTRL)
    {
        if (len < 1u)
        {
            iap_fail(IAP_EC_ARG);
            return;
        }
        switch (payload[0])
        {
        case IAP_C_BEGIN:
            if (len >= 9u)
            {
                do_begin(rd32le(payload + 1u), rd32le(payload + 5u));
            }
            break;
        case IAP_C_END:
            do_verify();
            break;
        case IAP_C_VERIFY:
            do_verify();
            break;
        case IAP_C_COMMIT:
            do_commit();
            break;
        case IAP_C_ABORT:
            do_abort();
            break;
        case IAP_C_RESUME:
            iap_fail(IAP_EC_ARG);
            break;
        default:
            iap_fail(IAP_EC_ARG);
            break;
        }
        return;
    }
    /* 不是我的 type（0x00~0x0F 是 音频类型段）⇒ 静默忽略，别抢 */
}

/* ==========================================================================
 *  初始化 / 心跳
 * ========================================================================== */
void IAP_AppInit(void)
{
    iap_meta_t m;

    s_state = IAP_S_IDLE;
    s_total = 0u;
    s_crc_expect = 0u;
    s_got = 0u;
    s_seq_expect = 0u;
    s_last_err = 0u;
    s_request_waiting = 0u;
    s_request_total = 0u;
    s_request_crc = 0u;
    s_request_at = 0u;

    printf("[IAP] app-side state machine ready; fw cap = %u KB\r\n",
           (unsigned)(IAP_FW_SIZE_MAX / 1024u));

    printf("[IAP] layout=%08X boot_match=%u flash=%uKB A=%uKB B=%uKB B_base=%08X\r\n",
           (unsigned)IAP_LAYOUT_ID, (unsigned)IAP_BootLayoutValid(),
           (unsigned)IAP_FlashCapacityKB(), (unsigned)(IAP_SLOT_A_SIZE / 1024u),
           (unsigned)(IAP_SLOT_B_SIZE / 1024u), (unsigned)IAP_SLOT_B_BASE);

    /*  自证「我到底跑在哪个地址」—— 这正是"迁基址后只烧 F407 会静默跑旧 App"那个坑的解药：
     *   把链接地址读出来跟 Slot A 期望值比一句，跑错位置时**一眼可见**，不必再靠行为反推。 */
    /*  三种情形分开报，别只报"错"—— 因为**迁移前**打到 0x08000000 是正常的，
     *   把它也报成 WARN 会让人误以为出事了（那才是最坏的一种噪音）。 */
    if (IAP_AppBase() == IAP_SLOT_A_BASE)
    {
        printf("[IAP] linked base = Slot A (0x%08X)  OK\r\n", (unsigned)IAP_AppBase());
    }
    else if (IAP_AppBase() == IAP_FLASH_BASE)
    {
        printf("[IAP] linked base = 0x%08X (pre-migration, no bootloader)  NORMAL\r\n",
               (unsigned)IAP_AppBase());
        printf("[IAP] IAP needs: bootloader in S0 + app moved to Slot A (see IAP layout doc)\r\n");
    }
    else
    {
        printf("[IAP] *** WARN: linked base 0x%08X is neither Slot A (0x%08X) nor 0x%08X ***\r\n",
               (unsigned)IAP_AppBase(), (unsigned)IAP_SLOT_A_BASE, (unsigned)IAP_FLASH_BASE);
        printf("[IAP] *** wrong image flashed? bootloader will not reach it ***\r\n");
    }

    if (IAP_MetaRead(&m) == 1u)
    {
        printf("[IAP] META state=%u size=%u crc=%08X a_crc=%08X\r\n", (unsigned)m.state,
               (unsigned)m.fw_size, (unsigned)m.fw_crc, (unsigned)m.a_crc);
    }
    else
    {
        printf("[IAP] META empty (bootloader will just jump to Slot A)\r\n");
    }
}

void IAP_AppTick(uint32_t now)
{
    /* `now` is sampled at the start of the main-loop turn, while a REQUEST
     * or DATA frame may update its timestamp later in that same turn.  An
     * unsigned subtraction then underflows and expires a fresh request (or
     * trips the DATA watchdog) immediately.  Signed tick differences are the
     * usual wrap-safe comparison and correctly treat a future timestamp as
     * "not elapsed yet". */
    if (s_request_waiting && (int32_t)(now - s_request_at) >= 120000)
    {
        sprintf(s_text, "@IAP:NO,timeout#\r\n");
        iap_send_text();
        printf("[IAP] OTA request expired without user approval\r\n");
        s_request_waiting = 0u;
        s_request_total = 0u;
        s_request_crc = 0u;
        s_request_at = 0u;
    }
    if ((s_state == IAP_S_DATA || s_state == IAP_S_VERIFY || s_state == IAP_S_COMMIT) &&
        (int32_t)(now - s_activity) > 30000)
    {
        iap_fail(IAP_EC_STATE);
        return;
    }
    if (s_state == IAP_S_ERASE)
    {
        if (s_erase_at == 0u)
        {
            s_erase_at = now + IAP_ERASE_DELAY_MS;
        }
        else if ((int32_t)(now - s_erase_at) >= 0)
        {
            do_erase_now();
        }
        return;
    }
    if (s_state == IAP_S_DONE)
    {
        if (s_done_at == 0u)
        {
            s_done_at = now;
        }
#if (IAP_JUMP_TIMEOUT_MS != 0u)
        else if ((now - s_done_at) >= IAP_JUMP_TIMEOUT_MS)
        {
            do_jump();
        }
#endif
        return;
    }
}
