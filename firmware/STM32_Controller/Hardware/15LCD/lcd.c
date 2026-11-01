#include "lcd.h"
#include "lcd_font.h"     /* ⚠️ 字模表只能在这里包含（数组是定义不是声明），见 lcd_font.h 顶部 */
#include <stdio.h>
#include "lcd_orientation.h"

/* ==================================================================
 * TFT LCD 并口屏驱动（ILI9341，8080，FSMC NE1）
 * ------------------------------------------------------------------
 * 设计取舍（为什么这么写，改之前先读）：
 *  1) 上电序列用的是 ILI9341 厂商通用那一整套（0xCF/0xED/0xE8/0xCB/
 *     0xF7/0xEA/0xC0/0xC1/0xC5/0xC7/0x3A/0x36/0xB1/0xB6/0xF2/0x26/
 *     0xE0/0xE1/0x11/0x29）。移植自例程 D:\AAkeil\zProject\03\Hardware\17LCD，
 *     只把板级相关的部分换成本板的（FSMC Bank1/NE1/A18、PB1 背光、竖屏 0x48）。
 *  2) 延时不用 TIM6：自己空转。谁调用都不会因为忘了 TIM6_Init 卡死。
 *  3) 读写时序分开（FSMC 扩展模式）：写快，读慢。
 *     ILI9341 这类的读周期要求 RD 低电平 ≥ 355ns，太快读 ID 就是 0。
 *  4) 读 ID 不猜面板对齐方式：16 位并口下每次读只承载 1 个 ID 字节，
 *     落在高半还是低半取决于面板，所以按三种假设各试一遍（见 LCD_ReadID）。
 * ================================================================== */

/* ---------- 命令 / 数据 两个地址（换算见 lcd.h 顶部） ---------- */
#define LCD_REG   (*(volatile uint16_t *)0x60000000UL)   /* RS = 0 */
#define LCD_RAM   (*(volatile uint16_t *)0x60080000UL)   /* RS = 1 */

/* Service only PWM/I2C between pixel rows; no ISR or recursive drawing. */
static void (*lcd_service_hook)(void);
static uint8_t lcd_servicing;
uint16_t lcd_width=240u, lcd_height=320u;
static uint8_t lcd_orientation=LCD_PORTRAIT_0;

void LCD_SetServiceHook(void (*hook)(void)) { lcd_service_hook = hook; }

static void lcd_service(void)
{
    if(lcd_service_hook && !lcd_servicing) {
        lcd_servicing=1u; lcd_service_hook(); lcd_servicing=0u;
    }
}

/* ---------- 背光 ---------- */
#define LCD_BL_PORT   GPIOB
#define LCD_BL_PIN    GPIO_Pin_1

/* ---------- 内部：粗延时 ----------
 * 空转，按 168MHz / -O0 标定约 0.6ms 一次（只用在 LCD_Init 的上电时序里）。
 * ⚠️ 换主频、换优化等级（-O0↔-O2）都会失准；只要求"够长"，不要求准。
 * 刻意不用 TIM6：LCD_Init 保持零依赖，谁调用都不会因为忘了 TIM6_Init 卡死。 */
static void lcd_delay_ms(uint16_t ms)
{
    volatile uint32_t i;

    while (ms--)
    {
        for (i = 0; i < 20000; i++)
        {
        }
    }
}

/* ---------- 内部：写命令 / 写数据 / 读数据 ---------- */
static void lcd_wr_cmd(uint16_t cmd)
{
    LCD_REG = cmd;
}

static void lcd_wr_data(uint16_t dat)
{
    LCD_RAM = dat;
}

void LCD_SetOrientation(uint8_t orientation)
{
    if(orientation>LCD_LANDSCAPE_270) return;
    lcd_wr_cmd(0x36u);
    lcd_wr_data(LCD_OrientationMadctl(orientation));
    LCD_OrientationSize(orientation,&lcd_width,&lcd_height);
    lcd_orientation=orientation;
}

uint8_t LCD_GetOrientation(void) { return lcd_orientation; }

static uint16_t lcd_rd_data(void)
{
    return LCD_RAM;
}

/* ==================================================================
 * 背光：极性由 LCD_BL_ACTIVE_HIGH 决定（上电全黑就改它）
 * ================================================================== */
void LCD_Backlight(uint8_t on)
{
#if LCD_BL_ACTIVE_HIGH
    if (on) GPIO_SetBits(LCD_BL_PORT, LCD_BL_PIN);
    else    GPIO_ResetBits(LCD_BL_PORT, LCD_BL_PIN);
#else
    if (on) GPIO_ResetBits(LCD_BL_PORT, LCD_BL_PIN);
    else    GPIO_SetBits(LCD_BL_PORT, LCD_BL_PIN);
#endif
}

