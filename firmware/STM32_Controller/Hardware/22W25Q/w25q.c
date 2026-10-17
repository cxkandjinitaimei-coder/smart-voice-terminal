/**
 * @file w25q.c
 * 用途：W25Q SPI NOR 的识别、容量、页写和扇区擦除。
 * 使用：SPI 初始化后先读 JEDEC，再使用有边界检查的读写接口；字库和图片层依赖此驱动。
 * 时序与边界：页写不可跨页，擦除前等空闲；该存储是板外器件，不属于 F407 的 A/B OTA 槽。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "w25q.h"

/* ---------- 模块状态 ---------- */
static uint8_t w25q_id[3];         /* JEDEC ID，失败时全 0 */
static uint32_t w25q_capacity;     /* 由 ID 第 3 字节解出，0 = 未知 */
static volatile uint8_t w25q_lock; /* 重入闸门，见 w25q_lock_enter 注释 */

/* ==================================================================
 * 一、底层：CS / 字节收发 / 等待
 * ================================================================== */

/* ------------------------------------------------------------------
 * 软件片选。high != 0 拉高，否则拉低。
 *  本模块**全程**用这一对，不用 STM32 的硬件 NSS：
 *   SPI1 重映射组的 NSS 是 PA15，而本板 PA15 已按"全走 SWD"算作占用
 *   （`引脚分配表.md §7`），片选只能是普通的 PB0。
 * ------------------------------------------------------------------ */
static void w25q_cs(uint8_t high)
{
    if (high != 0u)
    {
        GPIO_SetBits(W25Q_GPIO_PORT, W25Q_PIN_CS);
    }
    else
    {
        GPIO_ResetBits(W25Q_GPIO_PORT, W25Q_PIN_CS);
    }
}

/* ------------------------------------------------------------------
 * 等发送缓冲空（TXE=1）。返回 W25Q_OK / W25Q_ERR_TIMEOUT。
 * ------------------------------------------------------------------ */
static uint8_t w25q_wait_txe(void)
{
    uint32_t guard = W25Q_TIMEOUT_BYTE;

    while (SPI_I2S_GetFlagStatus(W25Q_SPI, SPI_I2S_FLAG_TXE) == RESET)
    {
        if (--guard == 0u)
        {
            return W25Q_ERR_TIMEOUT;
        }
    }
    return W25Q_OK;
}

/* ------------------------------------------------------------------
 * 等"这一个字节真的移出去了"：TXE=1 **且** BSY=0。
 *
 *  CS 拉高之前必须等这个，**不能只等 TXE** ——
 *    TXE=1 只说明"发送缓冲"空了，移位寄存器里还有最多 8 bit 在飞
 *    （10.5MHz 下就是 762ns）。提前拉高 CS ⇒ 最后一个字节被截断。
 *    最阴的是 `0x06 WriteEnable` 被截断：WEL 没置上 ⇒ 紧接着的页编程
 *    **什么都不写**，读回来还是 0xFF，**全程零报错**。
 *
 * 返回 W25Q_OK / W25Q_ERR_TIMEOUT。
 * ------------------------------------------------------------------ */
static uint8_t w25q_wait_idle(void)
{
    uint32_t guard = W25Q_TIMEOUT_BYTE;
    uint8_t rc = w25q_wait_txe();

    if (rc != W25Q_OK)
    {
        return rc;
    }
    while (SPI_I2S_GetFlagStatus(W25Q_SPI, SPI_I2S_FLAG_BSY) != RESET)
    {
        if (--guard == 0u)
        {
            return W25Q_ERR_TIMEOUT;
        }
    }
    return W25Q_OK;
}

/* ------------------------------------------------------------------
 * 底层唯一的一发一收：**每发出一个字节，都必须把收回的那个读走**。
 *
 *  这里是"整条读通路错位一个字节"的根因，不要把它拆回"只发"+"只收"：
 *   SPI 是全双工的 —— 主机每移出 8 bit，就同时移进 8 bit 并置位 RXNE。
 *   一旦有函数"发完不读 DR"，RXNE 就**一直挂着**；紧跟着的收字节函数
 *   一进 `while(RXNE==RESET)` 就秒过（标志本来就是 1），读回来的是
 *   **上一个字节的残留** ⇒ 从那一刻起**整体错位一个字节**。
 *
 *   上板实锤：`0x9F` 读 ID 打成 **`FF EF 40`**（真相 `EF 40 1x`）。
 *   开头那个 `FF` 就是发 `0x9F` 时 MISO 还是空闲高电平留下的残留；
 *   而**容量字节压根没被读出来**。同一根因下 `0x03` 读数据错位更多
 *   （命令 1 + 地址 3 = 4 个残留）。
 *
 *   ⇒ 所以这里**只留一个收发函数**，tx/rx 全部套在它上面：任何时刻
 *     "已发字节数 == 已收字节数"，DR 永远是空的，
 *     这一类"读到陈旧值 / 整体错位"的错**在结构上不可能再发生**。
 *
 *  RXNE 一定早于 BSY 清零，所以这里先读 DR 不会丢数据；
 *   真正的"发完了"由外面的 w25q_wait_idle() 把关，再拉高 CS。
 * ------------------------------------------------------------------ */
