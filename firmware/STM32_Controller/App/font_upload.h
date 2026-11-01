/* ==================================================================
 * font_upload.h —— 字库的**串口传输层**（`/font` 一族命令）
 * ------------------------------------------------------------------
 * 【和 main.c 的接法】必须排在 `Media_Command` **之前**：
 *   font_handled = FontUpload_Command(rx_line);      // 自己的标志，见下
 *   if(!font_handled && FontUpload_Active() && rx_line[0] == '/')  拒掉（不排队）
 *   if(!iap_handled && !font_handled && !MicRec_Command(rx_line)) ...
 *   ⚠️ 顺序反了就被 Media_Command 吃掉 —— 它对任何不认识的 "/xxx"
 *      都会回一句 "Unknown command" 并**整行吞掉**（media.c 的兜底）。
 *   ★ 2026-09-21：标志是**自己的** `font_handled`，**不再借 02 的 `iap_handled`**
 *     （共用判据 = 任一边改语义、另一边静默跟着变）。同时 `FontUpload_Active()`
 *     被当成 **busy 闸**用：会话中途别的 `/` 命令一律拒（不排队、不执行），
 *     **`/font abort` 例外**（走正常入口），所以这道闸锁不死人。
 *
 * 【口径】返回 1 = 是我的命令（已处理、已回包）；0 = 不是，交回调用者。
 *   与 `PCA9685_Command` 一致。
 *
 * 【为什么要 hex 文本，不用裸二进制】
 *   USART2 的行解析器（`Hardware/06USART/uart_rx.c`）见 `\0` **整行作废**、
 *   行长上限 255、硬件错误丢弃整行 ⇒ 裸二进制**必然**被打碎。
 *   本项目自己的先例就是 hex（`micrec.c` 的 `[DUMP]`），而且
 *   `问题日志.md` 明写过"**别再为了省一半传输时间改回原始字节**"。
 *
 * 【协议】115200 8N1，一行一条命令，行尾 `\n`：
 *
 *   /font begin <len> <crc32> <hdr32>   → 校参数 + 校镜像头 → 擦旧标记 → 擦镜像区
 *                                         回复 OK          / ERR <code>
 *   /font d <off> <hex>                 → 写一块（112 字节/行）
 *                                         回复 OK <下一个期望 off>
 *                                            / SEQ <下一个期望 off>
 *                                            / ERR <code> <fail_off>
 *   /font end                           → 回读整镜像算 CRC32 → 比对 → 写完成标记
 *                                         回复 CRC ok / CRC bad <读到的>
 *   /font abort                         → 清会话（**不动 flash**）
 *                                         回复 OK
 *   /font verify                        → 只回读算 CRC32（不写）
 *                                         回复 CRC <值>
 *   /font                               → 状态一行
 *   /font ?                             → 用法
 *
 *   数字：`len` 十进制；`crc32` / `off` 带 `0x` 前缀的十六进制。
 *
 * 【★ 两处比方案原文收紧的地方（都是为了"坏文件绝不能毁掉现有字库"）】
 *   ① `begin` **多带一个 `<hdr32>` = 镜像头那 16 字节的 hex（32 个字符）**。
 *      方案原文只有 `<len> <crc32>`，可镜像头是**跟着数据流**来的 ⇒ 那时
 *      根本没法判断"这个文件合不合法"，只能先擦了再说 —— 与
 *      "先解析源头部、坏文件绝不能先毁掉现有字库"的要求直接冲突。
 *      把头部提前到 begin 里，校验就能发生在**擦任何东西之前**。
 *   ② `begin` 在擦完之后会调一次 `FontStore_Init()`：此时标记已经是 0xFF
 *      ⇒ 它必然返回 `NO_MARK` ⇒ **RAM 里那份"旧字库可用"的状态被就地清掉**。
 *      不做这一步的话，`begin` 已经把镜像擦了一半，而正在跑的系统还以为
 *      旧字库好好的，查字会从**半擦的镜像**里取字模 ⇒ 屏上出现莫名其妙的
 *      字或方块，且不报错。
 *
 * 【★ 两个真·阻塞窗口，别把它们放进会超时的时序里】
 *   · `begin` 的擦除：典型 0.3s，最坏按 4s 留余量（块擦 800ms × 2，再留一倍）
 *   · `end`   的回读 + CRC：约 0.2s
 *   单个 `/font d` 只做 112 字节编程 + 回读（约 1ms）⇒ **主循环不是被占住
 *   30 秒**，只有上面两处会真的卡住。
 * ================================================================== */
#ifndef __FONT_UPLOAD_H
#define __FONT_UPLOAD_H

#include "stm32f4xx.h"

/* 命令入口。返回 1 = 是我的命令（已处理）；0 = 不是，交回调用者。 */
uint8_t FontUpload_Command(const char *line);

/* 1 = 有一笔上传正在进行（begin 之后、end/abort 之前）。给别的模块做互斥用。 */
uint8_t FontUpload_Active(void);

#endif /* __FONT_UPLOAD_H */