/* ==================================================================
 * GPIO：数据线 + 控制线全复用 AF12(FSMC)，背光当普通输出
 * ------------------------------------------------------------------
 * ⚠️ RCC_AHB1Periph_GPIOB 是给背光 PB1 的，别删！
 *    删了 → GPIO_Init(GPIOB,…) 是空写 → PB1 停在复位后的浮空输入
 *         → 背光状态不确定（实测：悬空时是**亮**的，见"按住 RST"那条）
 * ================================================================== */
static void lcd_gpio_init(void)
{
    GPIO_InitTypeDef gi;
    uint16_t pins_d, pins_e;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB | RCC_AHB1Periph_GPIOD |
                           RCC_AHB1Periph_GPIOE, ENABLE);

    /* ---- GPIOD ---- */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource0,  GPIO_AF_FSMC);  /* D2      */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource1,  GPIO_AF_FSMC);  /* D3      */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource4,  GPIO_AF_FSMC);  /* NOE→RD  */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource5,  GPIO_AF_FSMC);  /* NWE→WR  */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource7,  GPIO_AF_FSMC);  /* NE1→CS  */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource8,  GPIO_AF_FSMC);  /* D13     */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource9,  GPIO_AF_FSMC);  /* D14     */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource10, GPIO_AF_FSMC);  /* D15     */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource13, GPIO_AF_FSMC);  /* A18→RS  */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource14, GPIO_AF_FSMC);  /* D0      */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource15, GPIO_AF_FSMC);  /* D1      */

    /* ---- GPIOE：D4~D12 = PE7~PE15 ---- */
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource7,  GPIO_AF_FSMC);  /* D4  */
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource8,  GPIO_AF_FSMC);  /* D5  */
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource9,  GPIO_AF_FSMC);  /* D6  */
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource10, GPIO_AF_FSMC);  /* D7  */
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource11, GPIO_AF_FSMC);  /* D8  */
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource12, GPIO_AF_FSMC);  /* D9  */
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource13, GPIO_AF_FSMC);  /* D10 */
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource14, GPIO_AF_FSMC);  /* D11 */
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource15, GPIO_AF_FSMC);  /* D12 */

    pins_d = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_7 |
             GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_13 |
             GPIO_Pin_14 | GPIO_Pin_15;
    pins_e = GPIO_Pin_7 | GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 |
             GPIO_Pin_11 | GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15;

    gi.GPIO_Mode  = GPIO_Mode_AF;
    gi.GPIO_OType = GPIO_OType_PP;
    gi.GPIO_PuPd  = GPIO_PuPd_UP;      /* 内部上拉：空脚时不会悬空乱读 */
    gi.GPIO_Speed = GPIO_Speed_100MHz;

    gi.GPIO_Pin = pins_d;
    GPIO_Init(GPIOD, &gi);
    gi.GPIO_Pin = pins_e;
    GPIO_Init(GPIOE, &gi);

    /* ---- 背光 PB1：普通推挽输出 ---- */
    gi.GPIO_Mode  = GPIO_Mode_OUT;
    gi.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    gi.GPIO_Pin   = LCD_BL_PIN;
    GPIO_Init(LCD_BL_PORT, &gi);

    LCD_Backlight(1);   /* 先点亮：只接电源就能看见发白 = 供电通 */
}

/* ==================================================================
 * FSMC：NE1，SRAM 类型，16 位，扩展模式（读慢/写快分开给）
 * ⚠️ RCC_AHB3Periph_FSMC 在本函数里开，全工程只此一处，别重复开也别删。
 * ================================================================== */
