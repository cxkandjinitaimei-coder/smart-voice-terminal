/**
 * @file iap_flash.c
 * 用途：统一片内 Flash 分区、擦写边界、CRC32 与镜像校验底层。
 * 使用：应用和 Bootloader 共用本接口；布局改动必须同时更新链接区域和双方校验规则。
 * 时序与边界：A=0x08008000/224KiB，B=0x08040000/256KiB；Flash 擦除按扇区执行，写入前检查范围和对齐。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "iap_flash.h"

/* --------------------------------------------------------------------------
 * 扇区表
 * ------------------------------------------------------------------------ */
const uint32_t iap_sector_base[IAP_FLASH_SECTOR_COUNT] = {0x08000000u, 0x08004000u, 0x08008000u,
                                                          0x0800C000u, 0x08010000u, 0x08020000u,
                                                          0x08040000u, 0x08060000u};

const uint32_t iap_sector_size[IAP_FLASH_SECTOR_COUNT] = {
    16u * 1024u,  /* S0 */
    16u * 1024u,  /* S1 */
    16u * 1024u,  /* S2 */
    16u * 1024u,  /* S3 */
    64u * 1024u,  /* S4  ← 注意：这里开始变大 */
    128u * 1024u, /* S5 */
    128u * 1024u, /* S6 */
    128u * 1024u  /* S7 */
};

/* --------------------------------------------------------------------------
 * 刷 D-Cache（公开：iap_meta.c 直接读 Flash 时也要用）
 *
 *  为什么必须刷：`system_stm32f4xx.c:639` 起，SystemInit() 把
 *   FLASH_ACR_DCEN 置上了 ⇒ **D-Cache 是开着的**。
 *   擦/写完如果不刷，紧接着的回读可能命中**旧的缓存行** ⇒
 *   "校验失败"是假报警，或者更糟：把脏行留在缓存里。
 *   （F4 的 D-Cache 不参与 Flash 写入的字节序，但会干扰回读。）
 * ------------------------------------------------------------------------ */
void IAP_DCacheFlush(void)
{
    if ((FLASH->ACR & FLASH_ACR_DCEN) != 0u)
    {
        FLASH_DataCacheCmd(DISABLE);
        FLASH_DataCacheReset();
        FLASH_DataCacheCmd(ENABLE);
    }
}

/* --------------------------------------------------------------------------
 * 地址 → 扇区号
 * ------------------------------------------------------------------------ */
uint32_t IAP_SectorOf(uint32_t addr)
{
    uint32_t i;

    if ((addr < IAP_FLASH_BASE) || (addr >= IAP_FLASH_END))
    {
        return 0xFFFFFFFFu;
    }

    /* 从后往前找：低地址扇区小、高地址扇区大，倒着找第一个 base <= addr 的 */
    for (i = IAP_FLASH_SECTOR_COUNT; i > 0u; i--)
    {
        uint32_t s = i - 1u;
        if (addr >= iap_sector_base[s])
        {
            return s;
        }
    }
    return 0xFFFFFFFFu;
}

/* --------------------------------------------------------------------------
 * 擦扇区
 * ------------------------------------------------------------------------ */
uint8_t IAP_EraseSector(uint32_t sector)
{
    FLASH_Status st;

    if (sector >= IAP_FLASH_SECTOR_COUNT)
    {
        return IAP_ERR_RANGE;
    }

    /* FLASH_EraseSector 的入参是"SNB 位段"而不是扇区号：Sector_n = n << 3。
     * （stm32f4xx_flash.h 里 FLASH_Sector_0=0x00, _1=0x08, ... _7=0x38） */
    FLASH_Unlock();
    st = FLASH_EraseSector((uint32_t)sector << 3, VoltageRange_3);
    FLASH_Lock();

    IAP_DCacheFlush();

    return (st == FLASH_COMPLETE) ? IAP_OK : IAP_ERR_FLASH;
}

/* --------------------------------------------------------------------------
 * 写（按字）
 * ------------------------------------------------------------------------ */
