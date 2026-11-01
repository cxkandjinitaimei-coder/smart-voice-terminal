/* ============================================================================
 * iap_meta.c —— 见 iap_meta.h
 * ========================================================================= */
#include "iap_meta.h"

/* ★ 编译期断言：记录必须是 64 字节整。
 * 以后谁改字段（比如把 reserved[8] 改成 [7]）忘了配平，
 * 会**在这里直接编译报错**（数组长度不能为负），而不是运行期悄悄错位 ——
 * 那种错会表现为"元数据偶尔读不出来"，极难查。 */
typedef char iap_meta_size_must_be_64[
    (sizeof(iap_meta_t) == IAP_META_REC_SIZE) ? 1 : -1
];

/* 记录在扇区内的地址 */
static uint32_t meta_addr(uint32_t idx)
{
    return IAP_META_BASE + (idx * IAP_META_REC_SIZE);
}

/* 单条记录是否有效：magic 对 + version 对 + 自身 CRC 对 */
static uint8_t meta_rec_valid(const iap_meta_t *r)
{
    uint32_t want;

    if (r->magic != IAP_META_MAGIC)   { return 0u; }
    if (r->version != IAP_META_VERSION) { return 0u; }

    want = IAP_Crc32(r, IAP_META_REC_SIZE - 4u);

    return (want == r->crc) ? 1u : 0u;
}

void IAP_MetaFillEmpty(iap_meta_t *m)
{
    uint32_t i;

    m->magic   = IAP_META_MAGIC;
    m->version = IAP_META_VERSION;
    m->seq     = 0u;
    m->state   = IAP_ST_READY;
    m->fw_size = 0u;
    m->fw_crc  = 0u;
    m->a_crc   = 0u;
    for (i = 0u; i < 8u; i++)
    {
        m->reserved[i] = 0xFFFFFFFFu;
    }
    m->reserved[0] = IAP_LAYOUT_ID;
    m->crc     = 0u;    /* 由写的那一方最后算 */
}

/* --------------------------------------------------------------------------
 * 读：扫全扇区，取**最后一条**有效记录
 * ------------------------------------------------------------------------ */
uint8_t IAP_MetaRead(iap_meta_t *out)
{
    uint32_t      i;
    const iap_meta_t *p;
    uint8_t       found = 0u;

    if (out == 0)
    {
        return 0u;
    }

    /* 回读之前先刷 D-Cache，否则可能读到缓存里的旧行（见 iap_flash.c 的说明） */
    IAP_DCacheFlush();

    for (i = 0u; i < IAP_META_MAX_REC; i++)
    {
        p = (const iap_meta_t *)meta_addr(i);

        if (meta_rec_valid(p) != 0u)
        {
            *out  = *p;
            found = 1u;
        }
        else if ((p->magic == 0xFFFFFFFFu) && (found != 0u))
        {
            /* 遇到第一条"从没写过"的记录 ⇒ 后面不可能再有有效的了，提前收工 */
            break;
        }
    }

    return found;
}

/* --------------------------------------------------------------------------
 * 写：追加
 * ------------------------------------------------------------------------ */
uint8_t IAP_MetaWrite(iap_meta_t *m)
{
    iap_meta_t    cur;
    uint32_t      base_seq;
    uint32_t      idx;
    uint32_t      i;
    const iap_meta_t *p;

    if (m == 0)
    {
        return 0u;
    }

    /* seq 由这里统一递增，调用者不用操心 */
    base_seq = (IAP_MetaRead(&cur) != 0u) ? cur.seq : 0u;
    m->seq   = base_seq + 1u;
    m->magic = IAP_META_MAGIC;
    m->version = IAP_META_VERSION;
    m->crc   = IAP_Crc32(m, IAP_META_REC_SIZE - 4u);

    /* 找第一条尚未写过的记录位 */
    idx = IAP_META_MAX_REC;     /* 哨兵 = 没找到 */
    for (i = 0u; i < IAP_META_MAX_REC; i++)
    {
        p = (const iap_meta_t *)meta_addr(i);
        if (p->magic == 0xFFFFFFFFu)
        {
            idx = i;
            break;
        }
    }

    if (idx == IAP_META_MAX_REC)
    {
        /* 写满了：整扇区擦掉，从 0 号重来。
         * ★ 这一次擦除把**所有**历史记录都清掉了 —— 所以下一条一定是唯一的一条。 */
        if (IAP_EraseSector(IAP_META_SECTOR) != IAP_OK)
        {
            return 0u;
        }
        idx = 0u;
    }

    if (IAP_ProgramWords(meta_addr(idx), (const uint8_t *)m, IAP_META_REC_SIZE) != IAP_OK)
        return 0u;
    return IAP_MemEqualFlash(meta_addr(idx), (const uint8_t *)m, IAP_META_REC_SIZE);
}

/* --------------------------------------------------------------------------
 * 擦掉整个元数据扇区（恢复手段）
 * ------------------------------------------------------------------------ */
uint8_t IAP_MetaEraseAll(void)
{
    return IAP_EraseSector(IAP_META_SECTOR);
}