static void lcd_fsmc_init(void)
{
    FSMC_NORSRAMInitTypeDef       fsmc;
    FSMC_NORSRAMTimingInitTypeDef rd_t, wr_t;

    RCC_AHB3PeriphClockCmd(RCC_AHB3Periph_FSMC, ENABLE);

    /* ---- 读时序：RD 低电平要够长（ILI9341 类要求 ≥355ns）
     *      DATAST=70 → 70×5.95ns ≈ 417ns @168MHz ---- */
    rd_t.FSMC_AddressSetupTime      = 15;
    rd_t.FSMC_AddressHoldTime       = 0;
    rd_t.FSMC_DataSetupTime         = 70;
    rd_t.FSMC_BusTurnAroundDuration = 0;
    rd_t.FSMC_CLKDivision           = 0;
    rd_t.FSMC_DataLatency           = 0;
    rd_t.FSMC_AccessMode            = FSMC_AccessMode_B;

    /* ---- 写时序：先保守，能亮之后再往小调提速 ---- */
    wr_t.FSMC_AddressSetupTime      = 2;
    wr_t.FSMC_AddressHoldTime       = 0;
    wr_t.FSMC_DataSetupTime         = 8;
    wr_t.FSMC_BusTurnAroundDuration = 0;
    wr_t.FSMC_CLKDivision           = 0;
    wr_t.FSMC_DataLatency           = 0;
    wr_t.FSMC_AccessMode            = FSMC_AccessMode_B;

    fsmc.FSMC_Bank                  = FSMC_Bank1_NORSRAM1;
    fsmc.FSMC_DataAddressMux        = FSMC_DataAddressMux_Disable;
    fsmc.FSMC_MemoryType            = FSMC_MemoryType_SRAM;
    fsmc.FSMC_MemoryDataWidth       = FSMC_MemoryDataWidth_16b;
    fsmc.FSMC_BurstAccessMode       = FSMC_BurstAccessMode_Disable;
    fsmc.FSMC_AsynchronousWait      = FSMC_AsynchronousWait_Disable;
    fsmc.FSMC_WaitSignalPolarity    = FSMC_WaitSignalPolarity_Low;
    fsmc.FSMC_WrapMode              = FSMC_WrapMode_Disable;
    fsmc.FSMC_WaitSignalActive      = FSMC_WaitSignalActive_BeforeWaitState;
    fsmc.FSMC_WriteOperation        = FSMC_WriteOperation_Enable;
    fsmc.FSMC_WaitSignal            = FSMC_WaitSignal_Disable;
    fsmc.FSMC_ExtendedMode          = FSMC_ExtendedMode_Enable;
    fsmc.FSMC_WriteBurst            = FSMC_WriteBurst_Disable;
    fsmc.FSMC_ReadWriteTimingStruct = &rd_t;
    fsmc.FSMC_WriteTimingStruct     = &wr_t;

    FSMC_NORSRAMInit(&fsmc);
    FSMC_NORSRAMCmd(FSMC_Bank1_NORSRAM1, ENABLE);
}

/* ==================================================================
 * ILI9341 上电初始化序列（厂商通用）
 *   若某些面板颜色反（红蓝互换），把 0x36 的 BGR 位取反即可（0x48↔0x40）。
 * ================================================================== */
static void lcd_ili9341_init(void)
{
    /* ---- 电源控制 ---- */
    lcd_wr_cmd(0xCF);
    lcd_wr_data(0x00); lcd_wr_data(0xC1); lcd_wr_data(0x30);

    lcd_wr_cmd(0xED);                       /* 电源时序 */
    lcd_wr_data(0x64); lcd_wr_data(0x03); lcd_wr_data(0x12); lcd_wr_data(0x81);

    lcd_wr_cmd(0xE8);                       /* 驱动时序 A */
    lcd_wr_data(0x85); lcd_wr_data(0x00); lcd_wr_data(0x78);

    lcd_wr_cmd(0xCB);                       /* 电源时序 A */
    lcd_wr_data(0x39); lcd_wr_data(0x2C); lcd_wr_data(0x00);
    lcd_wr_data(0x34); lcd_wr_data(0x02);

    lcd_wr_cmd(0xF7);                       /* 泵比控制 */
    lcd_wr_data(0x20);

    lcd_wr_cmd(0xEA);                       /* 驱动时序 B */
    lcd_wr_data(0x00); lcd_wr_data(0x00);

    lcd_wr_cmd(0xC0);                       /* 电源控制 1 */
    lcd_wr_data(0x23);

    lcd_wr_cmd(0xC1);                       /* 电源控制 2 */
    lcd_wr_data(0x10);

    lcd_wr_cmd(0xC5);                       /* VCOM 控制 */
    lcd_wr_data(0x3E); lcd_wr_data(0x28);

    lcd_wr_cmd(0xC7);                       /* VCOM 偏移 */
    lcd_wr_data(0x86);

    lcd_wr_cmd(0x3A);                       /* 像素格式：16 位 RGB565（并口必需） */
    lcd_wr_data(0x55);

    lcd_wr_cmd(0x36);                       /* 存储访问控制（方向 + BGR） */
    lcd_wr_data(0x48);                      /* MY=0 MX=1 MV=0 BGR=1 → 竖屏 240×320
                                             * 若屏上"红"显示成"蓝"，把这里改成 0x40 */

    lcd_wr_cmd(0xB1);                       /* 帧率：79Hz */
    lcd_wr_data(0x00); lcd_wr_data(0x18);

    lcd_wr_cmd(0xB6);                       /* 显示功能控制 */
    lcd_wr_data(0x08); lcd_wr_data(0x82); lcd_wr_data(0x27);

    lcd_wr_cmd(0xF2);                       /* 3G 伽马禁止 */
    lcd_wr_data(0x00);

    lcd_wr_cmd(0x26);                       /* 伽马曲线 1 */
    lcd_wr_data(0x01);

    lcd_wr_cmd(0xE0);                       /* 正伽马 */
    lcd_wr_data(0x0F); lcd_wr_data(0x31); lcd_wr_data(0x2B); lcd_wr_data(0x0C);
    lcd_wr_data(0x0E); lcd_wr_data(0x08); lcd_wr_data(0x4E); lcd_wr_data(0xF1);
    lcd_wr_data(0x37); lcd_wr_data(0x07); lcd_wr_data(0x10); lcd_wr_data(0x03);
    lcd_wr_data(0x0E); lcd_wr_data(0x09); lcd_wr_data(0x00);

    lcd_wr_cmd(0xE1);                       /* 负伽马 */
    lcd_wr_data(0x00); lcd_wr_data(0x0E); lcd_wr_data(0x14); lcd_wr_data(0x03);
    lcd_wr_data(0x11); lcd_wr_data(0x07); lcd_wr_data(0x31); lcd_wr_data(0xC1);
    lcd_wr_data(0x48); lcd_wr_data(0x08); lcd_wr_data(0x0F); lcd_wr_data(0x0C);
    lcd_wr_data(0x31); lcd_wr_data(0x36); lcd_wr_data(0x0F);

    lcd_wr_cmd(0x11);                       /* 退出睡眠 */
    lcd_delay_ms(120);                      /* ⚠️ 这 120ms 是手册要求的，别删 */

    lcd_wr_cmd(0x29);                       /* 开显示 */
}

