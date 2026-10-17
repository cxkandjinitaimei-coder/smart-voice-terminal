/**
 * @file iap_flash.h
 * 用途：统一片内 Flash 分区、擦写边界、CRC32 与镜像校验底层。
 * 使用：应用和 Bootloader 共用本接口；布局改动必须同时更新链接区域和双方校验规则。
 * 时序与边界：A=0x08008000/224KiB，B=0x08040000/256KiB；Flash 擦除按扇区执行，写入前检查范围和对齐。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __IAP_FLASH_H
#define __IAP_FLASH_H

#include "stm32f4xx.h"

/* ==========================================================================
 *  F407VET6 扇区表（512 KB，**扇区大小不均匀 —— 这是本板最反直觉的一点**）
 * ==========================================================================
 * 0x08000000 S0  16 KB   ┐
 * 0x08004000 S1  16 KB   │
 * 0x08008000 S2  16 KB   │  这 4 个是 16 KB
 * 0x0800C000 S3  16 KB   ┘
 * 0x08010000 S4  64 KB   ← 突然变大
 * 0x08020000 S5 128 KB   ┐
 * 0x08040000 S6 128 KB   │  这 3 个是 128 KB
 * 0x08060000 S7 128 KB   ┘
 *
 *  最容易被忽略的后果：**「擦掉一个扇区」的代价可能是 128 KB 而不是 16 KB**。
 *    所以下面 Slot A 选 S2+S3+S4+S5（16+16+64+128）而不是"看上去更整"的 S4 单块。
 */
#define IAP_FLASH_SECTOR_COUNT 8u
#define IAP_FLASH_BASE 0x08000000u
#define IAP_FLASH_SIZE (512u * 1024u)
#define IAP_FLASH_END (IAP_FLASH_BASE + IAP_FLASH_SIZE)

extern const uint32_t iap_sector_base[IAP_FLASH_SECTOR_COUNT];
extern const uint32_t iap_sector_size[IAP_FLASH_SECTOR_COUNT];

/* ==========================================================================
 *  槽布局（**与 `docs/flash-and-ota.md §3.1` 必须一字不差**）
 * ==========================================================================
 * Bootloader 16 KB  S0  0x08000000    永不擦除（擦它 = 自毁）
 * 元数据     16 KB  S1  0x08004000   日志式追加（64 B/条，满了整扇区擦）
 * Slot A    224 KB  S2+S3+S4+S5          App 就链接在这里
 * Slot B    256 KB  S6+S7 0x08040000   只当暂存，**永不就地执行**
 * 512 KB fully allocated; no independently erasable spare sector
 */
#define IAP_BOOT_BASE 0x08000000u /* S0 */
#define IAP_BOOT_SECTOR 0u
#define IAP_BOOT_SIZE (16u * 1024u)

#define IAP_META_BASE 0x08004000u /* S1 */
#define IAP_META_SECTOR 1u
#define IAP_META_SIZE (16u * 1024u)

#define IAP_SLOT_A_BASE 0x08008000u /* S2..S5 */
#define IAP_SLOT_A_FIRST_SECTOR 2u
#define IAP_SLOT_A_SECTORS 4u
#define IAP_SLOT_A_SIZE (224u * 1024u)

#define IAP_SLOT_B_BASE 0x08040000u /* S6..S7 */
#define IAP_SLOT_B_FIRST_SECTOR 6u
#define IAP_SLOT_B_SECTORS 2u
#define IAP_SLOT_B_SIZE (256u * 1024u)

/*  固件体积硬上限 = min(Slot A, Slot B) = 224 KB。
 *
 * Slot B(256 KB) **比 Slot A(224 KB) 大**，而 B→A 是**逐字节拷贝、不是截断**。
 * ⇒ 一个 240 KB 的固件能完整下进 B、CRC 也全绿，bootloader 往 A 拷到一半溢出
 *   ⇒ **表现为「升级成功但变砖」**，且此时 Slot A 已被擦掉。
 * ⇒ 状态机必须在 META/BEGIN 阶段就拒（回 ERR,SIZE），**不能等收完再发现**。 */
