/**
 * @file micrec.c
 * 用途：短录音的 RAM 缓存、调试命令和十六进制导出。
 * 使用：INMP441 就绪后由 /rec 或应用调用，/dump 输出给 dump2wav 工具。
 * 时序与边界：这是兼容的阻塞调试路径，长录音走 micaud；115200 下导出耗时明显长于录制本身。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "micrec.h"
#include "inmp441.h" /* INMP441_Blocks / INMP441_Read、MIC_SAMPLE_RATE、MIC_HALF_SAMPLES */
#include "uart.h"    /* send_data —— 吐原始字节走它（printf 只能发文本） */
#include "time.h"    /* GetTick_ms */
#include <stdio.h>
#include <string.h>

/* ==================================================================
 * 录音（RAM 版）实现  * ------------------------------------------------------------------
 * 数据流：
 *   INMP441 --DMA--> g_dma_buf[1024]（64ms 环形，别人的）
 *     --(INMP441_Read 隔一个取一个)--> s_buf[32000]（64KB，本模块的）
 *     --(send_data 限速吐)--> USART2 --> PC 上的 dump2wav.py --> .wav
 *
 *  为什么不用"主循环里轮询着攒"那种非阻塞写法：
 *   麦克风的 DMA 环形缓冲**只有 64ms**（1024 半字），而主循环里刷一次
 *   状态页要几毫秒、printf 要排队、每 5 秒还要读一次 SD 扇区 —— 只要
 *   任意一次轮询间隔超过 64ms，中间那几块就被 DMA 覆盖了。
 *   **丢包不报错**，只在音频里留一点破音/杂音，事后极难查。
 *   ⇒ 录音这 1~2 秒干脆整段阻塞，换"一个样本都不丢"。测试工具，值。
 *
 *  为什么单片机**不**去直流、不归一化：
 *   s_buf 里保持的是**原始样本**。去直流/放大这类 DSP 全部留给 PC 端
 *   （`Tools/micrec/dump2wav.py`）—— 那边有浮点、没有内存限制，也不会
 *   因为"减掉 -3355 的直流后峰值超出 int16 量程"而削顶。
 *   单片机只**统计并打印**直流/峰峰值，不动数据。
 * ================================================================== */

/* 缓冲容量：16000Hz × 最多 2 秒 = 32000 个 int16 = 64KB。
 * static ⇒ 不进栈（栈只有 2KB，放栈上必炸）。 */
#define MICREC_SAMPLES_MAX (MIC_SAMPLE_RATE * MICREC_SEC_MAX)

/* 分段统计的段数 */
#define MICREC_ENV_SEG 8u

/* /dump 每行吐多少个十六进制字符（= 32 个 PCM 字节） */
#define MICREC_HEX_COLS 64u

/* 十六进制表（dump 用文本而不是原始字节，理由见 micrec.h） */
static const char MICREC_HEX[] = "0123456789ABCDEF";

static int16_t s_buf[MICREC_SAMPLES_MAX]; /* 录音缓冲（单声道 16kHz PCM） */
static uint32_t s_count = 0u;             /* 已录样本数 */
static uint32_t s_lost = 0u;              /* 上次录音丢掉的半块数（正常应为 0） */
static int32_t s_dc = 0;                  /* 上次录音的直流均值 */
static int32_t s_pp = 0;                  /* 上次录音的峰峰值 */

/* ==================================================================
 * 录音（阻塞）
 * ------------------------------------------------------------------
 * 只从"已完成的半块"取数（INMP441_Read 保证），所以不会拿到写了一半的
 * 数据。用 INMP441_Blocks() 的**变化**当节拍：它一变就说明有新的整块好了。
 * ================================================================== */
