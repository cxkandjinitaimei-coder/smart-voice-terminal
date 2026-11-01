/* ============================================================================
 * boot_main.c —— AIBootloader（第 4 章）
 *
 * 它**只做四件事**：读元数据 → 验 CRC → 把 Slot B 搬进 Slot A → 跳过去。
 *
 * ★★ 为什么故意做得这么小 / 不用 printf / 不用中断：
 *    bootloader 是**唯一没有备份的那段代码**（它在 Sector 0，没人能升级它，
 *    也没人能救它）。它每多依赖一样东西，就多一个"整板变砖"的理由。
 *    所以这里串口是自己写的轮询版（30 行），**不引 stdio、不引 uart.c、不开中断**。
 *
 * ★★ 分工（别搞混）：
 *    **收固件是 App 的活**（App 里的 iap.c 把新固件写进 Slot B）。
 *    **bootloader 只负责搬** B→A。所以 bootloader 里**没有 ESP、没有 UART 协议**。
 *
 * 启动流程：
 *    ① 元数据无效/空白  → 直接跳 Slot A（**不校验**）
 *         理由：ST-Link 刚烧完的板子就是这种状态，此时最需要它能起来。
 *              恢复手段也是"擦掉元数据扇区"（IAP_MetaEraseAll）。
 *    ② state == READY   → 验 Slot B 的 CRC，通过则擦 A、搬过去、回读校验、置 CONFIRMED
 *    ③ state == CONFIRMED → 验 Slot A 的 CRC，通过则跳
 *    ④ 任何一步失败 → **停在 bootloader**，串口报出原因（**不盲目重试、不跳坏图**）
 *
 * ⚠️ 本文件**不能**塞进 MyAIMachines.uvprojx —— 它链接在 0x08000000，
 *    而 App 链接在 0x08008000，两者是不同的工程（见 合作表 §2.1 ①）。
 * ========================================================================= */
#include "stm32f4xx.h"
#include "iap_flash.h"
#include "iap_meta.h"
#include "iap_image.h"

/* B→A 搬运的块大小。取 1 KB：够大而不会一次吃太多栈。
 * ★ 这里的缓冲是**栈上数组**，而 bootloader 的栈顶来自 startup 的 Stack_Size。 */
#define BOOT_COPY_CHUNK     1024u

/* 串口发送的忙等上限。★ 必须有：串口那根线没接、或者对手端没就绪时，
 * 无上限的 while 会把 bootloader 永久卡死 —— 那就等于变砖。 */
#define BOOT_TX_GUARD       100000u

/* ==========================================================================
 * 极简串口：PA2(TX) / PA3(RX)，USART2，115200-8N1，纯轮询
 * 与 App 的调试口（Hardware/06USART/uart.c，UART_BAUD=115200）是**同一个口**，
 * 所以同一个串口终端能直接看到 bootloader 和 App 两段日志。
 * ========================================================================= */
static void boot_uart_init(void)
{
    GPIO_InitTypeDef  gpio;
    USART_InitTypeDef us;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = GPIO_Pin_2 | GPIO_Pin_3;
    gpio.GPIO_Mode  = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd  = GPIO_PuPd_UP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    USART_StructInit(&us);
    us.USART_BaudRate = 115200u;
    USART_Init(USART2, &us);
    USART_Cmd(USART2, ENABLE);
}

static void boot_putc(char c)
{
    uint32_t guard = 0u;

    while ((USART2->SR & USART_FLAG_TXE) == 0u)
    {
        guard++;
        if (guard > BOOT_TX_GUARD)
        {
            return;         /* 发不出去就算了，绝不能因此卡住启动 */
        }
    }
    USART2->DR = (uint16_t)(uint8_t)c;
}

static void boot_puts(const char *s)
{
    while (*s != '\0')
    {
        if (*s == '\n')
        {
            boot_putc('\r');
        }
        boot_putc(*s);
        s++;
    }
}

static void boot_puthex(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    int32_t i;

    boot_puts("0x");
    for (i = 28; i >= 0; i -= 4)
    {
        boot_putc(hex[(v >> i) & 0x0Fu]);
    }
}

static void boot_putu(uint32_t v)
{
    char     buf[12];
    uint32_t n = 0u;

    if (v == 0u)
    {
        boot_putc('0');
        return;
    }
    while ((v > 0u) && (n < sizeof(buf)))
    {
        buf[n] = (char)('0' + (v % 10u));
        v /= 10u;
        n++;
    }
    while (n > 0u)
    {
        n--;
        boot_putc(buf[n]);
    }
}