/* S0 descriptor binds App OTA to the physically installed boot layout. */
#define IAP_LAYOUT_ID 0x00020001u
#define IAP_BOOT_DESCRIPTOR_ADDR (IAP_BOOT_BASE + IAP_BOOT_SIZE - 32u)
#define IAP_BOOT_DESCRIPTOR_MAGIC 0x3241594Cu
#define IAP_BOOT_DESCRIPTOR_CRC 0x559C4C58u
uint8_t IAP_BootLayoutValid(void);
uint16_t IAP_FlashCapacityKB(void);

#define IAP_FW_SIZE_MAX IAP_SLOT_A_SIZE /* = 224 KB */

/* ==========================================================================
 *  返回值（不是 FLASH_Status，是本模块自己的口径）
 * ========================================================================== */
#define IAP_OK 0u
#define IAP_ERR_RANGE 1u  /* 扇区号 / 地址越界 */
#define IAP_ERR_ALIGN 2u  /* 地址没 4 字节对齐 */
#define IAP_ERR_FLASH 3u  /* 硬件擦写失败（FLASH_Status != COMPLETE） */
#define IAP_ERR_VERIFY 4u /* 写后回读不一致 */

/* ==========================================================================
 *  接口
 * ========================================================================== */

/*  刷 D-Cache（见 .c 里的说明：SystemInit 把 DCEN 打开了，不刷会读到旧缓存行）。
* 擦 / 写 / 回读比对之后都要调；本模块内部已经调过，外部只在"绕过本模块直接读 Flash"时才需要。*/
void IAP_DCacheFlush(void);

/* 地址 → 扇区号；越界返回 0xFFFFFFFF。
 * 用途：IAP 开局先自检「我要擦的扇区是不是 Slot A/B 的」，防止擦错自己。 */
uint32_t IAP_SectorOf(uint32_t addr);

/* 擦一个扇区。 S5~S7 是 128 KB，**耗时 1~4 秒且期间 CPU 被硬件拉停**
 * （F4 单 bank：擦写时取指停在总线上 ⇒ 中断不响应 ⇒ 连 printf 都发不出去，
 *   串口会静默。这是正常的，不是死机）。
 *  本函数**没有超时**（底层 StdPeriph 的 FLASH_WaitForLastOperation 是无界轮询）。
 *    若 Flash 卡在 BUSY，这里会一直挂着，只能复位 —— 对本板可接受，记在此备查。 */
uint8_t IAP_EraseSector(uint32_t sector);

/* 从 src 写 len 字节到 addr（addr 必须 4 字节对齐）。
 * 尾部不足 4 字节的按 0xFF 补齐 —— 与本板 Flash 的"擦后为 0xFF"语义一致，
 * 不会往尾部引入 0x00。
 *  F4 只能按 **字(32 位)** 编程，没有字节写。别指望能单写一个字节。 */
uint8_t IAP_ProgramWords(uint32_t addr, const uint8_t *src, uint32_t len);

/* 回读比对：flash[addr..addr+len) 与 src 是否逐字节相同。1 = 相同。 */
uint8_t IAP_MemEqualFlash(uint32_t addr, const uint8_t *src, uint32_t len);

/* CRC32（反射式，poly 0xEDB88320，init 0xFFFFFFFF，末尾取反）
 *  选它而**不是** STM32 的硬件 CRC 单元，唯一理由是**跨平台一致**：
 *   硬件 CRC 用的是非反射多项式(0x04C11DB7)，ESP8266/PC 侧算不出同一个值。
 *   反射式这个就是 zlib/`crc32` 的口径，ESP 那边一行就能算出同样的数。
 * 代价：逐位算，96 KB 约 30 ms（168 MHz）。一次性开销，可接受。 */
uint32_t IAP_Crc32(const void *buf, uint32_t len);
uint32_t IAP_Crc32Flash(uint32_t addr, uint32_t len);

#endif /* __IAP_FLASH_H */