static void mrec_run(uint16_t seconds)
{
    uint32_t t0, target_ms, target, last_blocks, i;
    int32_t acc, mn, mx;

    if (seconds < 1u)
    {
        seconds = 1u;
    }
    if (seconds > MICREC_SEC_MAX)
    {
        seconds = MICREC_SEC_MAX;
    }

    s_count = 0u;
    s_lost = 0u;
    s_dc = 0;
    s_pp = 0;

    printf("[REC] 开始录 %u 秒（16kHz/16bit/单声道）—— 现在说话！\r\n", (unsigned)seconds);

    target = (uint32_t)seconds * (uint32_t)MIC_SAMPLE_RATE;
    target_ms = (uint32_t)seconds * 1000u;
    t0 = GetTick_ms();
    last_blocks = INMP441_Blocks();

    /*  这段是**阻塞**的，别改成"每轮返回一次"的协作式写法，理由见文件头。
     *    这 1~2 秒里不刷屏、不 printf、不读卡 —— 屏上秒表和 K-UP 都停着。 */
    while (((GetTick_ms() - t0) < target_ms) && (s_count < target))
    {
        uint32_t b = INMP441_Blocks();

        if (b != last_blocks)
        {
            uint32_t d = b - last_blocks;
            uint16_t got;

            last_blocks = b;

            /* 差值 >1 ⇒ 上次之后又好了 d 个半块，但我们只拿得到**最后那个**
             * ⇒ 前面 d-1 个已经被 DMA 覆盖了。紧凑循环里 d 应该恒为 1；
             * 一旦不是，就说明有东西卡住了主循环 —— 记下来让人看见。 */
            if (d > 1u)
            {
                s_lost += (d - 1u);
            }

            /* INMP441_Read 内部自己会夹到半个半块（512 个），这里不用再夹 */
            got = INMP441_Read(&s_buf[s_count], (uint16_t)(MICREC_SAMPLES_MAX - s_count));
            s_count += (uint32_t)got;
        }
        /* 没有新半块时在这里空转 —— 半块 32ms 才来一个，空转是正常的。
         * 不塞延时：延时会让我们错过下一块的边缘。 */
    }

    if (s_count == 0u)
    {
        printf("[REC] ★ 一个样本都没收到 —— 麦没接 / 没供上 3.3V / I2S 没跑起来。\r\n");
        printf("[REC]   看上面 [MIC] 那几行：Diag 会把\"SD 线没接\"和\"时钟没跑\"分开报\r\n");
        return;
    }

    /* ---- 统计（只读，不改数据）---- */
    acc = 0;
    mn = 32767;
    mx = -32768;
    for (i = 0u; i < s_count; i++)
    {
        int32_t v = (int32_t)s_buf[i];

        acc += v;
        if (v < mn)
        {
            mn = v;
        }
        if (v > mx)
        {
            mx = v;
        }
    }
    s_dc = acc / (int32_t)s_count;
    s_pp = mx - mn;

    printf("[REC] 完成：%lu 样本 = %lu ms，丢块 %lu，直流 %ld，峰峰值 %ld\r\n",
           (unsigned long)s_count, (unsigned long)(s_count / 16u), (unsigned long)s_lost,
           (long)s_dc, (long)s_pp);

    /* ---- 分段峰峰值：把录音等分 8 段，各段打一个数 ----
     * 为什么值得多这几行：录音是"敲回车之后才开始"的，很容易出现
     * "说到一半才想起发命令"或者"话说完了命令才发出去"。光看一个总
     * 峰峰值，这两种情况和"麦根本没拾到音"长得一模一样；分段之后
     * "第几段有声音"一眼可见，不用先去听 wav 就能判断时序对不对。 */
    {
        uint32_t seglen = (s_count + (MICREC_ENV_SEG - 1u)) / MICREC_ENV_SEG;
        uint16_t k;

        printf("[REC] 分段峰峰（每段约 %lums，从左到右是时间先后）：",
               (unsigned long)(seglen / 16u));

        for (k = 0u; k < (uint16_t)MICREC_ENV_SEG; k++)
        {
            uint32_t a = (uint32_t)k * seglen;
            uint32_t e;
            uint32_t j;
            int32_t smn = 32767;
            int32_t smx = -32768;

            if (a >= s_count)
            {
                printf(" -");
                continue;
            }

            e = a + seglen;
            if (e > s_count)
            {
                e = s_count;
            }

            for (j = a; j < e; j++)
            {
                int32_t v = (int32_t)s_buf[j];

                if (v < smn)
                {
                    smn = v;
                }
                if (v > smx)
                {
                    smx = v;
                }
            }
            printf(" %ld", (long)(smx - smn));
        }
        printf("\r\n");
    }

    printf("[REC] 想听就发 /dump（吐 %lu 个十六进制字符，115200 下约 %lu 秒），"
           "PC 端用 Tools\\micrec\\dump2wav.py 收\r\n",
           (unsigned long)(s_count * 4u), (unsigned long)((s_count * 4u) / 11520u + 1u));
}

