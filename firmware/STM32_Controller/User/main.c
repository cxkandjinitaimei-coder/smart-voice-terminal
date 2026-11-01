/*
 * 文件用途：F407 主循环和上电自检入口。
 *
 * 调用关系：启动代码进入 main；main 初始化时基、串口、LCD、SD、麦克风、
 * PCA9685、ESP 链路和 OTA，再以非 RTOS 的时间片方式轮询各模块。
 * 输入：USART2 调试命令、USART6/PC6-PC7 的 ESP 文本/二进制帧、PA0/PE3 按键。
 * 输出：LCD 局部刷新、PCA9685 舵机 PWM、SD/Flash 读写、ESP 上行状态和 OTA 回执。
 * 资源与限制：主循环不能长时间阻塞；麦克风 DMA 半块约 64 ms，录音或流式上传
 * 期间禁止整页绘制、SD 慢读和无界等待。F407 没有 RTOS，所有“任务”都必须在
 * 有界时间片内推进，并由全局毫秒时基和 busy 互斥条件保证顺序。
 */
#include "camera_link.h"
#include "stm32f4xx.h"
#include "../System/time.h"        /* TIM6_Init / GetTick_ms —— 全局时基 */
#include "uart.h"        /* UART_Init —— printf 重定向的前提（现在是 USART2/PA2-PA3） */
#include "lcd.h"         /* LCD 驱动 */
#include "startup_trace.h"
#include "app_version.h"
#include "sd.h"          /* SD 卡驱动 */
#include "pca9685.h"     /* 16 路 PWM（舵机） */
#include "key.h"         /* 板载 K-UP(PA0) 与 PE3 —— 切屏/OTA确认 */
#include <stdio.h>
#include <string.h>
#include "media.h"
#include "uart_rx.h"
#include "inmp441.h"     /* INMP441 麦克风（I2S2：PB10=CK / PB9=WS / PC3=SD） */
#include "micrec.h"      /* 录音（RAM 版）：/rec /dump /mic */
#include "esp_uart.h"    /* ESP8266 链路（USART6：PC6=TX / PC7=RX @921600，P3 音频流） */
#include "ailink.h"       /* ESP 下行帧解析 + 静默窗合并重绘（App\ailink.c） */
#include "led_pwm.h"      /* 板载双 LED = TIM3_CH1/CH2（PA6/PA7），@LED_ON/@LED_OFF 用 */
#include "robot_gui.h"
#include "face_anim.h"
#include "miclink.h"      /* 麦克风触发接口（MIC_TRIG_SRC=ESP 时认 @MIC: 帧）—— App\miclink.c */
#include "micaud.h"       /* MicAud_IsStreaming / MicAud_Diag —— 流式上行（Hardware\20MICAUD）*/
#include "dht11.h"        /* DHT11 DATA=PE0；每 2 秒读取并经 ESP 上报 */
/* === [03网页端] BEGIN === 板载 W25Q SPI Flash（SPI1 重映射组：PB0=CS(软片选) / PB3=SCK / PB4=MISO / PB5=MOSI）
 * ⚠️ PB3=JTDO、PB4=NJTRST ⇒ 本模块一初始化，**整条 JTAG 就废掉**（SWD 不受影响）。
 *    Keil 里的调试端口必须选 **SW**；选 JTAG 会变成“W25Q_Init() 之前连得上、之后连不上”。
 * 文件在 `Hardware\22W25Q\`（纯驱动，零依赖，不碰任何别的器件）。 */
#include "w25q.h"
/* 字库扩展：把 3755 个汉字放到上面那块 W25Q 上（App\font_store.c + App\font_upload.c）
 * ⚠️ `font_store.h` 是**全工程唯一的 W25Q 地址布局**出处，要改布局只改它。
 * ⚠️ `/font` 一族**必须以 `/` 开头** ⇒ 和 @IAP 一样，必须串在 Media_Command **之前**。 */
#include "font_store.h"
#include "font_upload.h"
#include "font.h"          /* Font_Count() —— 状态页那行 FONT: 用（★ 漏了它就是 #223-D 隐式声明） */
/* === [03网页端] END === */

/* === [02OTA] BEGIN === IAP / OTA（计划蓝图 §4/§5；代码在 Hardware\21IAP\）
 * ★ 本模块**全程不擦 Slot A**（= 正在跑的这份自己）—— 只擦 Slot B 暂存区。
 * ⚠️ 擦除期间 CPU 被 Flash 控制器停住 1~4 秒 ⇒ 别的模块**先问 IAP_IsBusy() 再决定**；
 *    要「**拒绝**」不要「排队」：麦克风 DMA 环只有 64ms，一排队必溢出。
 * ⚠️ `@IAP:...` 命令**不以 `/` 开头** ⇒ 若落到 Media_Command 的兜底分支，会被当成
 *    要显示的文字**直接上屏**（连 `@IAP:` 一起）且不报错 ⇒ 必须串在它**之前**。
 */
#include "iap_app.h"
/* === [02OTA] END === */