/* ==========================================================================
 * 镜像可用性检查 —— ★ 这是**防砖的第一道闸**
 *
 * 光看"我要不要跳"不够：如果 Slot A 是**空的**（全 0xFF）或者被写坏了，
 * 无条件跳过去会直接跑飞（栈指针 0xFFFFFFFF ⇒ 立刻 HardFault）。
 * 所以跳之前先按 ARM 的约定读两个向量并做**范围检查**：
 *   [0] = 初始 MSP，必须在 SRAM 里
 *   [4] = 复位向量，必须落在本槽内，且 bit0 = 1（Thumb）
 * ========================================================================= */
static uint8_t boot_image_ok(uint32_t base, uint32_t size)
{
    uint32_t sp = *(volatile uint32_t *)(base + 0u);
    uint32_t pc = *(volatile uint32_t *)(base + 4u);

    return IAP_ImageVectorsOK(sp, pc, size);
}

/* ==========================================================================
 * 跳转
 * ========================================================================= */
/* ARMCC assembly: no C locals or stack accesses after replacing MSP. */
__asm static void boot_enter(uint32_t sp, uint32_t pc)
{
    MSR MSP, r0
    MOVS r2, #0
    MSR CONTROL, r2
    MSR BASEPRI, r2
    MSR FAULTMASK, r2
    DSB
    ISB
    CPSIE I
    BX r1
}

static void boot_jump(uint32_t base)
{
    uint32_t sp = *(volatile uint32_t *)(base + 0u);
    uint32_t pc = *(volatile uint32_t *)(base + 4u);
    uint32_t i;

    boot_puts("[BOOT] 跳转 -> ");
    boot_puthex(base);
    boot_puts("  MSP=");
    boot_puthex(sp);
    boot_puts("  PC=");
    boot_puthex(pc);
    boot_puts("\n");
    /* Finish the last stop bit before App changes USART2/clocks. */
    for (i = 0u; i < BOOT_TX_GUARD && !(USART2->SR & USART_SR_TC); i++) {}

    /* ---- 关掉一切，别把 bootloader 的残留状态带进 App ---- */
    __disable_irq();
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL  = 0u;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    for (i = 0u; i < 8u; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }

    /* ---- 缓存：Slot A 刚被擦/写过，缓存里可能有旧行 ---- */
    if ((FLASH->ACR & FLASH_ACR_ICEN) != 0u)
    {
        FLASH_InstructionCacheCmd(DISABLE);
        FLASH_InstructionCacheReset();
        FLASH_InstructionCacheCmd(ENABLE); //   /* 本工程 StdPeriph 里的真名，不是 FLASH_ICacheReset */
    }
    if ((FLASH->ACR & FLASH_ACR_DCEN) != 0u)
    {
        FLASH_DataCacheCmd(DISABLE);
        FLASH_DataCacheReset();
        FLASH_DataCacheCmd(ENABLE);
    }

    /* ---- 设置向量表和栈，然后跳 ----
     * ★ 注意 App 自己的 SystemInit() 会**再把 VTOR 设一次**
     *   （User/system_stm32f4xx.c 的 VECT_TAB_OFFSET）——
     *   所以那边必须跟着改成 0x8000，两处指的是同一个地址。
     *   详见 规划部分/IAP布局.md §4。 */
    SCB->VTOR = base;
    boot_enter(sp, pc);

    for (;;)
    {
        /* 正常永远到不了这里 */
    }
}

/* ==========================================================================
 * 把 Slot B 搬到 Slot A
 * ========================================================================= */
