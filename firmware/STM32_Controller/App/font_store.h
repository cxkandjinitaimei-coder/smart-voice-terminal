/* ==================================================================
 * font_store.h —— 板载 W25Q 上的**扩展字库**（布局 + 头部解析 + 查字）
 * ------------------------------------------------------------------
 * 【这一层管什么】把 `Tools/font_gen/FONT16.BIN` 那 127686 字节当成
 *   "一块已安装的字库"来对待：校验它、把索引读进 RAM、按码点查 32 字节字模。
 *   下面垫着 `App/w25q_img.c`（擦/写/回读），再下面垫着
 *   `Hardware/22W25Q/w25q.c`（芯片操作）。**这一层才有"字库"这个概念。**
 *
 * 【★ 地址布局 —— 全工程唯一事实来源就是这里，别在别处再写一份】
 *
 *     0x000000 - 0x000FFF     4KB   ★ 留给 `W25Q_SelfTestRW()` 的试验田
 *     0x001000 - 0x00FFFF    60KB   预留（将来二级汉字 / 第二份字库）
 *     0x010000 - 0x02FFFF   128KB   ★ 字库镜像 = block 1 + block 2
 *     0x030000 - 0x030FFF     4KB   ★ 完成标记 F16R（独占一扇区）
 *
 *   ★ **base 刻意不从 0 起**：`W25Q_SelfTestRW()` 会擦掉 0 号扇区。
 *     哪天有人把它接成一条串口命令，"手敲一条命令就砸掉字库头"这种事
 *     只要 base=0 就一定会发生。挪到 block 1 起，**驱动一行都不用改**。
 *
 * 【镜像格式 = FONT16.BIN，一个字节都不加】
 *     偏移 0        16 B    头部（全小端）
 *                             +0  4s  magic       b'F16B'
 *                             +4  H   version     1
 *                             +6  B   w           16
 *                             +7  B   h           16
 *                             +8  I   count       N
 *                             +12 I   glyph_bytes 32
 *     偏移 16       2N B    码点索引：uint16 小端，**严格升序**
 *     偏移 16+2N    32N B   字模：第 i 个字在 16+2N+32i
 *     合计 = 16 + 34N
 *
 *   ★ **头里没有校验和**，这是用户定的（加 CRC32 会改掉 `16+34N` 这个式子）。
 *     ⇒ 完整性靠**另一处的 16 字节标记** + 回读时的 CRC32，不动文件格式。
 *
 * 【★ 完成标记不是可选项 —— 它是"两阶段提交"的那一半】
 *   回读校验只证明"**这一次**写对了"，证明不了"Flash 里**现在这一份**是完整的"。
 *   灌到一半断电的危险形态是：**头部完好 + 字模区是 0xFF** ⇒ 上电校验全绿
 *   ⇒ 屏上一片**实心方块**（或者更糟：画出一堆莫名其妙的字），而且不报错。
 *   ⇒ 装字库的顺序**就是安全性的全部**：
 *        ① 先校验"要装的东西"合法（坏文件绝不能先毁掉现有字库）
 *        ② **先擦掉旧标记**  ← 这一刀之后，断电就只会干净地退回片内 32 字
 *        ③ 擦 → 写 → 校验镜像
 *        ④ 最后才写标记
 *
 * 【★ RAM 账，换字表之前必须重算】
 *   索引数组 `s_cp[FONTSTORE_MAX_GLYPHS]` = **8192 B** 常驻。
 *   ⇒ `FONTSTORE_MAX_GLYPHS = 4096` 是**这一版的天花板**：
 *        · GB2312 一级 3755 字   → 7510 B   ✅ 装得下（本版默认）
 *        · GB2312 一+二级 6763 字 → 13526 B  ❌ 超了 ⇒ 要改这个宏 + 重估 RAM
 *        · CJK 基本区全量 20902 字 → 41804 B ❌ **本设计不支持**（别硬试）
 *   ⚠️ `count > FONTSTORE_MAX_GLYPHS` **必须在拷贝之前就拒**，
 *      否则就是缓冲区溢出 —— 这是 RAM 数组的越界闸门，不是建议。
 * ================================================================== */
#ifndef __FONT_STORE_H
#define __FONT_STORE_H

#include "stm32f4xx.h"

/* ---------- 布局（唯一事实来源） ---------- */
#define FONTSTORE_BASE          0x00010000u  /* 镜像起点 = block 1 */
#define FONTSTORE_MAX_BYTES     0x00020000u  /* 镜像区大小 = 128KB（block 1+2） */
#define FONTSTORE_MARK_ADDR     0x00030000u  /* 完成标记：独占一个 4KB 扇区 */
#define FONTSTORE_MARK_BYTES    16u

/* ---------- 镜像格式常量 ---------- */
#define FONTSTORE_HDR_BYTES     16u
#define FONTSTORE_GLYPH_BYTES   32u
#define FONTSTORE_INDEX_BYTES   2u
#define FONTSTORE_W             16u
#define FONTSTORE_H             16u
#define FONTSTORE_VERSION       1u

/* 索引数组的容量上限 = 越界闸门。理由见文件头「RAM 账」。 */
#define FONTSTORE_MAX_GLYPHS    4096u

/* 镜像总字节数 = 16 + 34N。写成一个宏，免得三处各算一遍算错。 */
#define FONTSTORE_IMAGE_BYTES(n) \
    (FONTSTORE_HDR_BYTES + (FONTSTORE_INDEX_BYTES + FONTSTORE_GLYPH_BYTES) * (n))