/* ==================================================================
 * 上电自检 + 显示（LCD / SD / PCA9685 集成测试）
 * ------------------------------------------------------------------
 * 调用顺序有讲究，别随手改：
 *   1) TIM6_Init()      必须最先 —— PCA9685 的等待靠它计时
 *   2) UART_Init()      必须在**任何 printf 之前** —— LCD_ReadID() 里就有 printf
 *   3) LCD_Init()       排在 PCA9685_Init() **前面** —— 就算 I2C 那边出问题，
 *                       屏也已经在自检了，不至于"屏全黑 → 你去查 LCD"这种误诊
 *   4) PCA9685_Init()   放最后：它带超时，最坏情况也只是打印一行错误
 *   5) INMP441_Init()   也放最后：它内部要配 **PLLI2S**（I2S 的独立时钟源），
 *                       跟前四项互不相干；没接麦也不影响别的功能
 *   6) ESP_UART_Init()  紧跟 UART_Init()：它只用 TIM6 时基 + printf，
 *                       不碰屏/SD/I2C，所以排在 LCD 之前
 *
 * 主循环按时基调度；LCD/SD/I2C 调用仍为同步操作，按键由 TIM6 独立采样。
 *
 * ★ 2026-09-17 两处改动（都是照着用户上机反馈改的）：
 *   · **调试串口从 USART1(PA9/PA10) 换到 USART2(PA2/PA3)** —— 屏插上 J1 之后
 *     USART1 那组排针被挡住了，日志根本接不出来（详见 uart.h 头部）。
 *   · **画面不再自动切**：改成按板载按键 **K-UP(PA0)** 轮流切换状态页、彩条、BMP和串口文字页。
 *
 * ⚠️ 上机前的硬件前提（不满足会被误判成软件 bug）：
 *   · ★ 屏是 **32 针**、和板上 J1（2×16）**配套**，**直接插上去就行，不用飞线**
 *     （早先"屏 34 针、J1 32 针、只能飞线"是照着**另一块板**的 34 针原理图
 *      得出的结论，对这块配套屏不成立，已作废）
 *   · **HC-SR04 模块必须物理拔掉**（它的 ECHO = PC8，和 SDIO_D0 抢线）
 *   · 串口接 **PA2(TX)/PA3(RX) + GND**，115200-8-N-1
 * ================================================================== */

/* ==================================================================
 * PCA9685 上电自动扫动 —— ★ 默认【关】
 * ------------------------------------------------------------------
 * 老固件里每 2 秒把三路舵机在 0°/180° 之间来回扫，是**裸测舵机**用的。
 * 现在舵机由 /pwm 命令和 ESP 来的 AI 决策帧驱动，这个自动扫动会
 *   **把 AI 刚转过去的角度扫走** —— 现象是"舵机动了一下，过两秒自己回去了"。
 * ⇒ 默认 0（关）。要单独裸测舵机时改成 1。
 * ================================================================== */
#define PCA_AUTO_SWEEP   0

/* ---------- 屏上状态（由自检结果填，主循环刷新用） ---------- */
static uint16_t    g_lcd_id  = 0;      /* 0 = 没读出来 */
static uint8_t     g_sd_ret  = SD_ERR_NOCARD;
static uint32_t    g_sd_sec  = 0;      /* 容量（512B 扇区数） */
static uint32_t    g_sec0_sig = 0;     /* 扇区 0 末两字节（拼成 16 位），0x55AA = 正常 */
static uint8_t     g_pca_ok  = 0;
static uint8_t     g_mic_ok  = 0;      /* 麦克风初始化是否成功（0 = 没成功，不报音量） */
static uint8_t     g_mic_pin_error;
static uint8_t g_painted_page=PAGE_KEEP;
static uint8_t g_gui_audio_active;

/* 两个板载按键都以 1 ms 周期采样，事件只在主循环消费，避免中断里切页或写 Flash。
 * PA0 为高电平有效，PE3 为低电平有效；LCD 使用 PD13 作为寄存器选择线。 */
void System_TickHook(void)
{
    KEY_UP_Tick1ms();
    KEY_OTA_Tick1ms();
}

/* 读扇区 0 并取末两字节（0x55AA 是 FAT 引导签名）。
 * 返回 0 = 读成功 */
static uint8_t read_sector0_sig(uint32_t *sig)
{
    static uint8_t buf[512];           /* static：512 字节不进栈 */

    uint8_t ret = Sd_Read_Sector(0u, buf, 1u);

    if (ret != SD_OK)
    {
        return ret;
    }
    if (sig != 0)
    {
        *sig = ((uint32_t)buf[510] << 8) | (uint32_t)buf[511];
    }
    return 0u;
}

/* ==================================================================
 * 状态页：整页画一次（切页 / 上电 / SD 结论变化时调）
 * 屏上秒表另有 lcd_update_uptime()，只重画那一行，免得整屏每秒闪一下
 * ================================================================== */
/* 状态行只读取已经缓存的 DHT 状态和实时 I2S 数据，这里不能直接触发 DHT 采样。
 * 所有绘制入口仍受录音互斥保护，避免慢速 LCD 操作打断音频 DMA。 */
static void lcd_gui_hardware(void)
{
    GuiHardware h;
    h.lcd_id=g_lcd_id; h.sd_ok=(uint8_t)(g_sd_ret==SD_OK);
    h.pca_ok=g_pca_ok; h.mic_ok=g_mic_ok; h.mic_pin_error=g_mic_pin_error;
    h.sd_mb=g_sd_sec/2048u; Gui_SetHardware(&h);
}
static void lcd_update_modules(void)
{
    lcd_gui_hardware(); Gui_ShowDevice(GetTick_ms()/1000u);
}
static void lcd_show_status(void)
{
    Gui_Frame("设备状态 / DEVICE",1u); lcd_update_modules();
}
static void lcd_update_uptime(uint32_t uptime_s)
{
    Gui_ShowDevice(uptime_s);
}
static void lcd_audio_banner(uint8_t force)
{
    Gui_AudioTick(g_painted_page,GetTick_ms(),force);
}

static void show_page(uint8_t page, uint32_t uptime_s)
{
    if(page!=PAGE_AI) FaceAnim_Stop();
    switch(page)
    {
        case PAGE_STATUS: lcd_show_status(); lcd_update_uptime(uptime_s); break;
        case PAGE_COLOR: LCD_ColorBar(); break;
        case PAGE_PHOTO: Media_ShowPhoto(); break;
        case PAGE_TEXT: Media_ShowText(); break;
        case PAGE_AI:
            if(g_painted_page == PAGE_AI) Media_UpdateAIPage();
            else Media_DrawAIPage();
            break;
        case PAGE_MIC_SETTINGS:
            Gui_Frame("语音设置 1/3",2u); Gui_ShowMicSettings(); break;
        case PAGE_SERVO_SETTINGS:
            Gui_Frame("动作设置 2/3",3u); Gui_ShowServoSettings(); break;
        case PAGE_SYSTEM_SETTINGS:
            Gui_Frame("系统信息",5u); Gui_ShowSystem(); break;
        case PAGE_CAMERA:
            Gui_Frame("摄像头 / CAMERA",4u); Gui_ShowCamera(); break;
        default: break;
    }
    g_painted_page = page;
    lcd_audio_banner(1u);
    if(page==PAGE_AI) AILink_TimelinePainted();
}