static uint8_t w25q_xfer(uint8_t out, uint8_t *in)
{
    uint32_t guard = W25Q_TIMEOUT_BYTE;
    uint8_t rc = w25q_wait_txe();

    if (rc != W25Q_OK)
    {
        return rc;
    }
    SPI_I2S_SendData(W25Q_SPI, (uint16_t)out); /* 收数据时给 0xFF，值无意义 */

    while (SPI_I2S_GetFlagStatus(W25Q_SPI, SPI_I2S_FLAG_RXNE) == RESET)
    {
        if (--guard == 0u)
        {
            return W25Q_ERR_TIMEOUT;
        }
    }
    /*  这一句不能省 —— 省了就是上面横幅说的那个错位。 */
    {
        uint8_t got = (uint8_t)SPI_I2S_ReceiveData(W25Q_SPI);

        if (in != 0)
        {
            *in = got;
        }
    }
    return W25Q_OK;
}

/* ------------------------------------------------------------------
 * 只发不收（命令字 / 地址 / 页编程数据）。
 *  收到的那个字节**照样读走丢掉** —— 不读就是错位，见 w25q_xfer。
 * 返回 W25Q_OK / W25Q_ERR_TIMEOUT。
 * ------------------------------------------------------------------ */
static uint8_t w25q_tx(uint8_t b)
{
    return w25q_xfer(b, 0);
}

/* ------------------------------------------------------------------
 * 发 0xFF 换回一个字节。返回 W25Q_OK / W25Q_ERR_TIMEOUT / W25Q_ERR_PARAM。
 * ------------------------------------------------------------------ */
static uint8_t w25q_rx(uint8_t *out)
{
    if (out == 0)
    {
        return W25Q_ERR_PARAM;
    }
    return w25q_xfer(0xFFu, out);
}

/* ------------------------------------------------------------------
 * 结束一个命令序列：等干净 → 拉高 CS。
 * 把"等 idle + 拉高"合成一步，免得哪条分支漏了其中一半。
 * ------------------------------------------------------------------ */
static uint8_t w25q_cs_off(void)
{
    uint8_t rc = w25q_wait_idle();

    w25q_cs(1);
    return rc;
}

/* ------------------------------------------------------------------
 * 发 0x06 置写使能（WEL）。返回 W25Q_OK / 错误码。
 *  每个写/擦周期之前都要重新发 —— 器件每完成一次就自动清掉 WEL。
 * ------------------------------------------------------------------ */
static uint8_t w25q_send_write_enable(void)
{
    uint8_t rc;

    w25q_cs(0);
    rc = w25q_tx(W25Q_CMD_WRITE_ENABLE);
    if (rc == W25Q_OK)
    {
        rc = w25q_cs_off();
    }
    else
    {
        w25q_cs(1);
    }
    return rc;
}

/* ------------------------------------------------------------------
 * 读一个状态寄存器字节。cmd 传 W25Q_CMD_READ_SR1 / SR2 / SR3。
 * ------------------------------------------------------------------ */
static uint8_t w25q_read_status_byte(uint8_t cmd, uint8_t *out)
{
    uint8_t rc;

    w25q_cs(0);
    rc = w25q_tx(cmd);
    if (rc == W25Q_OK)
    {
        rc = w25q_rx(out);
    }
    if (rc == W25Q_OK)
    {
        rc = w25q_cs_off();
    }
    else
    {
        w25q_cs(1);
    }
    return rc;
}

/* ------------------------------------------------------------------
 * 轮询 SR1 的 BUSY 位（bit0）直到清零。带超时。
 *  这个超时必须是 uint32_t 且按 64KB 块擦的 800ms 定 ——
 *   见 w25q.h 里 W25Q_TIMEOUT_BUSY 那段，写成 16 位会在擦到一半误判。
 * 返回 W25Q_OK / W25Q_ERR_TIMEOUT。
 * ------------------------------------------------------------------ */