static uint8_t boot_commit(const iap_meta_t *m)
{
    static uint8_t chunk[BOOT_COPY_CHUNK];      /* static：不在栈上占 1 KB */
    uint32_t       off;
    uint32_t       s;
    uint8_t        rc;

    boot_puts("[BOOT] 待提交: size=");
    boot_putu(m->fw_size);
    boot_puts("  crc=");
    boot_puthex(m->fw_crc);
    boot_puts("\n");

    /* ---- ① 体积闸：★ 必须在下发阶段就拒，这里再挡一道 ----
     * Slot B(256KB) > Slot A(224KB)，B→A 是逐字节拷贝 ⇒ 超过 224KB 必然溢出。
     * 见 iap_flash.h 里 IAP_FW_SIZE_MAX 的说明。 */
    if ((m->fw_size == 0u) || (m->fw_size > IAP_FW_SIZE_MAX))
    {
        boot_puts("[BOOT] !! size 越界（上限 ");
        boot_putu(IAP_FW_SIZE_MAX);
        boot_puts("）—— 拒绝搬运\n");
        return 0u;
    }

    /* ---- ② 先验 Slot B，**验不过绝不擦 Slot A** ---- */
    boot_puts("[BOOT] 校验 Slot B ...\n");
    if (IAP_Crc32Flash(IAP_SLOT_B_BASE, m->fw_size) != m->fw_crc)
    {
        boot_puts("[BOOT] !! Slot B CRC 不符 —— 拒绝搬运（Slot A 完好，未动）\n");
        return 0u;
    }
    if (!boot_image_ok(IAP_SLOT_B_BASE, m->fw_size))
    { boot_puts("[BOOT] invalid Slot-A vectors in staging image\n"); return 0u; }
    boot_puts("[BOOT] Slot B 校验通过\n");

    /* ---- ③ 擦 Slot A（4 个扇区：16+16+64+128）。这一步 1~4 秒，CPU 全程被硬件拉停 ---- */
    boot_puts("[BOOT] 擦除 Slot A ...（1~4 秒，期间串口无输出是正常的）\n");
    for (s = 0u; s < IAP_SLOT_A_SECTORS; s++)
    {
        if (IAP_EraseSector(IAP_SLOT_A_FIRST_SECTOR + s) != IAP_OK)
        {
            boot_puts("[BOOT] !! 擦除失败 sector=");
            boot_putu(IAP_SLOT_A_FIRST_SECTOR + s);
            boot_puts("\n");
            return 0u;
        }
    }

    /* ---- ④ 逐块搬 ---- */
    boot_puts("[BOOT] 搬运 B -> A ...\n");
    for (off = 0u; off < m->fw_size; off += BOOT_COPY_CHUNK)
    {
        uint32_t n = m->fw_size - off;
        if (n > BOOT_COPY_CHUNK)
        {
            n = BOOT_COPY_CHUNK;
        }

        /* 源在 Slot B、目标在 Slot A，**都是 Flash**。
         * ★ 读源之前那次写已经完成（IAP_ProgramWords 内部等到了 COMPLETE），
         *   所以不存在"边写边读同一块 Flash"的问题。 */
        {
            const uint8_t *src = (const uint8_t *)(IAP_SLOT_B_BASE + off);
            uint32_t       k;
            for (k = 0u; k < n; k++)
            {
                chunk[k] = src[k];
            }
        }

        rc = IAP_ProgramWords(IAP_SLOT_A_BASE + off, chunk, n);
        if (rc != IAP_OK)
        {
            boot_puts("[BOOT] !! 写入失败 off=");
            boot_putu(off);
            boot_puts(" rc=");
            boot_putu((uint32_t)rc);
            boot_puts("\n");
            return 0u;
        }
    }

    /* ---- ⑤ 回读校验：搬完必须再算一遍 A，不能只信"写成功了" ---- */
    boot_puts("[BOOT] 回读校验 Slot A ...\n");
    if (IAP_Crc32Flash(IAP_SLOT_A_BASE, m->fw_size) != m->fw_crc)
    {
        boot_puts("[BOOT] !! Slot A 回读 CRC 不符 —— 搬运失败\n");
        return 0u;
    }

    boot_puts("[BOOT] 搬运完成，Slot A 与 Slot B 一致\n");
    return 1u;
}

/* ==========================================================================
 * main
 * ========================================================================= */
/* Absolute read-only descriptor is present in Keil's native HEX/BIN. */
__attribute__((section("BOOT_LAYOUT"), used))
const uint32_t boot_layout_descriptor[8] = {
    IAP_BOOT_DESCRIPTOR_MAGIC, IAP_LAYOUT_ID, IAP_SLOT_A_BASE, IAP_SLOT_A_SIZE,
    IAP_SLOT_B_BASE, IAP_SLOT_B_SIZE, IAP_BOOT_SIZE, IAP_BOOT_DESCRIPTOR_CRC
};