/* ==================================================================
 * 背光极性自检（开机第一件事，肉眼直接看得出哪种状态是亮的）
 * ------------------------------------------------------------------
 * ★★ 2026-09-17 **已由实物定案，不用再猜**：
 *   用户上机看到"背光闪两下白" —— 那两下正是本函数里的相位 A 和 C（都是
 *   `LCD_Backlight(1)`），而相位 B/D（`LCD_Backlight(0)`）没有亮。
 *   ⇒ **`LCD_Backlight(1)` = 亮** ⇒ `LCD_BL_ACTIVE_HIGH = 1` 是**对的**。
 *   （同样的结论也来自"按住 RST 背光亮"：按住 RST 时 PB1 是悬空输入。）
 *
 * 为什么留着它：这块板的背光极性历史上被读反过一次（照 `板载TFT.png` 读出
 * "PB1 低 = 亮"，实物却是反的），当时**板上没有任何办法看出来**，只能靠串口 +
 * 万用表，绕了很久。现在开机把两种状态各亮一次，**看屏就知道**：
 *   · 相位 A/C 亮 → 宏 = 1（当前，已确认）
 *   · 相位 B/D 亮 → 宏要改成 0
 *   · 一次都不亮 → 背光没电（查 J1 的 VCC），跟极性无关
 * 顺带它也是"程序到底跑没跑起来"的指示灯 —— 串口被屏挡住时尤其有用。
 * 不想每次开机都闪：把 LCD_BL_SELFTEST 改成 0 即可（**改了也别忘本函数末尾
 * 会让背光停在"灭"，见 LCD_Init 里那句补偿的 LCD_Backlight(1)**）。
 * ================================================================== */
#define LCD_BL_SELFTEST   1

static void lcd_bl_selftest(void)
{
#if LCD_BL_SELFTEST
    LCD_Backlight(1);         /* 相位 A：按当前宏判定的"亮"（= PB1 高）→ 实测：亮 */
    lcd_delay_ms(400);
    LCD_Backlight(0);         /* 相位 B：反过来（= PB1 低）→ 实测：灭 */
    lcd_delay_ms(400);
    LCD_Backlight(1);         /* 相位 C：A 再一遍（人眼容易错过第一下） */
    lcd_delay_ms(400);
    LCD_Backlight(0);         /* 相位 D：停在这里 = 灭，由 LCD_Init 负责补点亮 */
    lcd_delay_ms(300);
#endif
}

/* ==================================================================
 * 初始化：背光自检 → GPIO → FSMC → 软复位 → ILI9341 序列 → 开显示 → 白闪自检
 * ================================================================== */
void LCD_Init(void)
{
    lcd_gpio_init();      /* 里面已把背光点亮，所以"只接电源就发白"能验供电 */
    lcd_bl_selftest();    /* ★ 先闪两下把极性看清楚，再做后面的事 */
    lcd_fsmc_init();
    lcd_delay_ms(20);

    lcd_wr_cmd(0x0001);   /* 软复位 */
    lcd_delay_ms(150);

    lcd_ili9341_init();
    /* User convention: roll servo 90 degrees is landscape. */
    LCD_SetOrientation(LCD_LANDSCAPE_90);

    /* ---- 白闪自检，并且**把背光留在点亮状态** ----
     * ⚠️ 这里原来漏了 LCD_Backlight(1)，是"闪两下白、然后就黑了"的根因：
     *    `lcd_bl_selftest()` 最后一步是 LCD_Backlight(0)（故意关掉认极性），
     *    之后**没有任何地方再点亮它** ⇒ 面板其实一直在正常刷图（先白后黑），
     *    但背光是灭的，人眼看到的只有全黑。**刷什么都看不见，看现象像驱动没跑。**
     * ⇒ 顺序必须是：**先点亮背光，再刷颜色**，否则自检刷的色一个都验不到。 */
    LCD_Backlight(1);                  /* ★ 别删：后面 LCD_Clear 的可见性全靠它 */

    LCD_Clear(LCD_WHITE);              /* 看得见这一下白闪 ⇒ 背光 + 面板初始化都通 */
    lcd_delay_ms(300);
    LCD_Clear(LCD_BLACK);

    LCD_Backlight(1);                  /* ★ 留在点亮状态，屏上才看得到后面画的字 */
}