/* === [01语音] BEGIN ===
 * 流式录音期间**不许刷屏** —— 切页的唯一入口在这里。
 * ------------------------------------------------------------------
 * 一张 160x160 的表情要 150~300ms，而麦克风 DMA 环只有 64ms ⇒ 刷一次就丢
 * ~200ms 音频，整段录音就废了（20MICAUD 的 16KB 环也垫不住 300ms）。
 * ⇒ 流式期间**只记账不真画**，流式一结束由主循环末尾补画一次：用户看到的
 *   结果一模一样，只是晚几秒。网页上有"正在录音、屏面暂停刷新"的提示，别
 *   当成死机（这句提示是 03网页端 配套加的）。
 * ★ 为什么做成函数、而不是在每个 show_page 调用点写 if：
 *   调用点有 4 处（按键切页 / Media_Command / AILink_Poll / MicLink_Tick），
 *   漏一处就是一个"偶尔丢 200ms 音频"的随机故障 —— 那是最难查的一类。
 * ================================================================== */
static uint8_t paint_pending;   /* 1 = 有一页攒着等流式结束后再画 */
static uint8_t paint_want;      /* 攒着的那一页 */

static uint8_t page_work_deferred(void)
{
    /* LCD 行回调和 BMP 分行读取期间仍维护 PWM 目标；屏幕刷新和本地按键不能等待
     * 慢速舵机走完。音频需要完整的主循环预算，因此继续保留现有的绘制互斥闸门。 */
    return (uint8_t)(MicAud_IsStreaming() || CameraLink_Busy());
}

static void paint_page(uint8_t p, uint32_t uptime_s)
{
    if(page_work_deferred())
    {
        paint_pending = 1u;
        paint_want    = p;
        return;
    }
    paint_pending = 0u; /* OTA 优先重绘会覆盖之前延后的页面请求。 */
    show_page(p, uptime_s);
}
/* === [01语音] END === */

