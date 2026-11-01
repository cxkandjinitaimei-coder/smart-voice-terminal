/* ==================================================================
 * w25q_img.h —— 往 W25Q 里搬"一整块镜像"的写内核（**通道无关**）
 * ------------------------------------------------------------------
 * 【为什么单独一层】`Hardware/22W25Q/w25q.c` 只到芯片操作（读 / 页编程 /
 *   扇区擦 / 块擦），它**不管**你要写进去的是什么。而"把 127686 字节的字库
 *   一字不差地灌进 Flash"这件事有三条硬规矩：
 *     ① **页编程不得跨页**（`w25q.h:140-143`）—— 跨页会被器件**回卷到页首
 *        覆盖**，而且**不报错**；
 *     ② 写完**必须回读比对**，不能信返回值 —— 本项目踩过"WEL 没置上 ⇒
 *        什么都不写、读回来还是 0xFF、全程零报错"（`问题日志_AI速查.md`
 *        硬件段有这条）；
 *     ③ 擦除跨 64KB 时用块擦（快 5 倍），但**块擦最长 800ms**，调用方得知道。
 *   这三条跟"数据是从串口来的、从 SD 卡来的、还是从片内常量表来的"
 *   **毫无关系** ⇒ 抽成本模块：传输通道换几次，这里一行都不用改。
 *
 * 【★ 本模块绝不自己碰 SPI】只调 `w25q.h` 的
 *   `W25Q_Read / PageProgram / SectorErase / BlockErase / ReadStatus`，
 *   **不新增任何字节收发、不自己拉 CS**。
 *   理由是一条已经付过代价的教训：commit `5781373` 修的就是"SPI 主机只发
 *   不收 ⇒ RXNE 一直挂着上一字节的残留 ⇒ 整条读通路**错位一个字节**"。
 *   多一套 SPI 原语就多一次重犯的机会（详见 `w25q.c` 顶部横幅）。
 *
 * 【本模块不 printf】照 `w25q.c` 的纪律：打印留给调用方。
 *   对外只有 **返回码** 和 **`*fail_off`（绝对地址）**。
 *
 * 【★ 地址一律是"绝对片内地址"】不是相对偏移。调用方自己做 base + off
 *   （字库的 base 在 `font_store.h`）。这样出错时拿到的地址可以直接丢给
 *   一个 flash dump 去看，不用再换算一次。
 * ================================================================== */
#ifndef __W25Q_IMG_H
#define __W25Q_IMG_H

#include "stm32f4xx.h"

/* ---------- 返回码（本模块自己的口径） ----------
 * ★ 前三个码与 `w25q.h` 的 `W25Q_ERR_*` **故意不共用**：那些是"芯片/SPI
 *   层面"的错，这几个是"我要写的这块镜像"层面的错，混在一起会让调用方
 *   分不清该去查接线还是查数据。 */
#define W25QIMG_OK              0u   /* 全对 */
#define W25QIMG_ERR_SRC         1u   /* 数据源回调没给够字节（只有 WriteRange 会返回） */
#define W25QIMG_ERR_PARAM       2u   /* 地址越界 / 长度 0 / 缓冲区空指针 */
#define W25QIMG_ERR_ERASE       3u   /* 擦除失败（见 *fail_off 是哪个地址） */
#define W25QIMG_ERR_PROGRAM     4u   /* 页编程调用本身报错 */
#define W25QIMG_ERR_VERIFY      5u   /* 回读比对不一致（写没生效，但 WEL 已清） */
#define W25QIMG_ERR_WEL         6u   /* ★ 回读不符 **且** WEL 还是 1 ⇒ 这次编程根本没执行 */
#define W25QIMG_ERR_SPI         7u   /* 读回读时 W25Q_Read 报错 */
#define W25QIMG_ERR_NOT_ERASED  8u   /* IsErased 发现非 0xFF 字节 */

/* ---------- 数据源回调（WriteRange 用） ----------
 * 语义：把源里 [off, off+want) 填进 buf，返回**实际填了几个字节**。
 *   · 返回 want          = 正常
 *   · 返回 0 < n < want  = 源到末尾了，写 n 个就算这一段结束（不是错误）
 *   · 返回 0             = 源给不出数据了 ⇒ ERR_SRC
 * `off` 是**相对整个 range 起点**的偏移。 */
typedef uint32_t (*W25QImg_Source)(void *ctx, uint32_t off, uint8_t *buf, uint32_t want);