/* ---------- 返回码 ---------- */
#define FONTSTORE_OK            0u
#define FONTSTORE_ERR_NO_MARK   1u   /* 标记区没有有效标记：没装过 / 灌一半断过电 */
#define FONTSTORE_ERR_MARK_HDR  2u   /* 标记本身坏了（magic / version / 保留位不对） */
#define FONTSTORE_ERR_HEADER    3u   /* 镜像头部有字段对不上（magic/ver/w/h/glyph） */
#define FONTSTORE_ERR_COUNT     4u   /* count 为 0 或超过 FONTSTORE_MAX_GLYPHS */
#define FONTSTORE_ERR_LEN       5u   /* len != 16 + 34*count，或与标记里记的长度不符 */
#define FONTSTORE_ERR_INDEX     6u   /* 码点索引不是**严格升序**（重复或乱序） */
#define FONTSTORE_ERR_GLYPH     7u   /* 首/末字模全是 0x00 或全是 0xFF（索引写了、字模区没写） */
#define FONTSTORE_ERR_SPI       8u   /* 读 W25Q 失败（芯片不在线 / 通信错） */
#define FONTSTORE_ERR_NOTREADY  9u   /* 还没成功 Init 就调了查字 / 校验 */
#define FONTSTORE_ERR_CRC       10u  /* 重算的 CRC32 与标记里存的不一致 */
#define FONTSTORE_ERR_PARAM     11u  /* 参数不合法 */

/* ==================== 安装 / 状态 ==================== */

/* 上电（或刚灌完）调一次：读标记 → 校镜像头 → 校验索引 → 探首末字模
 * → 把索引读进 RAM。**只读，不擦不写。**
 * 返回 FONTSTORE_OK 才算可用；失败时 `FontStore_Ready()` 为 0，
 * 上层应当**整体退回片内 32 字**（热路径一次 SPI 都不发）。
 * ★ 为什么不在这一步跑全量 CRC32：127686 字节约 0.15s，每次上电都跑不值；
 *   而"灌一半断电"这个真正危险的形态，已经被**标记两阶段提交**挡住了。 */
uint8_t     FontStore_Init(void);

/* 1 = 字库可用（= 最近一次 Init 成功，且没被 Uninstall）。 */
uint8_t     FontStore_Ready(void);

/* 当前有效的字数（不可用时返回 0）。 */
uint16_t    FontStore_Count(void);

/* 最近一次失败的原因码（FONTSTORE_OK = 没失败过）。 */
uint8_t     FontStore_LastError(void);

/* ★ 状态代号：每次 `Init()` / `Uninstall()` 各 +1，其余时候不变。
 *   查表层（`font.c` 的单字缓存）拿它当**失效判据**。
 *   为什么要有它：单字缓存按"码点"命中，而字库可以被换掉 / 卸掉 ——
 *   缓存里那个字就变成了**旧字库的字**，画出来是错的而且**不报错**。
 *   靠"调用方记得清缓存"的写法本轮已经踩过一次（`font_upload.c` 里
 *   那两处 `Font_Invalidate()`），所以这里给一个**忘了也不会错**的判据：
 *   只要字库状态动过，代号就变，缓存自动不命中。 */
uint32_t    FontStore_Gen(void);

/* 错误码 → 短 ASCII 名字（只用于打印，如 "NO_MARK"）。 */
const char *FontStore_ErrName(uint8_t rc);

/* ==================== 查字 ==================== */

/* 按码点查 32 字节字模，写进 `out32`（调用方自己的缓冲）。
 * 返回 1 = 查到；0 = 没有（**没查到也要一次 SPI 都不发** —— 二分在 RAM 里）。 */
uint8_t     FontStore_Glyph(uint16_t cp, uint8_t *out32);

/* ==================== 给传输层用的 ==================== */

uint32_t    FontStore_Base(void);        /* = FONTSTORE_BASE */
uint32_t    FontStore_MarkAddr(void);    /* = FONTSTORE_MARK_ADDR */
uint32_t    FontStore_ImageBytes(void);  /* 已安装镜像的实际长度；不可用返回 0 */

/* 纯函数（**不碰 Flash**）：校验一个 16 字节镜像头 + 它声称的总长。
 * 用来在**擦掉任何东西之前**判断"这个文件能不能装"。
 * 返回 FONTSTORE_OK 或具体的错码 —— 坏文件绝不能先毁掉现有字库。 */
uint8_t     FontStore_CheckHeader(const uint8_t *hdr16, uint32_t len);

/* 擦掉完成标记（= 让字库失效，但**不动镜像**）。改标记前必须先擦，
 * 因为 Flash 只能把 1 写成 0。 */
uint8_t     FontStore_MarkErase(void);

/* 写完成标记（len + crc32）。★ 只在镜像**写完且校验过**之后调。 */
uint8_t     FontStore_MarkWrite(uint32_t len, uint32_t crc);

/* 流式回读整镜像重算 CRC32，与标记里存的那个比。
 * 返回 FONTSTORE_OK / ERR_NOTREADY / ERR_CRC / ERR_SPI；*crc_out 填重算值。 */
uint8_t     FontStore_Verify(uint32_t *crc_out);

/* 卸载：擦标记 + 清状态。复位后 `FontStore_Ready()==0` ⇒ 干净退回片内 32 字。
 * （这就是"可以反悔"的证明手段，验收判据 C7 用它。） */
uint8_t     FontStore_Uninstall(void);

#endif /* __FONT_STORE_H */