/* ==================================================================
 * dump：把最近一次录音以**十六进制文本**吐到串口
 * ------------------------------------------------------------------
 * 线上格式（PC 端按这个解析，改这里必须同步改 dump2wav.py）：
 *   [DUMP] BEGIN <样本数> <PCM字节数>\r\n
 *   <2×PCM字节数 个十六进制字符，每行 64 个，行尾 \r\n>
 *   [DUMP] END <耗时>ms\r\n
 *
 *  为什么吐文本而不是原始字节（第一版就是原始字节，已改）：
 *   原始 PCM 里**任何字节都可能出现**，包括 '\r' '\n' 和 "[DUMP] END"
 *   这几个字符本身。串口助手"保存接收数据到文件"如果走的是文本模式，
 *   会把 \r\n 翻译成 \n —— 字节流被改过，音频直接变成一片噪声，而且
 *   **从表面看不出来**。全是可打印 ASCII 就没有这个问题：怎么存都不变样，
 *   还能直接在终端里看。代价是传输量翻倍，测试够用。
 *  先给长度仍然有价值：PC 端"数够 2×字节数 个十六进制字符就停"，
 *   不用去猜哪里是结尾，也不怕中间夹了日志。
 * ================================================================== */
static void mrec_dump(void)
{
    const uint8_t *p = (const uint8_t *)s_buf;
    char line[MICREC_HEX_COLS + 4u];
    uint32_t bytes, i, t0;
    uint16_t n;

    if (s_count == 0u)
    {
        printf("[DUMP] 还没录过东西 —— 先发 /rec（或 /rec 2）\r\n");
        return;
    }

    bytes = s_count * 2u;

    printf("[DUMP] BEGIN %lu %lu\r\n", (unsigned long)s_count, (unsigned long)bytes);

    t0 = GetTick_ms();
    n = 0u;

    /* 攒满一行再 send_data：send_data 是"写发送环形缓冲 + DMA 搬走"，
     * 缓冲满时它**自己会等** DMA 腾空间 ⇒ 天然限速，不会丢字节。
     * 115200-8N1 大约 11520 B/s，一行 66 字节 ≈ 5.7ms。 */
    for (i = 0u; i < bytes; i++)
    {
        uint8_t b = p[i];

        line[n] = MICREC_HEX[(b >> 4) & 0x0Fu];
        n++;
        line[n] = MICREC_HEX[b & 0x0Fu];
        n++;

        if (n >= MICREC_HEX_COLS)
        {
            line[n] = '\r';
            n++;
            line[n] = '\n';
            n++;
            send_data(line, n);
            n = 0u;
        }
    }
    if (n != 0u)
    {
        line[n] = '\r';
        n++;
        line[n] = '\n';
        n++;
        send_data(line, n);
    }

    printf("[DUMP] END %lums\r\n", (unsigned long)(GetTick_ms() - t0));
}

/* ==================================================================
 * 命令分发
 * ================================================================== */
uint8_t MicRec_Command(const char *line)
{
    const char *arg;

    if (line == 0)
    {
        return 0u;
    }

    /* ---- /rec —— 不带参数 = 1 秒 ---- */
    if (!strcmp(line, "/rec"))
    {
        mrec_run(1u);
        return 1u;
    }

    /* ---- /rec n ---- */
    if (!strncmp(line, "/rec ", 5))
    {
        uint16_t v = 0u;

        arg = line + 5;
        while ((*arg >= '0') && (*arg <= '9'))
        {
            v = (uint16_t)(v * 10u + (uint16_t)(*arg - '0'));
            if (v > 999u)
            {
                v = 999u;
            } /* 防越积越多把 uint16 绕回去 */
            arg++;
        }

        if (v == 0u)
        {
            printf("[REC] 用法：/rec [1..%u]（不带参数 = 1 秒）\r\n", (unsigned)MICREC_SEC_MAX);
        }
        else
        {
            mrec_run(v); /* 超出范围由 mrec_run 夹住 */
        }
        return 1u;
    }

    /* ---- /dump ---- */
    if (!strcmp(line, "/dump"))
    {
        mrec_dump();
        return 1u;
    }

    /* ---- /mic ---- */
    if (!strcmp(line, "/mic"))
    {
        printf("[REC] 缓冲 %lu/%lu 样本（%lu ms），上次丢块 %lu，"
               "直流 %ld 峰峰值 %ld\r\n",
               (unsigned long)s_count, (unsigned long)MICREC_SAMPLES_MAX,
               (unsigned long)(s_count / 16u), (unsigned long)s_lost, (long)s_dc, (long)s_pp);
        return 1u;
    }

    return 0u; /* 不是本模块的命令：交回调用者（Media_Command） */
}
