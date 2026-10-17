/**
 * @file iap_meta.h
 * 用途：S1 元数据日志的校验、序号选择和状态持久化。
 * 使用：升级提交与启动搬运调用追加记录，读取时选有效且最新的一条。
 * 时序与边界：READY 表示 B 完整待安装，CONFIRMED 表示 A 已校验；CRC 及写入顺序防止把半条记录视为有效。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __IAP_META_H
#define __IAP_META_H

#include "stm32f4xx.h"
#include "iap_flash.h"

#define IAP_META_MAGIC 0x31504149u /* "IAP1"（小端：49 41 50 31） */
#define IAP_META_VERSION 1u

/* ---- 状态 ---- */
#define IAP_ST_EMPTY 0u /* 占位；读出为空时不返回这个 */
#define IAP_ST_READY 1u /*  Slot B 里有待提交的新固件，下次复位由 bootloader 搬 */
#define IAP_ST_CONFIRMED 2u /* Slot A 已验过，就是 B 拷来的那一份 */
#define IAP_ST_FAILED 3u /* 上次搬运失败 —— **保留现场供查**，不要盲目重试 */

/* ---- 记录大小：恰好 64 字节 ---- */
#define IAP_META_REC_SIZE 64u
#define IAP_META_MAX_REC (IAP_META_SIZE / IAP_META_REC_SIZE) /* = 256 */

typedef struct
{
    uint32_t magic;   /* IAP_META_MAGIC */
    uint32_t version; /* IAP_META_VERSION */
    uint32_t seq;     /* 单调递增；日志式追加，越靠后越大 */
    uint32_t state;   /* IAP_ST_* */
    uint32_t fw_size; /* Slot B 里那份固件的字节数（搬完 = Slot A 里那份的大小） */
    uint32_t fw_crc;      /* 上面那份的 CRC32（zlib 口径，见 iap_flash.h） */
    uint32_t a_crc;       /* 搬运前 Slot A 的 CRC32（回滚判据） */
    uint32_t reserved[8]; /* reserved[0]=IAP_LAYOUT_ID，其余填 0xFFFFFFFF */
    uint32_t crc;         /* 本结构体**前 60 字节**的 CRC32 */
} iap_meta_t;

/* 读到最后一条**有效**记录。1 = 成功；0 = 一条都没有（扇区是空的/全坏）。 */
uint8_t IAP_MetaRead(iap_meta_t *out);

/* 追加一条记录（seq 由本函数自增，不用调用者填）。
 * 扇区写满时**自动整扇区擦掉重来**。1 = 成功。 */
uint8_t IAP_MetaWrite(iap_meta_t *m);

/* 把所有域按"空"填好（magic/version/seq=0/state=READY/...），供调用者改。 */
void IAP_MetaFillEmpty(iap_meta_t *m);

/* 物理擦掉元数据扇区 —— 恢复手段：擦完 bootloader 会「无元数据 ⇒ 直接跳 Slot A」。 */
uint8_t IAP_MetaEraseAll(void);

#endif /* __IAP_META_H */