static uint8_t w25q_poll_busy_clear(void)
{
    uint32_t guard = W25Q_TIMEOUT_BUSY;
    uint8_t sr = 0xFFu;
    uint8_t rc;

    for (;;)
    {
        rc = w25q_read_status_byte(W25Q_CMD_READ_SR1, &sr);
        if (rc != W25Q_OK)
        {
            return rc;
        }
        if ((sr & 0x01u) == 0u) /* BUSY = 0 */
        {
            return W25Q_OK;
        }
        if (--guard == 0u)
        {
            return W25Q_ERR_TIMEOUT;
        }
    }
}

/* ------------------------------------------------------------------
 * 地址/长度合法性。返回 W25Q_OK / W25Q_ERR_PARAM。
 * 认出容量之后就按容量卡死 —— 避免地址飘到芯片外面去，
 * 那种情况器件行为是未定义的（回卷/忽略），查起来最费劲。
 * ------------------------------------------------------------------ */
static uint8_t w25q_addr_ok(uint32_t addr, uint32_t len)
{
    if (len == 0u)
    {
        return W25Q_ERR_PARAM;
    }
    if (addr >= W25Q_ADDR_MAX)
    {
        return W25Q_ERR_PARAM;
    }
    if (len > (W25Q_ADDR_MAX - addr))
    {
        return W25Q_ERR_PARAM;
    }
    if ((w25q_capacity != W25Q_CAP_UNKNOWN) && ((addr + len) > w25q_capacity))
    {
        return W25Q_ERR_PARAM;
    }
    return W25Q_OK;
}

/* ==================================================================
 * 二、初始化
 * ================================================================== */

/* ------------------------------------------------------------------
 * 把 SCK/MISO/MOSI 切到 AF5(SPI1)。
 *
 *  三根脚必须**分别**调，绝不能图省事写成
 *        GPIO_PinAFConfig(GPIOB, W25Q_SRC_SCK | W25Q_SRC_MISO | W25Q_SRC_MOSI, W25Q_AF);
 *    因为 F4 的 `GPIO_PinSourceX` 是"引脚下标 0~15"，**不是位掩码**
 *    （`Lib/inc/stm32f4xx_gpio.h` 里是一排 0x00~0x0F）：
 *        3 | 4 | 5 == 7 == GPIO_PinSource7
 *    ⇒ 那一行改的是 **PB7**，而 PB7 = 本板 **PCA9685 的 I2C1_SDA**
 *    ⇒ 症状是：**SPI1 的 PB3/PB4/PB5 一根都没动、还停在 AF0(JTAG)**，
 *      读 ID 永远全 0x00/0xFF；同时 **PCA9685 突然全不动了**。
 *      而且 0x07 既是个合法下标、又能通过 `IS_GPIO_PIN_SOURCE`
 *      断言 ⇒ **零报错、零警告**。
 *     和它形成对照的是：下面的 `GPIO_InitStruct.GPIO_Pin` **才是**位掩码。
 *       同一个函数里两种语义，这就是它坑人的地方。
 *
 *  另外：F4 上**没有** `GPIO_Remap_SWJ_JTAGDisable` / `GPIO_Remap_SPI1`
 *   （本工程 Lib 里根本没有这两个宏，那是 F1 的 AFIO_MAPR 写法）。
 *   PB3/PB4 复位后是 AF0(JTAG)，**只要在这里切到 AF5，它们就自动脱离 JTAG**，
 *   不需要也没有别的动作。代价是 JTAG-DP 从此废掉 ——
 *   这与 `引脚分配表.md §7`「本板 JTAG 已关闭（SWD-only）」一致。
 *    Keil 里 Debug 端口**必须选 SW**：选 JTAG 的话，本函数执行前能连上、
 *      执行后连不上，症状极迷惑。
 * ------------------------------------------------------------------ */
static void w25q_gpio_af_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    GPIO_PinAFConfig(W25Q_GPIO_PORT, W25Q_SRC_SCK, W25Q_AF);
    GPIO_PinAFConfig(W25Q_GPIO_PORT, W25Q_SRC_MISO, W25Q_AF);
    GPIO_PinAFConfig(W25Q_GPIO_PORT, W25Q_SRC_MOSI, W25Q_AF);

    GPIO_InitStruct.GPIO_Pin = W25Q_PIN_SCK | W25Q_PIN_MISO | W25Q_PIN_MOSI; /*  这才是掩码 */
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(W25Q_GPIO_PORT, &GPIO_InitStruct);
}