/* ------------------------------------------------------------------
 * 写一段**已经在 RAM 里**的数据。串口分块传走的就是这条路。
 *
 * 内部做三件事，一件都不省：
 *   1. 按 `n = min(剩余, 256 - (addr % 256))` **拆到不跨页**再调 PageProgram；
 *   2. 每个子块写完后，读回**它所在的那一整页（256B，页对齐）**，比对**刚写的
 *      那一段**（★ 不抽样：这个坑的形态是"整片 0xFF / 整片按位与"，抽样会踩空）；
 *   3. 比对失败时再查一次 SR1 的 WEL —— **WEL 仍为 1 说明这次编程压根没执行**
 *      （器件在写周期结束时自动清 WEL），那是 `ERR_WEL`，不是 `ERR_VERIFY`。
 *      ⚠️ 反过来：**WEL==1 但数据对得上**，判 OK 不报错 —— 那是器件怪癖，
 *      喊狼来了比不喊更坏。
 *
 * `*fail_off` 在失败时填**绝对地址**（可为 0 传 NULL）。
 * 返回 W25QIMG_OK / ERR_PARAM / ERR_PROGRAM / ERR_VERIFY / ERR_WEL / ERR_SPI。
 *
 * ⚠️ **调用方要保证 dest 这一段已经是 0xFF**（先 EraseRange）。Flash 只能把
 *    1 写成 0，往没擦干净的地方写，读回来是"新旧按位与"，比对必然失败。
 * ------------------------------------------------------------------ */
uint8_t W25QImg_WriteChunk(uint32_t addr, const uint8_t *buf, uint32_t len,
                           uint32_t *fail_off);

/* ------------------------------------------------------------------
 * 从**回调数据源**拉数据写（PC harness / 将来的片内常量表 / SD 卡文件都走它）。
 * 建在 WriteChunk 之上，所以页拆分 + 回读比对一条不少。
 *
 * `skip_if_equal != 0` 时：每个子块写之前先读回比一次，**已经对的就跳过编程**
 *   —— 断点续传用。注意"跳过"的依据是"内容和源一致"，不是"不是 0xFF"。
 * ------------------------------------------------------------------ */
uint8_t W25QImg_WriteRange(uint32_t addr, uint32_t len, W25QImg_Source src,
                           void *ctx, uint8_t skip_if_equal, uint32_t *fail_off);

/* 把 Flash 里 [addr, addr+len) 与 RAM 里的 expect 逐字节比对。
 * 返回 OK / ERR_PARAM / ERR_SPI / ERR_VERIFY（*fail_off 是第一个不符的绝对地址）。 */
uint8_t W25QImg_VerifyRange(uint32_t addr, const uint8_t *expect, uint32_t len,
                            uint32_t *fail_off);

/* 擦 [addr, addr+len)。**自动对齐**：起点向下、终点向上各凑到一个 4KB 扇区
 * （擦除粒度就是整个扇区，凑齐是唯一正确的做法）。
 * 64KB 对齐的整段走 **BlockErase**（典型 150ms，比 16 次 SectorErase 快 5 倍）。
 * ⚠️ **这是阻塞调用**：典型 0.3s，最坏按 4s 留余量（两次块擦各按 800ms 上限
 *    再留一倍）。调用方在它前后要打日志。
 * 返回 OK / ERR_PARAM / ERR_ERASE（*fail_addr 是失败的绝对地址）。 */
uint8_t W25QImg_EraseRange(uint32_t addr, uint32_t len, uint32_t *fail_addr);

/* [addr, addr+len) 是不是"全 0xFF"（= 已擦）。
 * 返回 OK = 是全 FF；ERR_NOT_ERASED = 不是，*first_bad_off 填第一个非 FF 的
 * **绝对地址**；ERR_PARAM / ERR_SPI 同上。 */
uint8_t W25QImg_IsErased(uint32_t addr, uint32_t len, uint32_t *first_bad_off);

/* 写一条**定长小记录**（完成标记就 16 字节）。
 * ⚠️ 它**不擦** —— 目标那 16 字节必须已经是 0xFF（标记独占一个扇区就是为了
 *    让这件事永远成立）。走 WriteChunk，所以回读 + WEL 一样不少。 */
uint8_t W25QImg_WriteRecord(uint32_t addr, const uint8_t *rec, uint32_t len);

/* ---------- CRC32（与 PC 端 `zlib.crc32` **逐位同值**） ----------
 * 反射式多项式 0xEDB88320、初值 0xFFFFFFFF、末尾异或 0xFFFFFFFF ——
 * 这三个数就是 zlib 的定义，改一个两边就对不上。
 * 拆成三个函数是为了让"哪一步异或"这件事没法被理解错。 */
uint32_t W25QImg_Crc32Init(void);
uint32_t W25QImg_Crc32Update(uint32_t crc, const uint8_t *buf, uint32_t len);
uint32_t W25QImg_Crc32Final(uint32_t crc);

/* 流式读 [addr, addr+len) 算 CRC32（内部按 256B 一块读，不占大缓冲）。
 * 返回 OK / ERR_PARAM / ERR_SPI；*crc_out 填最终值（ERR 时无意义）。 */
uint8_t W25QImg_Crc32Range(uint32_t addr, uint32_t len, uint32_t *crc_out);

#endif /* __W25Q_IMG_H */