/* ==================================================================
 * 读 ID：先打原始值（定位用），再尽力认型号
 * ------------------------------------------------------------------
 * RD 没接、线序错、屏不在 8080 档，读回来都是 0xFFFF 或 0x0000。
 * 能读回 0x9341 / 0x9486 / 0x7796 这种，说明 16 根数据线 + CS/RS/WR/RD
 * 全对 —— 这是最快的"接线是否全通"证据。
 *
 * ⚠️ 上一版这里**恒返回 0**（真 bug）：它把 4 次读的结果摊平成字节流，
 *    再去滑窗找相邻的 0x93,0x41。但 16 位并口下**每次读只承载 1 个 ID 字节**
 *    （另半是 0x00），摊平后是 {00,00, 00,93, 00,41} —— 0x93 后面永远跟着
 *    0x00，永远匹配不上。现在改成按三种对齐假设各试一遍。
 * ================================================================== */
uint16_t LCD_ReadID(void)
{
    static const uint16_t known[6] =        /* 认得出的型号 */
    {
        0x9341,   /* ILI9341 */
        0x9486,   /* ILI9486 */
        0x9488,   /* ILI9488 */
        0x7796,   /* ST7796  */
        0x5310,   /* NT35310 */
        0x9325    /* ILI9325 */
    };
    static const char *method[3] =          /* 三种对齐假设，与 cand[] 一一对应 */
    {
        "ID 字节在低半（例程假设）",
        "ID 字节在高半",
        "每次读承载 2 字节"
    };
    uint16_t raw[4];
    uint16_t cand[3];
    uint8_t  i, m, k;

    lcd_wr_cmd(0xD3);                    /* Read ID4：dummy + 0x00 + 型号两字节 */
    for (i = 0; i < 4; i++)
    {
        raw[i] = lcd_rd_data();
    }

    printf("[LCD] RDID4 raw: %04X %04X %04X %04X\r\n",
           raw[0], raw[1], raw[2], raw[3]);

    /* 三种"ID 落在哪半"的假设，逐个试 */
    cand[0] = (uint16_t)((((uint16_t)(raw[2] & 0xFF)) << 8) |
                          ((uint16_t)(raw[3] & 0xFF)));
    cand[1] = (uint16_t)((((uint16_t)(raw[2] >> 8)) << 8) |
                          ((uint16_t)(raw[3] >> 8)));
    cand[2] = raw[1];

    for (m = 0; m < 3; m++)
    {
        for (k = 0; k < 6; k++)
        {
            if (cand[m] == known[k])
            {
                printf("[LCD] ID = 0x%04X（%s 命中）\r\n", known[k], method[m]);
                return known[k];
            }
        }
    }

    printf("[LCD] 三个候选值都没命中已知型号: %04X %04X %04X\r\n",
           cand[0], cand[1], cand[2]);
    if ((raw[1] & raw[2] & raw[3]) == 0xFFFF || (raw[1] | raw[2] | raw[3]) == 0x0000)
    {
        printf("[LCD] 读回全是 %04X —— RD(PD4) 没接 / CS 没通 / 屏没供电\r\n",
               raw[1]);
    }
    return 0;
}

/* ==================================================================
 * 诊断：写命令 reg 后连续读回 len 个 16 位数据
 * ================================================================== */
void LCD_ReadReg(uint16_t reg, uint16_t *buf, uint8_t len)
{
    uint8_t i;

    if (buf == 0 || len == 0) return;

    lcd_wr_cmd(reg);
    for (i = 0; i < len; i++)
    {
        buf[i] = lcd_rd_data();
    }
}

/* ==================================================================
 * 显存窗口：先定坐标范围，再连续写数据
 * ================================================================== */
void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    lcd_wr_cmd(0x002A);                  /* 列地址 x0~x1 */
    lcd_wr_data((uint16_t)(x0 >> 8));
    lcd_wr_data((uint16_t)(x0 & 0xFF));
    lcd_wr_data((uint16_t)(x1 >> 8));
    lcd_wr_data((uint16_t)(x1 & 0xFF));

    lcd_wr_cmd(0x002B);                  /* 行地址 y0~y1 */
    lcd_wr_data((uint16_t)(y0 >> 8));
    lcd_wr_data((uint16_t)(y0 & 0xFF));
    lcd_wr_data((uint16_t)(y1 >> 8));
    lcd_wr_data((uint16_t)(y1 & 0xFF));

    lcd_wr_cmd(0x002C);                  /* 开始写显存 */
}