/* ------------------------------------------------------------------
 * 配 SPI1：模式 0 / 8bit / MSB / 软件 NSS / 预分频见 w25q.h。
 * ------------------------------------------------------------------ */
static void w25q_spi_init(void)
{
    SPI_InitTypeDef SPI_InitStruct;

    /*  先 DeInit。`SPI_Init` 只写 CR1 的一部分（它清的是 SPE/CRCNEXT/CRCEN）
     *   和 I2SCFGR/CRCPR —— **CR2 一个字节都不碰**。将来 bootloader 若先配过
     *   SPI1 并留了 TXEIE/RXNEIE/ERRIE，这里不 DeInit 就会莫名进中断。 */
    SPI_I2S_DeInit(W25Q_SPI);

    SPI_InitStruct.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStruct.SPI_Mode = SPI_Mode_Master; /*  它自带 SSI=1 */
    SPI_InitStruct.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStruct.SPI_CPOL = SPI_CPOL_Low; /* 模式 0 */
    SPI_InitStruct.SPI_CPHA = SPI_CPHA_1Edge;
    SPI_InitStruct.SPI_NSS = SPI_NSS_Soft; /* 片选走 PB0，不占 NSS 脚 */
    SPI_InitStruct.SPI_BaudRatePrescaler = W25Q_PRESCALER;
    SPI_InitStruct.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStruct.SPI_CRCPolynomial = 7; /* 不用 CRC，但得给个合法值 */
    SPI_Init(W25Q_SPI, &SPI_InitStruct);

    /*  `SPI_Init` 把 SPE 清掉了，必须补这一句，否则一个时钟都不出。
     *   注：SSM=1 / SSI=1 已由 `SPI_Mode_Master|SPI_NSS_Soft` 配好，
     *   **不要**再去动 CR2 的 SSOE（那是接 NSS 脚才用的）。 */
    SPI_Cmd(W25Q_SPI, ENABLE);
}

/* ------------------------------------------------------------------
 * 发 0x9F 读 JEDEC ID 三字节。
 * ------------------------------------------------------------------ */
static uint8_t w25q_read_jedec(void)
{
    uint8_t rc;

    w25q_cs(0);
    rc = w25q_tx(W25Q_CMD_JEDEC_ID);
    if (rc == W25Q_OK)
    {
        rc = w25q_rx(&w25q_id[0]);
    }
    if (rc == W25Q_OK)
    {
        rc = w25q_rx(&w25q_id[1]);
    }
    if (rc == W25Q_OK)
    {
        rc = w25q_rx(&w25q_id[2]);
    }
    if (rc == W25Q_OK)
    {
        rc = w25q_cs_off(); /*  拉 CS 前把最后一个字节送完 */
    }
    else
    {
        w25q_cs(1);
    }
    return rc;
}

/* ------------------------------------------------------------------
 * JEDEC ID 第 3 字节 = 容量代码 N，容量 = 2^N 字节：
 *     EF 40 14 = 2^20 = 1MB   (W25Q80)
 *     EF 40 15 = 2^21 = 2MB   (W25Q16)  ← 文档口径是它，但**不写死，实测为准**
 *     EF 40 16 = 2^22 = 4MB   (W25Q32)
 *     EF 40 17 = 2^23 = 8MB   (W25Q64)
 *     EF 40 18 = 2^24 = 16MB  (W25Q128)
 * 只认 0x11~0x18；再大就得用 4 字节地址，本驱动的 24 位地址读不了 ⇒ 返回未知(0)。
 * ------------------------------------------------------------------ */
static uint32_t w25q_decode_capacity(uint8_t code)
{
    if ((code < 0x11u) || (code > 0x18u))
    {
        return W25Q_CAP_UNKNOWN;
    }
    return (uint32_t)1u << code;
}

/* ==================================================================
 * 三、对外接口
 * ================================================================== */