int main(void)
{
    uint32_t last_1s, last_5s, last_mic;
    uint32_t uptime_s = 0;
    uint32_t key_count = 0;
    uint8_t page = PAGE_STATUS;
    static char rx_line[UART_LINE_SIZE];
#if PCA_AUTO_SWEEP
    uint32_t last_servo;
    uint16_t servo_angle = 0;
#endif

    StartupTrace_Print("[START] App main reached\r\n");
    SystemCoreClockUpdate();
    /* 步骤队列放在 CCM（0x10000000，64KB）；该 RAM 的接口时钟必须先开。 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_CCMDATARAMEN, ENABLE);
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

    /* ---------- ① 时基 + 串口（顺序不能反） ---------- */
    TIM6_Init();
    StartupTrace_Print("[START] TIM6 init returned\r\n");
    UART_Init();
    StartupTrace_Print("[START] UART init returned; PRIMASK=");
    StartupTrace_Hex(__get_PRIMASK());
    StartupTrace_Print(" VTOR="); StartupTrace_Hex(SCB->VTOR);
    StartupTrace_Print("\r\n[START] begin DMA printf\r\n");

    printf("\r\n===== 上电自检开始 CN-ASR-OTA-BOOT-DIAG2 =====\r\n");
    printf("[MAIN] Running firmware: %s\r\n", APP_FW_VERSION);
    printf("[MAIN] 调试串口 = USART2：PA2=TX / PA3=RX @115200\r\n");
    printf("[MAIN] （USART1 的 PA9/PA10 被屏挡住了，日志改走这里）\r\n");

    /* ---------- ①b ESP8266 链路（USART6：PC6=TX / PC7=RX @115200） ----------
     * 放在 LCD 之前：它只用 TIM6 时基 + printf，不碰屏/SD/I2C。
     * ★ 这里没有"初始化失败"这回事（USART6 配置永远成功，双方也没握手）。
     *   链路通没通看状态页的 ESP: 一行，它以 esp_last_rx_ms 为准。
     * ★ ESP 的 VCC 必须独立 3.3V 供电（发射峰值 300mA+），别从 MCU 的 LDO 取。 */
    ESP_UART_Init();
    printf("[ESP] USART6 PC6/PC7 @%u init ok（帧解析在 App\\ailink.c）\r\n",
           (unsigned)ESP_BAUD);

    /* DHT11 读时会关中断约 25~35ms，初始化/诊断只做一次；周期读取放在
     * 主循环并避开 20MICAUD 流式录音，避免吃掉 DMA 环的半块。 */
    DHT11_Init();
    DHT11_Diag();

    /* ---------- ①c 板载双 LED PWM（PA6/PA7 = TIM3_CH1/CH2，低电平点亮） ----------
     * ★ TIM3 原来是 HC-SR04 输入捕获用的，本板已经不用超声波了（它占 PC8 =
     *   SDIO_D0，和 SD 卡抢线，计划里要求物理拔掉）⇒ TIM3 现在归 LED。
     *   ⚠️ **不要再调 HCSR04_Init()** —— 会抢同一个定时器。
     * 用途：网页「测试」页的 @LED_ON/@LED_OFF 两条默认快捷键（page-test.js）。 */
    LED_PWM_Init();
    printf("[LED] PA6/PA7 (TIM3 CH1/CH2) PWM ready\r\n");

    /* ---------- ①d ESP 下行帧解析器 ----------
     * 只打白名单清单，不碰硬件；真正的解析在主循环的 AILink_Poll() 里。
     * 放在 LCD 之前：它只需要 printf 和 TIM6 时基。 */
    AILink_Init();

    /* ---------- ①e 麦克风触发接口 ----------
     * ★ 2026-09-20 [01语音]：MIC_TRIG_SRC 已拨到 ESP（miclink.h:27），所以现在
     *   它能认网页/云端下发的 @MIC: 帧了；拨回 NONE 就仍然只认串口的 /rec。
     *   MicLink_Init() 会把当前触发源和认的帧打出来 —— 现场先看这一行。 */
    MicLink_Init();

    /* ==================== [03网页端] BEGIN ====================
     * ①f 板载 W25Q SPI Flash —— 上电自检（**只读**）
     * ------------------------------------------------------------------
     * ★ 2026-09-20：用户当面派的新活 —— 字库将来要存进这颗**板载 W25Q**，
     *   第一步得先能初始化它。**驱动本体在 `Hardware\22W25Q\`**：纯驱动，
     *   只到芯片操作层（读 ID / 读 / 页编程 / 扇区擦 / 忙等待），**不含
     *   "字库""文件""分区"任何概念**；而且是**零依赖**的，连 printf 都切在
     *   单独一个文件里 —— 为的是将来 bootloader 能复用同一份源码。
     *   这里**只调一次自检**，不碰任何别的器件。
     *
     * ★ 为什么归 ① 组：本组按 `:18-32` 的定义是"只用 printf + 时基、
     *   不碰屏/SD/I2C"，而这段只碰 SPI1（PB0/PB3/PB4/PB5），正好一类。
     *   ⚠️ **别挪到 LCD 之后** —— `LCD_Init()` 在下面两行才跑，此刻屏还没上电。
     *
     * ★ 这一行还是**全工程第一次**真的用 SPI1（此前一行 SPI 代码都没有），
     *   所以它顺带回答了两件悬着的事：
     *     · 「SPI1 这条通路到底通不通」（`规划部分\计划蓝图.md:758`）
     *     · 「板载 W25Q 到底是 2MB 还是 4/8MB」（同文件 附录 B #2）
     *   期望 `EF 40 1x`：EF = Winbond，末字节 = 容量代码，容量 = 2^末字节 字节。
     *   ⚠️ 若读到 `00 00 00` / `FF FF FF`：**先查 PB3/PB4 有没有切到 AF5**
     *      （复位后它们是 JTAG 的 JTDO/NJTRST；F4 上**没有** F1 那种
     *      `GPIO_Remap_*` 宏），再查 CS 是 PB0 且空闲为高。
     *      详见 `Hardware\22W25Q\w25q.h` 头部。
     * ⚠️ 只读，不擦不写 —— 自检每按一次复位就跑一遍，擦写等于白磨损。
     * ================================================================ */
    if (W25Q_SelfTest() != W25Q_OK)
    {
        printf("[MAIN] 板载 W25Q 自检没通过 —— 看上面 [W25Q] 那几行的排查顺序\r\n");
    }

    /* ---------- ①f-2 扩展字库装载（**只读**） ----------
     * 把 W25Q 上那份字库的索引取进 RAM（7510 B），顺带校验标记 + 头部 + 索引。
     * ★ 全部只读：查标记、读 16 B 头部、读索引、探首末两个字模，**一次擦写都没有**
     *   —— 自检每按一次复位就跑，擦写等于白磨损。
     * ★ 这里**失败是正常路径**，不是故障：没灌过库就是 `rc=1(NO_MARK)`，
     *   屏上照旧 `FONT:FLASH 32`，中文页与改造前逐字一致。
     *   真的坏了才需要看 `rc` —— `FontStore_ErrName()` 给的是人话。
     * ⚠️ 必须在 `W25Q_SelfTest()` **之后**：先确认这颗片子认得出来，
     *    否则这里报的 `rc=8(SPI)` 会是个二手症状，把矛头指错。 */
    {
        uint8_t frc = FontStore_Init();
        if (frc == FONTSTORE_OK)
        {
            printf("[FONT] W25Q 字库可用: %u 字 @0x%08X, 镜像 %u B, 索引 %u B 已入 RAM\r\n",
                   (unsigned)FontStore_Count(), (unsigned)FontStore_Base(),
                   (unsigned)FontStore_ImageBytes(),
                   (unsigned)((uint32_t)FontStore_Count() * 2u));
        }
        else
        {
            printf("[FONT] ★ rc=%u(%s) —— 退回片内 %u 字（屏上 FONT:FLASH）\r\n",
                   (unsigned)frc, FontStore_ErrName(frc), (unsigned)Font_Count());
        }
    }
    /* ==================== [03网页端] END ==================== */
    /* ==================== [02OTA] BEGIN ====================
     * ①g IAP / OTA —— 只打状态，不碰硬件（归 ① 组：只用 printf + 时基）
     * ------------------------------------------------------------------
     * ★ 这一行是 L1 验收的**第一个判据**，它会打出三件事：
     *   · `fw cap`      —— IAP_FW_SIZE_MAX（= min(Slot A, Slot B) = 96 KB）
     *   · `linked base` —— `Image$$ER_IROM1$$Base`。**Slot A 应为 0x08008000**；
     *                     打出 0x08000000 且标 NORMAL = 工程还没迁基址（**正常**，
     *                     迁移属 L3）；标 WARN = 基址落在不合法区，要查。
     *   · `META` 状态   —— 有没有待提交的镜像（bootloader 靠它决定跳谁）
     * ★ 放 LCD 之前：此刻屏还没上电，但本模块只用 printf，不需要屏。
     * ================================================================ */
    IAP_AppInit();
    /* ==================== [02OTA] END ==================== */

    /* ---------- ② LCD ---------- */
    printf("[MAIN] LCD init begin\r\n");
    LCD_Init();                     /* 开头会先闪两下背光验极性 */
    LCD_SetServiceHook(AILink_ServiceMotion);
    printf("[MAIN] LCD init done\r\n");
    g_lcd_id = LCD_ReadID();        /* 验收判据：0x9341 */
    if (g_lcd_id != 0)
    {
        printf("[MAIN] LCD 识别成功：0x%04X\r\n", (unsigned)g_lcd_id);
    }
    else
    {
        printf("[MAIN] LCD 没读出 ID —— 看上面的 raw 值："
               "FFFF=RD/CS 没通，0000=屏没供电\r\n");
    }

    /* ---------- ③ SD 卡 ---------- */
    g_sd_ret = Sd_Init();           /* SD_OK = 协议/容量/双速率读通过；签名单独报告 */
    if (g_sd_ret == SD_OK)
    {
        g_sd_sec = Sd_Get_Sector_Count();
        Sd_Get_Card_Info();
        g_sd_ret = read_sector0_sig(&g_sec0_sig);
        if (g_sd_ret == SD_OK)
        {
            printf("[MAIN] 扇区 0 末两字节 %02X %02X（%s）\r\n",
                   (unsigned)((g_sec0_sig >> 8) & 0xFF),
                   (unsigned)(g_sec0_sig & 0xFF),
                   (g_sec0_sig == 0x55AAu) ? "0x55AA 引导签名，正常" : "不是 55AA，卡可能没格式化");
        }
        else
        {
            printf("[MAIN] 扇区 0 读失败\r\n");
        }

        /* ★ 写通路自测：录 WAV 要往卡上写，而这张卡到目前**只读过**、
         *   一条 CMD24 都没发过 —— 卡/卡座在写通路上的毛病（内部编程、
         *   DAT0 忙线、CRC 状态）读的时候完全看不出来，所以必须先单测。
         *   非破坏性：拿卡尾那个扇区写完再还原。 */
        {
            uint32_t t0 = GetTick_ms();
            uint8_t wret = Sd_WriteSelfTest();
            uint32_t dt = GetTick_ms() - t0;
            printf("[MAIN] 写自测耗时 %lums（含 2 次写 + 3 次读；"
                   "每扇区写约 %lums —— 录音每 32ms 要写 2 个扇区）\r\n",
                   (unsigned long)dt, (unsigned long)(dt / 5u));
            if (wret != SD_OK)
            {
                g_sd_ret = wret;      /* 写不通 ⇒ SD 这一路整体降级 */
                printf("[MAIN] ★ SD 写不通（r=%u）—— 录 WAV 会失败，"
                       "先别急着录，把上面那行 [SD] 的错贴出来\r\n", (unsigned)wret);
            }
        }
    }
    else
    {
        printf("[MAIN] SD 初始化失败：ret=%u（0=OK 1=没卡 2=CMD8 3=ACMD41 "
               "4=电压 5=超时 6=读错 7=入参 8=卡应答）\r\n", (unsigned)g_sd_ret);
        printf("[MAIN] 先确认 HC-SR04 模块已拔掉（PC8 冲突）\r\n");
    }

    /* ---------- ④ PCA9685（放最后：带超时，最坏只打一行错误） ---------- */
    g_pca_ok = (PCA9685_Init() == 0u) ? 1u : 0u;
    if (g_pca_ok)
    {
        /* 管理员校准档案尚未到达前不发任意 PWM。ESP 会重放已保存的参数快照；
         * AILink 只在初始化完成且通过互斥闸门后，把已登记的每根轴回到其 front 角一次。 */
        printf("[SERVO] startup home waits for saved profile from ESP; no forced 90 deg\r\n");
    }

    /* 先初始化两个板载按键，再允许主循环消费按键事件。 */
    KEY_UP_Init();
    KEY_OTA_Init();

    /* ---------- ⑥ 麦克风 INMP441（I2S2：PB10=CK PB9=WS PC3=SD） ----------
     * 放在最后：它内部会配 PLLI2S（I2S 的独立时钟源），跟前面几项互不相干。
     * ★ 没接麦也不影响别的功能：I2S 照跑、只是收不到数据（SD 脚有上拉，
     *   读回恒定值），INMP441_Diag() 会把这个和"时钟没跑"分开报。
     * ★ 想临时关掉麦克风，把下面 5 行注释掉即可（驱动本身可以留着不登记）。 */
    g_mic_ok = (INMP441_Init() == 0u) ? 1u : 0u;
    if (g_mic_ok)
    {
        INMP441_Start();
        g_mic_pin_error = INMP441_PinCheck();
        (void)INMP441_Calibrate();   /* 定标左声道在缓冲里的奇偶（没接麦就白等 ~50ms） */
        INMP441_Diag();              /* 把时钟链和数据线的实况打出来 —— 出问题先看这几行 */
    }

    /* ---------- ⑦ 扫卡；启动先显示含固件版本的状态页，K-UP 可切图片 ---------- */
    Media_Init(g_sd_ret == SD_OK);
    lcd_gui_hardware();
    page = PAGE_AI;
    show_page(page, 0u);
    printf("[GUI] LCD GUI enabled: FACE / DEVICE / MIC / SERVO / CAMERA; FW %s\r\n", APP_FW_VERSION);

    printf("[PWM] 命令已就绪：/pwm [id|all] [0~180|off]（id = 模块丝印号 1~16）\r\n");
    printf("[MAIN] 舵机自动扫动 = %s\r\n",
           (PCA_AUTO_SWEEP != 0) ? "开（裸测模式）" : "关（AI/命令驱动）");

    printf("===== 自检结束，进入主循环（画面靠 PE3 或 K-UP 切）=====\r\n\r\n");

    /* ---------- 主循环：先处理按键，再处理周期任务 ---------- */
    last_1s  = GetTick_ms();
    last_5s  = last_1s;
    last_mic = last_1s;
#if PCA_AUTO_SWEEP
    last_servo = last_1s;
#endif

    while (1)
    {
        uint32_t now = GetTick_ms();
        int rx_result;
        uint8_t request;
        uint8_t iap_handled = 0u;   /* [02OTA] @IAP: 命令本圈是否已被消费 */
        /* [03网页端] ★ 2026-09-21：字库这条线的"本行已被吃"**改用我自己的变量**。
         *   原先借的是 02 的 `iap_handled`（当时为了不改他那行）—— 那是**两个模块
         *   共用一个判据**：任一边改语义、换判据或把变量挪走，另一边都会**静默**跟着变
         *   （这类"共用判据"在本项目已经踩过不止一次）。各留一个，各归各的。 */
        uint32_t font_gen = FontStore_Gen();
        uint8_t font_handled = 0u;  /* [03网页端] /font: 命令本圈是否已被消费 */

        /* TIM6已消抖：主循环切页；图片/文字页不被秒表覆盖。
         * ★ 2026-09-22 [02OTA]：**K-UP 改成一键两用** —— 有待确认的 OTA 申请时它当
         *   确认键用，没有申请时才是切页键（用户不必记"这次该按哪个键"）。
         * ⚠️ `KEY_UP_Scan()` **只能取一次**：key.c:66 里它是**消费**式的（取走就 ++consumed，
         *   第二次必返 0）⇒ 必须先按键、再在里层问 OTA。写成两句 Scan() 会让切页键**失灵**。 */
        /* 一次性消费两个按键计数；若同时按下，只执行一次切页/确认操作。 */
        { uint8_t up_key=KEY_UP_Scan(), extra_key=KEY_OTA_Scan();
        if (up_key || extra_key)
        {
            AILink_UserActivity();
            if (IAP_ConfirmRequest())
            {
                /* 任一实体按键都可以确认待处理的 OTA 请求。 */
                Media_SetCaption("OTA CONFIRMED\nWAITING FOR TRANSFER");
                page = PAGE_AI;
                paint_pending = 0u;
                show_page(page, uptime_s); /* 本地明确确认优先于普通页面切换。 */
                printf("[KEY] OTA approved by physical K-UP/PE3 key\r\n");
            }
            else
            {
                uint32_t paint_start = GetTick_ms();
                key_count++;
                page = Media_NextPage(page);
                paint_page(page, uptime_s);   /* [01语音] 流式期间只记账不刷屏 */
                printf("[KEY] #%lu page=%u (0=device 4=face 5=mic 6=servo 7=camera) draw=%lums\r\n",
                       (unsigned long)key_count, (unsigned)page,
                       (unsigned long)(GetTick_ms() - paint_start));
            }
        }

        }
        rx_result = UART_GetLine(rx_line, sizeof(rx_line));
        if(rx_result > 0) AILink_UserActivity();
        if(rx_result > 0 && MicLink_IsRecording())
        {
            if(!strncmp(rx_line, "/font", 5)) printf("[FONT] ERR 4 (recording busy)\r\n");
            else printf("[RX] busy: recording; command rejected\r\n");
            rx_result = 0;
        }
        if(rx_result > 0)
        {
            if(strncmp(rx_line,"/font d ",8)) printf("[RX] %s\r\n", rx_line);
            /* ★ 录音命令**必须先截**：Media_Command 对任何不认识的 `/xxx`
             *   都会回一句 "Unknown command" 并把行吃掉，轮不到 micrec。
             *   它不是录音命令时才返回 0，再交给 media 走原来那套。 */
            /* === [02OTA] BEGIN ===
             * ★ IAP 命令**必须排在整条链最前面**：`@IAP:...` 不以 `/` 开头，
             *   落到 Media_Command 的兜底分支会被当成文字**上屏**且不报错。
             * ★ 擦写中把命令**拒掉**（只拦 `@` 开头，`/xxx` 照旧）—— 不排队：
             *   排队会把命令拖到擦除之后才执行，语义就变了。 */
            if(FontUpload_Active() && !strncmp(rx_line,"@IAP:",5))
            { printf("[IAP] ERR: font upload busy\r\n"); iap_handled=1u; }
            else iap_handled = IAP_AppCommand(rx_line);
            if(iap_handled && !strncmp(rx_line,"@IAP:REQUEST,",13))
            {
                /* 阶段：OTA 请求已被识别，先提示用户确认，再等待 PE3/K-UP；此处不排队执行。 */
                Media_SetCaption("OTA REQUESTED\nPRESS PE3 OR K-UP");
                page = PAGE_AI;
                paint_pending = 0u;
                show_page(page, uptime_s); /* 调试串口发来的 OTA 请求同样绕过动作队列。 */
            }
            if(!iap_handled && IAP_IsBusy() && rx_line[0] == '@')
            {
                iap_handled = 1u;
                printf("[IAP] busy(%s)：命令被拒（擦写中 CPU 会停 1~4s，不排队）\r\n",
                       IAP_StateName());
            }
            /* === [02OTA] END === */
            /* === [03网页端] BEGIN ===
             * 字库传输 `/font ...` —— **必须排在 Media_Command 之前**（同上理由：
             * `/font` 会落到它的兜底分支被整行吃掉，还会回一句 "Unknown command"）。
             * ★ 2026-09-21：标志改用**自己的** `font_handled`（不再借 02 的
             *   `iap_handled`），并加一道 busy 闸，见下。 */
            if (IAP_IsBusy() && !strncmp(rx_line,"/font",5))
            { printf("[FONT] ERR 4 (IAP busy)\r\n"); font_handled=1u; }
            else font_handled = FontUpload_Command(rx_line);
            if (font_handled && FontStore_Gen() != font_gen) paint_page(page, uptime_s);

            /* ★ busy 闸：会话进行中（`begin` 之后、`end`/`abort` 之前）**别的命令一律拒掉**。
             *   判据用现成的 `FontUpload_Active()`（**能给判据就别给纪律**），不另记标志。
             *   为什么不排队、也不放行：
             *     ① 一笔上传是**严格有序**的一串 `/font d`，PC 端靠"回包干净 + 每条回包
             *        带下一个期望 off"来自纠（丢包/重发/重复都能自愈）⇒ 中间插进来的命令
             *        会把自己的回包**混进这条应答流**，自纠的前提就没了；
             *     ② 那些命令可能**长阻塞**（`/rec` 十几秒），把主循环卡住 ⇒ 后面的 `/font d`
             *        堆在 USART2 的环里、PC 端一条条超时重发，白白拖慢整包。
             *   拦的条件是 `rx_line[0] == '/'`，跟 02 那边"只拦 `@`"同一个理由：
             *   **只拦走这条链的东西**，别的模块的前缀（`@` 开头那些）不碰。
             *   ⚠️ `/font abort` 走上面那行、**不受本闸影响** ⇒ 会话卡住时永远解得开，
             *      这道闸不会把人锁死。 */
            if(!font_handled && (FontUpload_Active() != 0u) && rx_line[0] == '/')
            {
                font_handled = 1u;
                printf("[FONT] busy：一次上传正在进行中，本条命令被拒（不排队；"
                       "要中止用 /font abort）\r\n");
            }
            /* ⚠️ 别把上面这道闸理解成"主循环被上传占住了" —— 没有：
             *   单条 `/font d` 只做 112 字节编程 + 回读（约 1ms），整包 1141 条
             *   是**一条一条过主循环**的；真阻塞只有 `begin` 的擦除（≤4s）和
             *   `end` 的 CRC（≈0.2s），那两处是在**这一行内部**卡住的。 */
            /* === [03网页端] END === */
            /* [03网页端] 级联里多一个 `!font_handled`（只加这一个 token，语义不变）：
             *   被字库那行吃掉的 `/font` 命令**不能**再掉进下面的兜底分支。 */
            if(!iap_handled && !font_handled && !MicRec_Command(rx_line))
            {
                /* ★ 顺序不能改：Media_Command 对任何不认识的 "/xxx" 都会回一句
                 *   "Unknown command" 并把**整行吃掉**，轮不到后面的模块。
                 *   ⇒ 新模块的命令必须串在 Media_Command **之前**。 */
                if(!PCA9685_Command(rx_line))
                {
                    request = Media_Command(rx_line);
                    if(request != PAGE_KEEP)
                    {
                        page = request;
                        paint_page(page, uptime_s);   /* [01语音] 流式期间只记账 */
                    }
                }
            }
        }
        else if(rx_result < 0)
            printf("[RX] Line discarded: overflow/error or >255 bytes; resend\r\n");

        /* --- ESP 下行帧：取尽 + 手势步进 + 静默窗合并重绘 ---
         * ★ 跟 Media_Command 一个套路：返回 PAGE_KEEP = 不用切页；
         *   返回 PAGE_AI = "这批帧攒齐了，该重画 AI 页了"。
         * ★ 顺序放在 UART_GetLine 之后：两条线互不相干，但先取完调试口
         *   的命令（优先级高、是人在敲），再处理 ESP 的批量帧。 */
        request = AILink_Poll();
        if(request != PAGE_KEEP)
        {
            page = request;
            paint_page(page, uptime_s);   /* [01语音] 流式期间只记账不刷屏 */
        }

        /* 阶段：录音请求只改变状态，不立即做整页重绘；录音生命周期只更新有界的
         * ASCII 状态行，避免 LCD 慢操作占满麦克风 DMA 的时间片。 */
        /* I2S2 始终用于麦克风输入；录音调度不再等待扬声器播放。 */
        request = MicLink_Tick(now);
        /* 阶段：在真正开始采集前显示“聆听”画面；采集开始后不再读 SD 或整页刷新，
         * 等发送/排空完成后再恢复页面，避免用户切页请求被录音流程覆盖。 */
        if(g_gui_audio_active!=MicLink_IsRecording() && !IAP_IsBusy() && !AILink_OtaPending()) {
            g_gui_audio_active=MicLink_IsRecording();
            if(g_gui_audio_active) { page=PAGE_AI; paint_page(page,uptime_s); }
            else if(page==PAGE_AI) paint_page(page,uptime_s);
        }
        lcd_audio_banner(0u);
        if(request != PAGE_KEEP)
        {
            page = request;
            paint_page(page, uptime_s);
        }
        /* === [01语音] END === */
        /* === [02OTA] BEGIN ===
         * --- IAP / OTA 状态机推进（时基驱动）---
         * ★ **每圈都要调**，不是"只在 OTA 时"：`@IAP:BEGIN` 之后收数据、校验、
         *   提交、以及**延迟擦除**（到点才擦，不在中断里擦）都靠它推进。
         *   非 OTA 时它只读一个 static 就返回，代价可忽略。
         * ⚠️ 擦除就在**这一句里面**发生 —— CPU 会在这里被停住 1~4 秒。
         * ================================================================ */
        IAP_AppTick(now);
        /* === [02OTA] END === */

        /* 只有 LCD 驱动真正改变方向后才重绘全部坐标；字库传输、采集/排空和 OTA
         * 期间保持当前方向，避免在临界阶段做整页刷新。 */
        if(!FontUpload_Active() && Gui_OrientationTick(GetTick_ms())) {
            g_painted_page=PAGE_KEEP;
            paint_page(page,uptime_s);
        }

        /* DHT11 采样成功后同时打印 USART2 并发送 @SENSOR: 给 ESP；录音期间
         * 禁止读取，保证 64ms 音频 DMA 环不会被 30ms 的单总线时序打断。 */
        if (!CameraLink_Busy() && !MicAud_IsStreaming() && !IAP_IsBusy() && !AILink_ServoBusy())
            DHT11_sys();

        /* --- 每 1 秒刷新屏上的秒表（只重画那一行，不整屏重画以免闪） --- */
        if (now - last_1s >= 1000u)
        {
            last_1s = now;
            uptime_s = (uint32_t)(now / 1000u);   /* TIM6 从开机起算，直接除 */
            if (page == PAGE_STATUS && !CameraLink_Busy() && !MicAud_IsStreaming() && !IAP_IsBusy())
            {
                lcd_update_uptime(uptime_s);
                lcd_update_modules();
            }
            else if(!CameraLink_Busy() && !MicAud_IsStreaming() && !IAP_IsBusy()) Gui_UpdateParameters(g_painted_page);
        }

        /* --- 每 500ms 报一次麦克风音量：对着麦说话，看这个数跳不跳 ---
         * ★ [01语音] 流式期间**跳过**：这行 printf 走调试口（USART2/115200），
         *   80 来个字符要 ~7ms 的忙等，白占音频节拍。要看状态就看下面
         *   每 5 秒那行 `[MICAUD]`（它在流式期间照打）。 */
        if ((now - last_mic >= 500u) && !CameraLink_Busy() && !MicAud_IsStreaming() &&
            !AILink_ServoBusy() && !IAP_IsBusy())
        {
            last_mic = now;
            if (g_mic_ok)
            {
                /* 音量 0~100（已去直流 + 开方压缩）。真实语音大约落在 30~90；
                 * 一直 0 而 半块 在涨 ⇒ 麦在响但太小/没供电；半块 不涨 ⇒ 见下面 ndtr 那一列。
                 * 峰峰值是判断"麦到底有没有在动"最直观的量：静音只剩底噪时几十~几百，
                 * 说话能到几千~几万。
                 * ★ ndtr = DMA 还剩几个半字没搬完。**半块=0 而 ndtr 在跳** ⇒ 数据其实在搬，
                 *   问题在中断那条路，**别去查接线**（2026-09-18 就在这儿栽过一次）。 */
                printf("[MIC] 音量=%u%% rms=%lu 峰峰=%ld 半块=%lu ndtr=%u\r\n",
                       (unsigned)INMP441_Level(),
                       (unsigned long)INMP441_Rms(),
                       (long)INMP441_PeakToPeak(),
                       (unsigned long)INMP441_Blocks(),
                       (unsigned)INMP441_DmaLeft());
            }
        }

        /* --- 每 5 秒重读一次扇区 0：证明 SD 是**持续**可读，不是碰巧过了一次 --- */
        if (now - last_5s >= 5000u && !CameraLink_Busy() && !MicAud_IsStreaming() && !FontUpload_Active() &&
            !AILink_ServoBusy() && !IAP_IsBusy())
        {
            uint32_t sig = 0;
            uint8_t ret;

            last_5s = now;

            /* ★ [01语音] 每 5 秒的音频流摘要（一行，非流式时就是 IDLE 0/0）。
             *   ★ 流式期间**照打** —— 这正是要盯着它的时候：
             *     丢半块必须是 0，重传 >0 是修复成功（不是故障）。 */
            MicAud_Diag();

            /* ★ [01语音] 流式期间**不读 SD**：FatFs 读一次扇区最坏几十 ms，
             *   而音频每 64ms 就要一块、ESP 那边每块只等 800ms。
             *   不丢检查 —— 流式结束后下一个 5 秒边界照常读。
             *   ⚠️ 别把这句挪到整个 if 外面：那样流式一开始 `now - last_5s`
             *      就一直是旧值、SD 检查会被无限推迟。 */
            if ((g_sd_ret == SD_OK) && (MicAud_IsStreaming() == 0u))
            {
                ret = read_sector0_sig(&sig);
                if (ret == SD_OK)
                {
                    printf("[MAIN] 重读扇区 0：末两字节 %04X %s\r\n",
                           (unsigned)sig,
                           (sig == 0x55AAu) ? "OK" : "签名不对");
                    if (sig != g_sec0_sig)
                    {
                        g_sec0_sig = sig;
                        if (page == PAGE_STATUS)
                        {
                            lcd_show_status();
                            lcd_update_uptime(uptime_s);
                        }
                    }
                }
                else
                {
                    g_sd_ret = ret;
                    printf("[MAIN] 重读扇区 0 失败 ret=%u；停止周期读卡，复位后重试\r\n", (unsigned)ret);
                    if (page == PAGE_STATUS) lcd_show_status();
                }
            }
            printf("[MAIN] UP=%lus SD=%u PCA=%s PA0=%u KEY=%lu\r\n",
                   (unsigned long)(GetTick_ms() / 1000u), (unsigned)g_sd_ret,
                   g_pca_ok ? "OK" : "FAIL", (unsigned)KEY_UP_Level(),
                   (unsigned long)key_count);
        }

        /* === [01语音] BEGIN ===
         * --- 流式期间攒下的那一次重画，在这里补上 ---
         * ★ 放主循环末尾（所有会改页面的模块都跑完之后）：这样攒下的是**最后
         *   那一个**页号，而不是中间态 —— 流式期间来了 5 条 @TEXT: 也只画一次。
         * ★ 只在**流式结束之后**才画：MicAud_IsStreaming() 变 0 的那一刻，
         *   麦克风已经不发新块了，这 150~300ms 不再吃音频。 */
        if (paint_pending && !page_work_deferred())
        {
            paint_pending = 0u;
            show_page(paint_want, uptime_s);
        }
        /* === [01语音] END === */

        /* 阶段：只有音频、OTA、字库传输都空闲时，才执行有限的 SD/LCD 小片段。 */
        Gui_StatusTick(GetTick_ms(),0u);
        Media_AnimationTick(g_painted_page,GetTick_ms());

#if PCA_AUTO_SWEEP
        /* --- 每 2 秒把 id 1/2/3 在 0° 和 180° 之间来回（老固件的裸测动作） ---
         * ★ 默认关：见文件头 PCA_AUTO_SWEEP 那段。开着会把 AI 的角度扫走。 */
        if (g_pca_ok && (now - last_servo >= 2000u))
        {
            last_servo = now;
            servo_angle = (servo_angle == 0u) ? 180u : 0u;
            PCA9685_SetAngle(PCA_ID_TO_CH(1u), servo_angle);
            PCA9685_SetAngle(PCA_ID_TO_CH(2u), servo_angle);
            PCA9685_SetAngle(PCA_ID_TO_CH(3u), servo_angle);
            printf("[MAIN] 舵机 id 1/2/3 -> %u 度\r\n", (unsigned)servo_angle);
        }
#endif
    }
}