/* ---------- 填充 ---------- */
void LCD_Fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    uint16_t row, col;

    if (x0 > x1 || y0 > y1 || x0 >= LCD_W || y0 >= LCD_H) return;
    if (x1 >= LCD_W) x1 = LCD_W - 1;
    if (y1 >= LCD_H) y1 = LCD_H - 1;

    LCD_SetWindow(x0, y0, x1, y1);

    for(row=y0; row<=y1; row++)
    {
        for(col=x0; col<=x1; col++) LCD_RAM = color;
        lcd_service();
    }
}

void LCD_Clear(uint16_t color)
{
    LCD_Fill(0, 0, LCD_W - 1, LCD_H - 1, color);
}

void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= LCD_W || y >= LCD_H) return;
    LCD_SetWindow(x, y, x, y);
    LCD_RAM = color;
}

/* 把 RAM 里的一整块 RGB565 位图（w×h，行内 stride 就是 w）刷到显存。
 * 用于显示图片/图标：比逐点 LCD_DrawPoint 快两个数量级。
 * 越出屏幕右/下边的部分自动裁掉（只读源缓冲的前若干列/行，所以裁剪仍左对齐）。 */
void LCD_ShowBuf(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *buf)
{
    uint16_t row, col;
    uint16_t stride = w;                 /* 源缓冲的行跨度，裁剪后不能跟着变小 */

    if (w == 0 || h == 0 || buf == 0) return;
    if (x >= LCD_W || y >= LCD_H) return;
    if ((uint32_t)x + w > LCD_W) w = (uint16_t)(LCD_W - x);
    if ((uint32_t)y + h > LCD_H) h = (uint16_t)(LCD_H - y);

    LCD_SetWindow(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    for (row = 0; row < h; row++)
    {
        const uint16_t *p = buf + (uint32_t)row * stride;
        for (col = 0; col < w; col++)
        {
            LCD_RAM = *p++;
        }
        lcd_service();
    }
}

uint16_t LCD_ReadPoint(uint16_t x, uint16_t y)
{
    uint16_t c;

    if (x >= LCD_W || y >= LCD_H) return 0;

    LCD_SetWindow(x, y, x, y);
    lcd_wr_cmd(0x002E);                  /* 读显存 */
    (void)lcd_rd_data();                 /* 空读（丢弃） */
    c = lcd_rd_data();

    return c;
}

/* ==================================================================
 * 画线 / 矩形 / 圆
 * ================================================================== */
void LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    int dx, dy, sx, sy, err, e2;
    int x = (int)x0;
    int y = (int)y0;

    dx = (x1 > x0) ? (int)(x1 - x0) : (int)(x0 - x1);
    dy = (y1 > y0) ? (int)(y1 - y0) : (int)(y0 - y1);
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;

    for (;;)
    {
        LCD_DrawPoint((uint16_t)x, (uint16_t)y, color);
        if (x == (int)x1 && y == (int)y1) break;

        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 <  dx) { err += dx; y += sy; }
    }
}

void LCD_DrawRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    LCD_DrawLine(x0, y0, x1, y0, color);
    LCD_DrawLine(x0, y1, x1, y1, color);
    LCD_DrawLine(x0, y0, x0, y1, color);
    LCD_DrawLine(x1, y0, x1, y1, color);
}