uint8_t W25Q_Init(void)
{
    uint8_t rc;
    GPIO_InitTypeDef GPIO_InitStruct;

    /*  重入闸门：**不是死等**（`while(w25q_lock);` 在中断里调用会死锁），
     *   而是直接返回错误码。本工程 `System/time.c` 的 TIM6 中断每 1ms 触发
     *   一次、还带 `System_TickHook()`；只要那个钩子（或任何别的调用点）
     *   碰了 SPI1 或 PB0，命令序列就被插进来 ⇒ 随机读写错、不报错。 */
    if (w25q_lock != 0u)
    {
        return W25Q_ERR_BUSY;
    }
    w25q_lock = 1u;

    w25q_id[0] = 0u;
    w25q_id[1] = 0u;
    w25q_id[2] = 0u;
    w25q_capacity = W25Q_CAP_UNKNOWN;

    /* ---- 1) 时钟。 GPIOB 在 AHB1、SPI1 在 APB2 —— 别写成一个总线 ---- */
    RCC_AHB1PeriphClockCmd(W25Q_GPIO_CLK, ENABLE);
    RCC_APB2PeriphClockCmd(W25Q_SPI_CLK, ENABLE);

    /* ---- 2) CS：先当普通推挽输出，并**先置高** ----
     * 顺序要紧：复位后 PB0 是浮空输入。先把它顶到高电平，再去切 AF，
     * 免得切 AF 那一瞬间的抖动被器件当成一条命令。
     *  代码只能保证"软件先置高"；从复位到本函数之间 /CS 是真悬空的，
     *    那段时间要稳得靠**板上 CS 加 10k 上拉**（硬件侧的事，记在这备查）。 */
    GPIO_InitStruct.GPIO_Pin = W25Q_PIN_CS;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(W25Q_GPIO_PORT, &GPIO_InitStruct);
    w25q_cs(1);

    /* ---- 3) SCK/MISO/MOSI 切 AF5（ 三次单独调用，理由见函数头） ---- */
    w25q_gpio_af_init();

    /* ---- 4) SPI1 本体 ---- */
    w25q_spi_init();

    /* ---- 5) 回读 JEDEC ID —— **这才是"真通了"的判据** ----
     * 前面几步全是"写寄存器"，写错了也不报错；只有把 ID 读回来才算数。
     * 判据跟 `PCA9685_Init` 回读 MODE1 是同一个思路。 */
    rc = w25q_read_jedec();
    w25q_capacity = w25q_decode_capacity(w25q_id[2]);

    if ((rc == W25Q_OK) && (w25q_id[0] != 0xEFu))
    {
        rc = W25Q_ERR_NOCHIP;
    }

    w25q_lock = 0u;
    return rc;
}

const uint8_t *W25Q_ID(void)
{
    return w25q_id;
}

uint32_t W25Q_CapacityBytes(void)
{
    return w25q_capacity;
}

uint8_t W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t rc;
    uint32_t i;

    if (w25q_lock != 0u)
    {
        return W25Q_ERR_BUSY;
    }
    if (buf == 0)
    {
        return W25Q_ERR_PARAM;
    }
    rc = w25q_addr_ok(addr, len);
    if (rc != W25Q_OK)
    {
        return rc;
    }

    w25q_lock = 1u;

    w25q_cs(0);
    rc = w25q_tx(W25Q_CMD_READ_DATA);
    if (rc == W25Q_OK)
    {
        rc = w25q_tx((uint8_t)(addr >> 16));
    }
    if (rc == W25Q_OK)
    {
        rc = w25q_tx((uint8_t)(addr >> 8));
    }
    if (rc == W25Q_OK)
    {
        rc = w25q_tx((uint8_t)addr);
    }
    for (i = 0u; (i < len) && (rc == W25Q_OK); i++)
    {
        rc = w25q_rx(&buf[i]);
    }
    if (rc == W25Q_OK)
    {
        rc = w25q_cs_off();
    }
    else
    {
        w25q_cs(1);
    }

    w25q_lock = 0u;
    return rc;
}

