/* ============================================================================
 * iap_meta.h —— IAP 元数据（**App 写、bootloader 读**，两边共用同一份定义）
 *
 * 它回答 bootloader 开机时必须知道的三个问题：
 *   ① Slot B 里那份新固件**有多大、CRC 是多少**（决定要不要搬）
 *   ② 现在该不该搬（state）
 *   ③ 搬完之后 Slot A 里应该是哪一份（a_crc，回滚判据）
 *
 * ★★ 存放方式：**日志式追加**，不是"两份互备"。
 *   原因很实际：元数据整个都在 **Sector 1 这一个 16 KB 扇区**里，
 *   而 Flash 的擦除粒度就是**整个扇区** —— 想"只更新其中一份"是做不到的，
 *   擦一份必然把另一份一起擦掉。所以改成：**往扇区里追加 64 字节记录，写满再整扇区擦**。
 *   ⇒ 16 KB / 64 B = **256 条**，一次 OTA 约写 3 条 ⇒ 约 85 次升级才擦一次，
 *     顺带还拿到了磨损均衡。
 * ========================================================================= */
#ifndef __IAP_META_H
#define __IAP_META_H

#include "stm32f4xx.h"
#include "iap_flash.h"

#define IAP_META_MAGIC     0x31504149u      /* "IAP1"（小端：49 41 50 31） */
#define IAP_META_VERSION   1u

/* ---- 状态 ---- */
#define IAP_ST_EMPTY       0u   /* 占位；读出为空时不返回这个 */
#define IAP_ST_READY       1u   /* ★ Slot B 里有待提交的新固件，下次复位由 bootloader 搬 */
#define IAP_ST_CONFIRMED   2u   /* Slot A 已验过，就是 B 拷来的那一份 */
#define IAP_ST_FAILED      3u   /* 上次搬运失败 —— **保留现场供查**，不要盲目重试 */

/* ---- 记录大小：恰好 64 字节 ---- */
#define IAP_META_REC_SIZE  64u
#define IAP_META_MAX_REC   (IAP_META_SIZE / IAP_META_REC_SIZE)     /* = 256 */

typedef struct
{
    uint32_t magic;         /* IAP_META_MAGIC */
    uint32_t version;       /* IAP_META_VERSION */
    uint32_t seq;           /* 单调递增；日志式追加，越靠后越大 */
    uint32_t state;         /* IAP_ST_* */
    uint32_t fw_size;       /* Slot B 里那份固件的字节数（搬完 = Slot A 里那份的大小） */
    uint32_t fw_crc;        /* 上面那份的 CRC32（zlib 口径，见 iap_flash.h） */
    uint32_t a_crc;         /* 搬运前 Slot A 的 CRC32（回滚判据） */
    uint32_t reserved[8];   /* reserved[0]=IAP_LAYOUT_ID，其余填 0xFFFFFFFF */
    uint32_t crc;           /* 本结构体**前 60 字节**的 CRC32 */
} iap_meta_t;

/* 读到最后一条**有效**记录。1 = 成功；0 = 一条都没有（扇区是空的/全坏）。 */
uint8_t IAP_MetaRead(iap_meta_t *out);

/* 追加一条记录（seq 由本函数自增，不用调用者填）。
 * 扇区写满时**自动整扇区擦掉重来**。1 = 成功。 */
uint8_t IAP_MetaWrite(iap_meta_t *m);

/* 把所有域按"空"填好（magic/version/seq=0/state=READY/...），供调用者改。 */
void    IAP_MetaFillEmpty(iap_meta_t *m);

/* 物理擦掉元数据扇区 —— 恢复手段：擦完 bootloader 会「无元数据 ⇒ 直接跳 Slot A」。 */
uint8_t IAP_MetaEraseAll(void);

#endif /* __IAP_META_H */