void LCD_DrawCircle(uint16_t xc, uint16_t yc, uint16_t r, uint16_t color)
{
    int x = 0;
    int y = (int)r;
    int d;

    if (r == 0) return;
    d = 3 - 2 * (int)r;

    while (x <= y)
    {
        LCD_DrawPoint((uint16_t)((int)xc + x), (uint16_t)((int)yc + y), color);
        LCD_DrawPoint((uint16_t)((int)xc - x), (uint16_t)((int)yc + y), color);
        LCD_DrawPoint((uint16_t)((int)xc + x), (uint16_t)((int)yc - y), color);
        LCD_DrawPoint((uint16_t)((int)xc - x), (uint16_t)((int)yc - y), color);
        LCD_DrawPoint((uint16_t)((int)xc + y), (uint16_t)((int)yc + x), color);
        LCD_DrawPoint((uint16_t)((int)xc - y), (uint16_t)((int)yc + x), color);
        LCD_DrawPoint((uint16_t)((int)xc + y), (uint16_t)((int)yc - x), color);
        LCD_DrawPoint((uint16_t)((int)xc - y), (uint16_t)((int)yc - x), color);

        if (d < 0)
        {
            d += 4 * x + 6;
        }
        else
        {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

/* ==================================================================
 * 字符 / 字符串 / 数字 / 汉字
 * ================================================================== */

/* 取字模某列 8 位图案：seg=0 上半（行 0~7）/ seg=1 下半（行 8~15） */
static uint8_t lcd_font_col(uint8_t idx, uint8_t col, uint8_t seg, uint8_t size)
{
    if (size == LCD_FONT_16)
    {
        return (seg == 0) ? LCD_F8X16[idx * 16 + col]
                          : LCD_F8X16[idx * 16 + col + 8];
    }
    return LCD_F6x8[idx][col];
}

void LCD_ShowChar(uint16_t x, uint16_t y, uint8_t chr,
                  uint16_t fc, uint16_t bc, uint8_t size, uint8_t mode)
{
    uint8_t idx, w, h, col, row, seg, bit, v;

    if (chr < ' ') chr = ' ';

    /* ⚠️ 必须夹上界：字模表只到 '~'(0x7E)，越大越界读到下一张表的头部
     *    —— 不崩，只花屏，最难查。上界由 lcd_font.h 里的 sizeof 宏给出。 */
    idx = (uint8_t)(chr - ' ');
    if (size == LCD_FONT_16)
    {
        if ((uint32_t)idx >= LCD_F8X16_NUM) idx = (uint8_t)(LCD_F8X16_NUM - 1u);
        w = 8;  h = 16;
    }
    else
    {
        if ((uint32_t)idx >= LCD_F6X8_NUM) idx = (uint8_t)(LCD_F6X8_NUM - 1u);
        w = 6;  h = 8;
    }

    if (mode == LCD_MODE_TRANS)
    {
        /* 透明模式：只有笔画处画前景色，窗口式整块写没法用 */
        for (row = 0; row < h; row++)
        {
            seg = (uint8_t)((row < 8) ? 0 : 1);
            bit = (uint8_t)(row & 7);
            for (col = 0; col < w; col++)
            {
                v = lcd_font_col(idx, col, seg, size);
                if (v & (1u << bit))
                {
                    LCD_DrawPoint((uint16_t)(x + col), (uint16_t)(y + row), fc);
                }
            }
        }
        return;
    }

    /* 叠加模式：设好窗口后整块连续写，前景/背景逐像素选色 */
    LCD_SetWindow(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    for (row = 0; row < h; row++)
    {
        seg = (uint8_t)((row < 8) ? 0 : 1);
        bit = (uint8_t)(row & 7);
        for (col = 0; col < w; col++)
        {
            v = lcd_font_col(idx, col, seg, size);
            LCD_RAM = (v & (1u << bit)) ? fc : bc;
        }
        lcd_service();
    }
}

void LCD_ShowString(uint16_t x, uint16_t y, const char *p,
                    uint16_t fc, uint16_t bc, uint8_t size, uint8_t mode)
{
    uint8_t w = (size == LCD_FONT_16) ? 8 : 6;

    if (p == 0) return;

    while (*p != '\0')
    {
        LCD_ShowChar(x, y, (uint8_t)(*p), fc, bc, size, mode);
        lcd_service();
        x = (uint16_t)(x + w);
        if ((uint16_t)(x + w) > LCD_W)     /* 一行放不下 → 换行 */
        {
            x = 0;
            y = (uint16_t)(y + ((size == LCD_FONT_16) ? 16 : 8));
        }
        p++;
    }
}

static uint32_t lcd_pow(uint8_t m, uint8_t n)
{
    uint32_t r = 1;

    while (n--)
    {
        r *= m;
    }
    return r;
}

void LCD_ShowNum(uint16_t x, uint16_t y, uint32_t num, uint8_t len,
                 uint16_t fc, uint16_t bc, uint8_t size)
{
    uint8_t t, temp, enshow = 0;
    uint8_t w = (size == LCD_FONT_16) ? 8 : 6;

    for (t = 0; t < len; t++)
    {
        temp = (uint8_t)((num / lcd_pow(10, (uint8_t)(len - t - 1))) % 10);
        if (enshow == 0 && t < (uint8_t)(len - 1))
        {
            if (temp == 0)                 /* 前导 0 显示成空格 */
            {
                LCD_ShowChar((uint16_t)(x + (uint16_t)t * w), y, ' ',
                             fc, bc, size, LCD_MODE_NORMAL);
                continue;
            }
            enshow = 1;
        }
        LCD_ShowChar((uint16_t)(x + (uint16_t)t * w), y, (uint8_t)(temp + '0'),
                     fc, bc, size, LCD_MODE_NORMAL);
    }
}

/* 汉字 16×16：字模 LCD_Hzk[2*no]=上半 16 列，LCD_Hzk[2*no+1]=下半 */
void LCD_ShowChinese(uint16_t x, uint16_t y, uint8_t no,
                     uint16_t fc, uint16_t bc, uint8_t mode)
{
    uint8_t col, row, seg, bit, v;
    uint8_t base;

    /* ⚠️ 表里只有 6 个汉字（重/庆/粤/嵌/科/技），no 再大就越界读 → 花屏 */
    if ((uint32_t)no >= LCD_HZK_NUM) no = (uint8_t)(LCD_HZK_NUM - 1u);
    base = (uint8_t)(no * 2u);

    if (mode == LCD_MODE_TRANS)
    {
        for (row = 0; row < 16; row++)
        {
            seg = (uint8_t)((row < 8) ? 0 : 1);
            bit = (uint8_t)(row & 7);
            for (col = 0; col < 16; col++)
            {
                v = LCD_Hzk[base + seg][col];
                if (v & (1u << bit))
                {
                    LCD_DrawPoint((uint16_t)(x + col), (uint16_t)(y + row), fc);
                }
            }
        }
        return;
    }

    LCD_SetWindow(x, y, (uint16_t)(x + 15), (uint16_t)(y + 15));
    for (row = 0; row < 16; row++)
    {
        seg = (uint8_t)((row < 8) ? 0 : 1);
        bit = (uint8_t)(row & 7);
        for (col = 0; col < 16; col++)
        {
            v = LCD_Hzk[base + seg][col];
            LCD_RAM = (v & (1u << bit)) ? fc : bc;
        }
    }
}

/* ==================================================================
 * 汉字 16×16：字模由调用方给（App/font.c 的字库里查出来的 32 字节）
 * ------------------------------------------------------------------
 * 【格式 —— 和上面 LCD_ShowChinese 的 LCD_Hzk **不是一套**，别互相套用】
 *   32 字节 = 16 行 × 2 字节；第 row 行的
 *     g[row*2]   → x = 0..7
 *     g[row*2+1] → x = 8..15
 *   每个字节 **高位在左**（bit7 = 该字节最左边那点），1 = 画笔画色。
 *   —— 与 SD 卡上 FONT16.BIN 的字模区**故意一致**（见 Tools/font_gen/gen.py）。
 *   上面 LCD_Hzk 那套是**列优先**（每列 1 字节、bit0 = 最上面），两回事。
 *
 * 【为什么单独写一个函数而不改 LCD_ShowChinese】LCD_ShowChinese 读的是
 *   lcd_font.h 里那张已验证过的老表，动它风险大、收益零；新表要用的格式
 *   是"逐行式"（PCtoLCD / 任何字模工具都能生成），阶段二的 FONT16.BIN 也用它。
 *   两套格式并存，注释里互相点明，避免以后有人拿错。
 * ================================================================== */
void LCD_ShowGlyph16(uint16_t x, uint16_t y, const uint8_t *g,
                     uint16_t fc, uint16_t bc, uint8_t mode)
{
    uint8_t col, row, v;

    if (g == 0) return;                    /* 调用方没查到字：直接不画，别崩 */

    if (mode == LCD_MODE_TRANS)
    {
        for (row = 0; row < 16; row++)
        {
            for (col = 0; col < 16; col++)
            {
                v = g[row * 2u + (col >> 3)];
                if (v & (uint8_t)(0x80u >> (col & 7u)))
                {
                    LCD_DrawPoint((uint16_t)(x + col), (uint16_t)(y + row), fc);
                }
            }
        }
        return;
    }

    LCD_SetWindow(x, y, (uint16_t)(x + 15), (uint16_t)(y + 15));
    for (row = 0; row < 16; row++)
    {
        for (col = 0; col < 16; col++)
        {
            v = g[row * 2u + (col >> 3)];
            LCD_RAM = (v & (uint8_t)(0x80u >> (col & 7u))) ? fc : bc;
        }
        lcd_service();
    }
}

/* ==================================================================
 * 8 条竖向彩条：白 黄 青 绿 品 红 蓝 黑
 * 用途：① 证明"能出图"  ② 看颜色顺序对不对
 *   若第 6 条（应为红）显示成蓝、第 7 条（应为蓝）显示成红
 *   → 屏是 RGB 序不是 BGR，把 LCD_Init 里 MADCTL 的 0x48 改成 0x40
 * ================================================================== */
void LCD_ColorBar(void)
{
    static const uint16_t bar[8] =
    {
        LCD_WHITE, LCD_YELLOW, LCD_CYAN, LCD_GREEN,
        LCD_MAGENTA, LCD_RED, LCD_BLUE, LCD_BLACK
    };
    uint16_t i;
    uint16_t w = LCD_W / 8;

    for (i = 0; i < 8; i++)
    {
        LCD_Fill((uint16_t)(i * w), 0, (uint16_t)(i * w + w - 1),
                 LCD_H - 1, bar[i]);
    }
}