uint8_t W25Q_PageProgram(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    uint8_t rc;
    uint16_t i;

    if (w25q_lock != 0u)
    {
        return W25Q_ERR_BUSY;
    }
    if (buf == 0)
    {
        return W25Q_ERR_PARAM;
    }
    /*  跨页必须在这里挡死：W25Q 遇到跨页是**回卷到本页页首覆盖**，
     *   不报错、不置标志，事后完全看不出是谁写坏的。别指望调用方自觉。 */
    if (((uint32_t)(addr & 0xFFu) + (uint32_t)len) > W25Q_PAGE_SIZE)
    {
        return W25Q_ERR_PARAM;
    }
    rc = w25q_addr_ok(addr, (uint32_t)len);
    if (rc != W25Q_OK)
    {
        return rc;
    }

    w25q_lock = 1u;

    rc = w25q_send_write_enable();
    if (rc == W25Q_OK)
    {
        w25q_cs(0);
        rc = w25q_tx(W25Q_CMD_PAGE_PROGRAM);
        if (rc == W25Q_OK)
        {
            rc = w25q_tx((uint8_t)(addr >> 16));
        }
        if (rc == W25Q_OK)
        {
            rc = w25q_tx((uint8_t)(addr >> 8));
        }
        if (rc == W25Q_OK)
        {
            rc = w25q_tx((uint8_t)addr);
        }
        for (i = 0u; (i < len) && (rc == W25Q_OK); i++)
        {
            rc = w25q_tx(buf[i]);
        }
        if (rc == W25Q_OK)
        {
            rc = w25q_cs_off();
        }
        else
        {
            w25q_cs(1);
        }
    }

    /*  写完必须等这一页真正落进 Flash（最长 3ms）再返回。
     *   不等就返回，调用方会立刻发下一条命令 —— 那些命令**会被器件丢弃**，
     *   而且没有"忙"以外的任何迹象。 */
    if (rc == W25Q_OK)
    {
        rc = w25q_poll_busy_clear();
    }

    w25q_lock = 0u;
    return rc;
}

/* ------------------------------------------------------------------
 * 擦除的公共部分：0x20(4KB 扇区) 与 0xD8(64KB 块) 只差一个命令字节。
 * ------------------------------------------------------------------ */
static uint8_t w25q_erase(uint8_t cmd, uint32_t addr)
{
    uint8_t rc;

    if (w25q_lock != 0u)
    {
        return W25Q_ERR_BUSY;
    }
    if (addr >= W25Q_ADDR_MAX)
    {
        return W25Q_ERR_PARAM;
    }
    if ((w25q_capacity != W25Q_CAP_UNKNOWN) && (addr >= w25q_capacity))
    {
        return W25Q_ERR_PARAM;
    }

    w25q_lock = 1u;

    rc = w25q_send_write_enable();
    if (rc == W25Q_OK)
    {
        w25q_cs(0);
        rc = w25q_tx(cmd);
        if (rc == W25Q_OK)
        {
            rc = w25q_tx((uint8_t)(addr >> 16));
        }
        if (rc == W25Q_OK)
        {
            rc = w25q_tx((uint8_t)(addr >> 8));
        }
        if (rc == W25Q_OK)
        {
            rc = w25q_tx((uint8_t)addr);
        }
        if (rc == W25Q_OK)
        {
            rc = w25q_cs_off();
        }
        else
        {
            w25q_cs(1);
        }
    }

    /*  擦完要等 BUSY 清零。4KB 扇区最长 400ms、64KB 块最长 800ms，
     *   所以超时哨兵是按 800ms 定的（见 w25q.h）。 */
    if (rc == W25Q_OK)
    {
        rc = w25q_poll_busy_clear();
    }

    w25q_lock = 0u;
    return rc;
}

uint8_t W25Q_SectorErase(uint32_t addr)
{
    return w25q_erase(W25Q_CMD_SECTOR_ERASE, addr);
}

uint8_t W25Q_BlockErase(uint32_t addr)
{
    return w25q_erase(W25Q_CMD_BLOCK_ERASE, addr);
}

uint8_t W25Q_ReadStatus(uint8_t which, uint8_t *out)
{
    uint8_t rc;

    if (w25q_lock != 0u)
    {
        return W25Q_ERR_BUSY;
    }
    if (out == 0)
    {
        return W25Q_ERR_PARAM;
    }
    if ((which < 1u) || (which > 3u))
    {
        return W25Q_ERR_PARAM;
    }

    w25q_lock = 1u;
    rc = w25q_read_status_byte((which == 1u)   ? W25Q_CMD_READ_SR1
                               : (which == 2u) ? W25Q_CMD_READ_SR2
                                               : W25Q_CMD_READ_SR3,
                               out);
    w25q_lock = 0u;
    return rc;
}

uint8_t W25Q_WaitBusy(void)
{
    uint8_t rc;

    if (w25q_lock != 0u)
    {
        return W25Q_ERR_BUSY;
    }

    w25q_lock = 1u;
    rc = w25q_poll_busy_clear();
    w25q_lock = 0u;
    return rc;
}

uint8_t W25Q_WriteEnable(void)
{
    uint8_t rc;

    if (w25q_lock != 0u)
    {
        return W25Q_ERR_BUSY;
    }

    w25q_lock = 1u;
    rc = w25q_send_write_enable();
    w25q_lock = 0u;
    return rc;
}