uint8_t IAP_ProgramWords(uint32_t addr, const uint8_t *src, uint32_t len)
{
    uint32_t nwords;
    uint32_t i;
    FLASH_Status st;

    if ((addr & 3u) != 0u)
    {
        return IAP_ERR_ALIGN;
    }
    if ((src == 0) || addr < IAP_FLASH_BASE || addr > IAP_FLASH_END || len > IAP_FLASH_END - addr)
    {
        return IAP_ERR_RANGE;
    }
    if (len == 0u)
    {
        return IAP_OK;
    }

    nwords = (len + 3u) >> 2; /* 向上取整到字 */

    FLASH_Unlock();

    for (i = 0u; i < nwords; i++)
    {
        uint32_t word = 0xFFFFFFFFu; /* 尾部补 0xFF，与"擦后为 0xFF"一致 */
        uint32_t k;

        for (k = 0u; k < 4u; k++)
        {
            uint32_t idx = (i << 2) + k;
            if (idx < len)
            {
                word &= ~(0xFFu << (k * 8u));
                word |= ((uint32_t)src[idx]) << (k * 8u);
            }
        }

        st = FLASH_ProgramWord(addr + (i << 2), word);
        if (st != FLASH_COMPLETE)
        {
            FLASH_Lock();
            IAP_DCacheFlush();
            return IAP_ERR_FLASH;
        }
    }

    FLASH_Lock();
    IAP_DCacheFlush();

    return IAP_OK;
}

/* --------------------------------------------------------------------------
 * 回读比对
 * ------------------------------------------------------------------------ */
uint8_t IAP_MemEqualFlash(uint32_t addr, const uint8_t *src, uint32_t len)
{
    const uint8_t *fl = (const uint8_t *)addr;
    uint32_t i;

    if ((src == 0) || addr < IAP_FLASH_BASE || addr > IAP_FLASH_END || len > IAP_FLASH_END - addr)
    {
        return 0u;
    }

    IAP_DCacheFlush(); /* 保证读的是 Flash 的真值，不是缓存里的旧行 */

    for (i = 0u; i < len; i++)
    {
        if (fl[i] != src[i])
        {
            return 0u;
        }
    }
    return 1u;
}

/* --------------------------------------------------------------------------
 * CRC32（反射式 / zlib 口径）
 * ------------------------------------------------------------------------ */
uint32_t IAP_Crc32(const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    uint32_t k;

    for (i = 0u; i < len; i++)
    {
        crc ^= (uint32_t)p[i];
        for (k = 0u; k < 8u; k++)
        {
            if ((crc & 1u) != 0u)
            {
                crc = (crc >> 1) ^ 0xEDB88320u;
            }
            else
            {
                crc = (crc >> 1);
            }
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

uint32_t IAP_Crc32Flash(uint32_t addr, uint32_t len)
{
    /* F4 的 Flash 是内存映射的，直接当数组读即可（先刷 D-Cache） */
    IAP_DCacheFlush();
    return IAP_Crc32((const void *)addr, len);
}

uint16_t IAP_FlashCapacityKB(void)
{
#ifdef IAP_TEST_LINK_BASE
    return 512u;
#else
    return *(const uint16_t *)0x1FFF7A22u;
#endif
}

uint8_t IAP_BootLayoutValid(void)
{
    const uint32_t *d = (const uint32_t *)IAP_BOOT_DESCRIPTOR_ADDR;
    return (uint8_t)((IAP_FlashCapacityKB() == 512u || IAP_FlashCapacityKB() == 1024u) &&
                     d[0] == IAP_BOOT_DESCRIPTOR_MAGIC && d[1] == IAP_LAYOUT_ID &&
                     d[2] == IAP_SLOT_A_BASE && d[3] == IAP_SLOT_A_SIZE &&
                     d[4] == IAP_SLOT_B_BASE && d[5] == IAP_SLOT_B_SIZE && d[6] == IAP_BOOT_SIZE &&
                     d[7] == IAP_Crc32(d, 28u));
}