int main(void)
{
    iap_meta_t meta;
    uint8_t    have_meta;

    /* Reset_Handler already called SystemInit before initializing C runtime. */
    boot_uart_init();

    boot_puts("\n\n[BOOT] ==== AIBootloader EXPAND_A224_B256_R1 ====\n");
    boot_puts("[BOOT] App 槽 A=");
    boot_puthex(IAP_SLOT_A_BASE);
    boot_puts(" 下载槽 B=");
    boot_puthex(IAP_SLOT_B_BASE);
    boot_puts("\n");

    /* ★ 开局自检：确认扇区表算出来的槽地址和编译期常量对得上。
     * 万一以后有人改了扇区表，这里会立刻报出来，而不是擦错地方。 */
    if ((IAP_SectorOf(IAP_SLOT_A_BASE) != IAP_SLOT_A_FIRST_SECTOR) ||
        (IAP_SectorOf(IAP_SLOT_B_BASE) != IAP_SLOT_B_FIRST_SECTOR))
    {
        boot_puts("[BOOT] !! 扇区表自检失败 —— 布局常量与扇区表不一致，停在这里\n");
        for (;;) { }
    }

    if (!IAP_BootLayoutValid())
    { boot_puts("[BOOT] layout descriptor/capacity invalid; use full bootstrap HEX\n"); for (;;) {} }
    boot_puts("[BOOT] A=224KiB B=256KiB cap=229376 layout=00020001\n");
    have_meta = IAP_MetaRead(&meta);
    if (have_meta && meta.reserved[0] != IAP_LAYOUT_ID)
    { boot_puts("[BOOT] old metadata layout; install full bootstrap HEX via ROM UART\n"); for (;;) {} }
    if (have_meta && (meta.fw_size < 8u || meta.fw_size > IAP_FW_SIZE_MAX ||
        (meta.state != IAP_ST_READY && meta.state != IAP_ST_CONFIRMED && meta.state != IAP_ST_FAILED)))
    { boot_puts("[BOOT] invalid metadata bounds/state; use ROM UART full bootstrap HEX\n"); for (;;) {} }

    if (have_meta == 0u)
    {
        /* ---- ① 没有元数据：ST-Link 刚烧完 / 元数据被擦过 ---- */
        boot_puts("[BOOT] 元数据无效或为空 -> 直接跳 Slot A（不校验）\n");
    }
    else
    {
        boot_puts("[BOOT] 元数据 seq=");
        boot_putu(meta.seq);
        boot_puts(" state=");
        boot_putu(meta.state);
        boot_puts("\n");

        if (meta.state == IAP_ST_READY)
        {
            /* ---- ② 有待提交的固件：先搬，成功了才改写状态 ---- */
            if (boot_commit(&meta) != 0u)
            {
                meta.state = IAP_ST_CONFIRMED;
                if (IAP_MetaWrite(&meta) == 0u)
                {
                    boot_puts("[BOOT] !! 元数据写入失败（固件本身已就位，可以照常启动）\n");
                }
            }
            else
            {
                /* ★ 失败就**记下来并停住**，不要下次开机又擦一遍 Slot A：
                 *   反复擦写既伤 Flash，又会把"为什么失败"的现场抹掉。 */
                meta.state = IAP_ST_FAILED;
                IAP_MetaWrite(&meta);
                boot_puts("[BOOT] !! 提交失败，已置 FAILED 并停在 bootloader\n");
                boot_puts("[BOOT] 恢复：用 ST-Link 重烧，或擦掉元数据扇区\n");
                for (;;) { }
            }
        }
        else if (meta.state == IAP_ST_FAILED)
        {
            boot_puts("[BOOT] !! 上次提交失败（state=FAILED）—— 不自动重试，停在 bootloader\n");
            boot_puts("[BOOT] 恢复：用 ST-Link 重烧，或擦掉元数据扇区\n");
            for (;;) { }
        }
        else
        {
            /* ---- ③ CONFIRMED：验一下 A 再跳 ---- */
            if (meta.fw_size != 0u)
            {
                if (IAP_Crc32Flash(IAP_SLOT_A_BASE, meta.fw_size) != meta.fw_crc)
                {
                    boot_puts("[BOOT] !! Slot A CRC 不符（固件坏了）—— 停在 bootloader\n");
                    for (;;) { }
                }
                boot_puts("[BOOT] Slot A 校验通过\n");
            }
        }
    }

    /* ---- ④ 跳之前做范围检查：这是防"跳进空 Flash 直接跑飞"的那道闸 ---- */
    if (boot_image_ok(IAP_SLOT_A_BASE, IAP_SLOT_A_SIZE) == 0u)
    {
        boot_puts("[BOOT] !! Slot A 里没有可用的镜像（空或损坏）—— 停在这里\n");
        boot_puts("[BOOT] 恢复：用 ST-Link 烧 App（基址 0x08008000）\n");
        for (;;) { }
    }

    boot_jump(IAP_SLOT_A_BASE);

    for (;;) { }
}

