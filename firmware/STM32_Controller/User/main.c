/**
 * @file main.c
 * 用途：F407 应用启动、自检和无 RTOS 协作调度主循环。
 * 使用：中断只完成采样/收字节与标志更新；前台依次服务按键、协议、录音、OTA、传感器与 GUI。
 * 时序与边界：任务共享毫秒时基与 busy 状态；不可在关键采样窗口加入大段阻塞。普通/OTA 工程共用本源文件。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "camera_link.h"
#include "stm32f4xx.h"
#include "../System/time.h" /* TIM6_Init / GetTick_ms —— 全局时基 */
#include "uart.h" /* UART_Init —— printf 重定向的前提（现在是 USART2/PA2-PA3） */
#include "lcd.h"  /* LCD 驱动 */
#include "startup_trace.h"
#include "app_version.h"
#include "sd.h"      /* SD 卡驱动 */
#include "pca9685.h" /* 16 路 PWM（舵机） */
#include "key.h"     /* 板载 K-UP(PA0) 与 PE3 —— 切屏/OTA确认 */
#include <stdio.h>
#include <string.h>
#include "media.h"
#include "uart_rx.h"
#include "inmp441.h"  /* INMP441 麦克风（I2S2：PB10=CK / PB9=WS / PC3=SD） */
#include "micrec.h"   /* 录音（RAM 版）：/rec /dump /mic */
#include "esp_uart.h" /* ESP8266 链路（USART6：PC6=TX / PC7=RX @921600，P3 音频流） */
#include "ailink.h"   /* ESP 下行帧解析 + 静默窗合并重绘（App\ailink.c） */
#include "led_pwm.h"  /* 板载双 LED = TIM3_CH1/CH2（PA6/PA7），@LED_ON/@LED_OFF 用 */
#include "robot_gui.h"
#include "face_anim.h"
#include "miclink.h" /* 麦克风触发接口（MIC_TRIG_SRC=ESP 时认 @MIC: 帧）—— App\miclink.c */
#include "micaud.h" /* MicAud_IsStreaming / MicAud_Diag —— 流式上行（Hardware\20MICAUD）*/
#include "dht11.h"  /* DHT11 DATA=PE0；每 2 秒读取并经 ESP 上报 */

#include "w25q.h"
/* 字库扩展：把 3755 个汉字放到上面那块 W25Q 上（App\font_store.c + App\font_upload.c）
 *  `font_store.h` 是**全工程唯一的 W25Q 地址布局**出处，要改布局只改它。
 *  `/font` 一族**必须以 `/` 开头** ⇒ 和 @IAP 一样，必须串在 Media_Command **之前**。 */
#include "font_store.h"
#include "font_upload.h"
#include "font.h" /* Font_Count() —— 状态页那行 FONT: 用（ 漏了它就是 #223-D 隐式声明） */
/* ===  === */

/* ===  === IAP / OTA（计划蓝图 §4/§5；代码在 Hardware\21IAP\）
 *  本模块**全程不擦 Slot A**（= 正在跑的这份自己）—— 只擦 Slot B 暂存区。
 *  擦除期间 CPU 被 Flash 控制器停住 1~4 秒 ⇒ 别的模块**先问 IAP_IsBusy() 再决定**；
 *    要「**拒绝**」不要「排队」：麦克风 DMA 环只有 64ms，一排队必溢出。
 *  `@IAP:...` 命令**不以 `/` 开头** ⇒ 若落到 Media_Command 的兜底分支，会被当成
 *    要显示的文字**直接上屏**（连 `@IAP:` 一起）且不报错 ⇒ 必须串在它**之前**。
 */
#include "iap_app.h"
/* ===  === */

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
 *  两处改动（都是照着用户上机反馈改的）：
 *   · **调试串口从 USART1(PA9/PA10) 换到 USART2(PA2/PA3)** —— 屏插上 J1 之后
 *     USART1 那组排针被挡住了，日志根本接不出来（详见 uart.h 头部）。
 *   · **画面不再自动切**：改成按板载按键 **K-UP(PA0)** 轮流切换状态页、彩条、BMP和串口文字页。
 *
 *  上机前的硬件前提（不满足会被误判成软件 bug）：
 *   ·  屏是 **32 针**、和板上 J1（2×16）**配套**，**直接插上去就行，不用飞线**
 *     （早先"屏 34 针、J1 32 针、只能飞线"是照着**另一块板**的 34 针原理图
 *      得出的结论，对这块配套屏不成立，已作废）
 *   · **HC-SR04 模块必须物理拔掉**（它的 ECHO = PC8，和 SDIO_D0 抢线）
 *   · 串口接 **PA2(TX)/PA3(RX) + GND**，115200-8-N-1
 * ================================================================== */

/* ==================================================================
 * PCA9685 上电自动扫动 ——  默认【关】
 * ------------------------------------------------------------------
 * 老固件里每 2 秒把三路舵机在 0°/180° 之间来回扫，是**裸测舵机**用的。
 * 现在舵机由 /pwm 命令和 ESP 来的 AI 决策帧驱动，这个自动扫动会
 *   **把 AI 刚转过去的角度扫走** —— 现象是"舵机动了一下，过两秒自己回去了"。
 * ⇒ 默认 0（关）。要单独裸测舵机时改成 1。
 * ================================================================== */
#define PCA_AUTO_SWEEP 0

/* ---------- 屏上状态（由自检结果填，主循环刷新用） ---------- */
static uint16_t g_lcd_id = 0; /* 0 = 没读出来 */
static uint8_t g_sd_ret = SD_ERR_NOCARD;
static uint32_t g_sd_sec = 0;   /* 容量（512B 扇区数） */
static uint32_t g_sec0_sig = 0; /* 扇区 0 末两字节（拼成 16 位），0x55AA = 正常 */
static uint8_t g_pca_ok = 0;
static uint8_t g_mic_ok = 0; /* 麦克风初始化是否成功（0 = 没成功，不报音量） */
static uint8_t g_mic_pin_error;
static uint8_t g_painted_page = PAGE_KEEP;
static uint8_t g_gui_audio_active;

/* TIM6每1ms采样两枚按键，主循环通过扫描接口取走已消抖的事件。
 * PA0为高电平有效，PE3为低电平有效；LCD寄存器选择信号使用PD13。 */
void System_TickHook(void)
{
    KEY_UP_Tick1ms();
    KEY_OTA_Tick1ms();
}

/* 读扇区 0 并取末两字节（0x55AA 是 FAT 引导签名）。
 * 返回 0 = 读成功 */
static uint8_t read_sector0_sig(uint32_t *sig)
{
    static uint8_t buf[512]; /* static：512 字节不进栈 */

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
/* 状态页读取硬件初始化结果、缓存的温湿度及麦克风状态，不在绘制时启动DHT11采样。
 * 调用方避开流式音频传输，防止同步绘制造成采样缓冲积压。 */
static void lcd_gui_hardware(void)
{
    GuiHardware h;
    h.lcd_id = g_lcd_id;
    h.sd_ok = (uint8_t)(g_sd_ret == SD_OK);
    h.pca_ok = g_pca_ok;
    h.mic_ok = g_mic_ok;
    h.mic_pin_error = g_mic_pin_error;
    h.sd_mb = g_sd_sec / 2048u;
    Gui_SetHardware(&h);
}
static void lcd_update_modules(void)
{
    lcd_gui_hardware();
    Gui_ShowDevice(GetTick_ms() / 1000u);
}
static void lcd_show_status(void)
{
    Gui_Frame("设备状态 / DEVICE", 1u);
    lcd_update_modules();
}
static void lcd_update_uptime(uint32_t uptime_s)
{
    Gui_ShowDevice(uptime_s);
}
static void lcd_audio_banner(uint8_t force)
{
    Gui_AudioTick(g_painted_page, GetTick_ms(), force);
}

static void show_page(uint8_t page, uint32_t uptime_s)
{
    if (page != PAGE_AI)
        FaceAnim_Stop();
    switch (page)
    {
    case PAGE_STATUS:
        lcd_show_status();
        lcd_update_uptime(uptime_s);
        break;
    case PAGE_COLOR:
        LCD_ColorBar();
        break;
    case PAGE_PHOTO:
        Media_ShowPhoto();
        break;
    case PAGE_TEXT:
        Media_ShowText();
        break;
    case PAGE_AI:
        if (g_painted_page == PAGE_AI)
            Media_UpdateAIPage();
        else
            Media_DrawAIPage();
        break;
    case PAGE_MIC_SETTINGS:
        Gui_Frame("设置 / 麦克风 1/2", 2u);
        Gui_ShowMicSettings();
        break;
    case PAGE_SERVO_SETTINGS:
        Gui_Frame("设置 / 舵机 2/2", 3u);
        Gui_ShowServoSettings();
        break;
    case PAGE_CAMERA:
        Gui_Frame("摄像头 / CAMERA", 4u);
        Gui_ShowCamera();
        break;
    default:
        break;
    }
    g_painted_page = page;
    lcd_audio_banner(1u);
    if (page == PAGE_AI)
        AILink_TimelinePainted();
}

static uint8_t paint_pending; /* 1 = 有一页攒着等流式结束后再画 */
static uint8_t paint_want;    /* 攒着的那一页 */

static uint8_t page_work_deferred(void)
{
    /* LCD逐行绘制和BMP逐行读取会继续服务舵机插值，慢速转动本身不阻止切页。
     * 流式录音与相机传输仍需优先获得主循环时间，因此这两种占用期间
     * 只记住目标页面，等待传输结束后再执行页面绘制。 */
    return (uint8_t)(MicAud_IsStreaming() || CameraLink_Busy());
}

static void paint_page(uint8_t p, uint32_t uptime_s)
{
    if (page_work_deferred())
    {
        paint_pending = 1u;
        paint_want = p;
        return;
    }
    paint_pending = 0u; /* 当前请求可以立即绘制，取消此前暂存的旧页面。 */
    show_page(p, uptime_s);
}
/* ===  === */

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
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

    /* ----------  时基 + 串口（顺序不能反） ---------- */
    TIM6_Init();
    StartupTrace_Print("[START] TIM6 init returned\r\n");
    UART_Init();
    StartupTrace_Print("[START] UART init returned; PRIMASK=");
    StartupTrace_Hex(__get_PRIMASK());
    StartupTrace_Print(" VTOR=");
    StartupTrace_Hex(SCB->VTOR);
    StartupTrace_Print("\r\n[START] begin DMA printf\r\n");

    printf("\r\n===== 上电自检开始 CN-ASR-OTA-BOOT-DIAG2 =====\r\n");
    printf("[MAIN] Running firmware: %s\r\n", APP_FW_VERSION);
    printf("[MAIN] 调试串口 = USART2：PA2=TX / PA3=RX @115200\r\n");
    printf("[MAIN] （USART1 的 PA9/PA10 被屏挡住了，日志改走这里）\r\n");

    /* ---------- ESP8266链路（USART6：PC6=TX / PC7=RX，波特率由ESP_BAUD定义） ----------
     * 放在 LCD 之前：它只用 TIM6 时基 + printf，不碰屏/SD/I2C。
     *  这里没有"初始化失败"这回事（USART6 配置永远成功，双方也没握手）。
     *   链路通没通看状态页的 ESP: 一行，它以 esp_last_rx_ms 为准。
     *  ESP 的 VCC 必须独立 3.3V 供电（发射峰值 300mA+），别从 MCU 的 LDO 取。 */
    ESP_UART_Init();
    printf("[ESP] USART6 PC6/PC7 @%u init ok（帧解析在 App\\ailink.c）\r\n", (unsigned)ESP_BAUD);

    /* DHT11 读时会关中断约 25~35ms，初始化/诊断只做一次；周期读取放在
     * 主循环并避开 20MICAUD 流式录音，避免吃掉 DMA 环的半块。 */
    DHT11_Init();
    DHT11_Diag();

    /* ---------- c 板载双 LED PWM（PA6/PA7 = TIM3_CH1/CH2，低电平点亮） ----------
     *  TIM3 原来是 HC-SR04 输入捕获用的，本板已经不用超声波了（它占 PC8 =
     *   SDIO_D0，和 SD 卡抢线，计划里要求物理拔掉）⇒ TIM3 现在归 LED。
     *    **不要再调 HCSR04_Init()** —— 会抢同一个定时器。
     * 用途：网页「测试」页的 @LED_ON/@LED_OFF 两条默认快捷键（page-test.js）。 */
    LED_PWM_Init();
    printf("[LED] PA6/PA7 (TIM3 CH1/CH2) PWM ready\r\n");

    /* ---------- d ESP 下行帧解析器 ----------
     * 只打白名单清单，不碰硬件；真正的解析在主循环的 AILink_Poll() 里。
     * 放在 LCD 之前：它只需要 printf 和 TIM6 时基。 */
    AILink_Init();

    /* ---------- e 麦克风触发接口 ----------
     *  ：MIC_TRIG_SRC 已拨到 ESP（miclink.h:27），所以现在
     *   它能认网页/云端下发的 @MIC: 帧了；拨回 NONE 就仍然只认串口的 /rec。
     *   MicLink_Init() 会把当前触发源和认的帧打出来 —— 现场先看这一行。 */
    MicLink_Init();

    if (W25Q_SelfTest() != W25Q_OK)
    {
        printf("[MAIN] 板载 W25Q 自检没通过 —— 看上面 [W25Q] 那几行的排查顺序\r\n");
    }

    /* ---------- f-2 扩展字库装载（**只读**） ----------
     * 把 W25Q 上那份字库的索引取进 RAM（7510 B），顺带校验标记 + 头部 + 索引。
     *  全部只读：查标记、读 16 B 头部、读索引、探首末两个字模，**一次擦写都没有**
     *   —— 自检每按一次复位就跑，擦写等于白磨损。
     *  这里**失败是正常路径**，不是故障：没灌过库就是 `rc=1(NO_MARK)`，
     *   屏上照旧 `FONT:FLASH 32`，中文页与改造前逐字一致。
     *   真的坏了才需要看 `rc` —— `FontStore_ErrName()` 给的是人话。
     *  必须在 `W25Q_SelfTest()` **之后**：先确认这颗片子认得出来，
     *    否则这里报的 `rc=8(SPI)` 会是个二手症状，把矛头指错。 */
    {
        uint8_t frc = FontStore_Init();
        if (frc == FONTSTORE_OK)
        {
            printf("[FONT] W25Q 字库可用: %u 字 @0x%08X, 镜像 %u B, 索引 %u B 已入 RAM\r\n",
                   (unsigned)FontStore_Count(), (unsigned)FontStore_Base(),
                   (unsigned)FontStore_ImageBytes(), (unsigned)((uint32_t)FontStore_Count() * 2u));
        }
        else
        {
            printf("[FONT] ★ rc=%u(%s) —— 退回片内 %u 字（屏上 FONT:FLASH）\r\n", (unsigned)frc,
                   FontStore_ErrName(frc), (unsigned)Font_Count());
        }
    }
    /* ====================  ==================== */
    /* ---------- IAP初始化与布局诊断 ----------
     * 初始化应用侧升级状态机，此处不擦写固件，LCD未初始化也可输出诊断。
     * 固件上限由IAP_FW_SIZE_MAX决定：当前A槽224KiB、B槽256KiB，上限为224KiB。
     * OTA工程链接地址应为0x08008000；普通独立调试工程为0x08000000。
     * 输出META状态用于判断有无待安装镜像；真正的B到A搬运由Bootloader完成。
     * 分区改动须同步检查iap_flash.h、链接文件和Bootloader布局描述。 */
    IAP_AppInit();
    /* ====================  ==================== */

    /* ----------  LCD ---------- */
    printf("[MAIN] LCD init begin\r\n");
    LCD_Init(); /* 开头会先闪两下背光验极性 */
    LCD_SetServiceHook(AILink_ServiceMotion);
    printf("[MAIN] LCD init done\r\n");
    g_lcd_id = LCD_ReadID(); /* 验收判据：0x9341 */
    if (g_lcd_id != 0)
    {
        printf("[MAIN] LCD 识别成功：0x%04X\r\n", (unsigned)g_lcd_id);
    }
    else
    {
        printf("[MAIN] LCD 没读出 ID —— 看上面的 raw 值："
               "FFFF=RD/CS 没通，0000=屏没供电\r\n");
    }

    /* ----------  SD 卡 ---------- */
    g_sd_ret = Sd_Init(); /* SD_OK = 协议/容量/双速率读通过；签名单独报告 */
    if (g_sd_ret == SD_OK)
    {
        g_sd_sec = Sd_Get_Sector_Count();
        Sd_Get_Card_Info();
        g_sd_ret = read_sector0_sig(&g_sec0_sig);
        if (g_sd_ret == SD_OK)
        {
            printf("[MAIN] 扇区 0 末两字节 %02X %02X（%s）\r\n",
                   (unsigned)((g_sec0_sig >> 8) & 0xFF), (unsigned)(g_sec0_sig & 0xFF),
                   (g_sec0_sig == 0x55AAu) ? "0x55AA 引导签名，正常" : "不是 55AA，卡可能没格式化");
        }
        else
        {
            printf("[MAIN] 扇区 0 读失败\r\n");
        }

        /*  写通路自测：录 WAV 要往卡上写，而这张卡到目前**只读过**、
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
                g_sd_ret = wret; /* 写不通 ⇒ SD 这一路整体降级 */
                printf("[MAIN] ★ SD 写不通（r=%u）—— 录 WAV 会失败，"
                       "先别急着录，把上面那行 [SD] 的错贴出来\r\n",
                       (unsigned)wret);
            }
        }
    }
    else
    {
        printf("[MAIN] SD 初始化失败：ret=%u（0=OK 1=没卡 2=CMD8 3=ACMD41 "
               "4=电压 5=超时 6=读错 7=入参 8=卡应答）\r\n",
               (unsigned)g_sd_ret);
        printf("[MAIN] 先确认 HC-SR04 模块已拔掉（PC8 冲突）\r\n");
    }

    /* ----------  PCA9685（放最后：带超时，最坏只打一行错误） ---------- */
    g_pca_ok = (PCA9685_Init() == 0u) ? 1u : 0u;
    if (g_pca_ok)
    {
        /* 收到管理员配置前不发送固定角度的PWM，避免覆盖已标定的回正位置。
         * ESP会重发已保存的参数快照；AILink完成初始化及占用检查后，
         * 按配置将已登记的各轴回正一次。 */
        printf("[SERVO] startup home waits for saved profile from ESP; no forced 90 deg\r\n");
    }

    /* 先初始化两枚按键，再进入主循环接收消抖后的按键事件。 */
    KEY_UP_Init();
    KEY_OTA_Init();

    /* ----------  麦克风 INMP441（I2S2：PB10=CK PB9=WS PC3=SD） ----------
     * 放在最后：它内部会配 PLLI2S（I2S 的独立时钟源），跟前面几项互不相干。
     *  没接麦也不影响别的功能：I2S 照跑、只是收不到数据（SD 脚有上拉，
     *   读回恒定值），INMP441_Diag() 会把这个和"时钟没跑"分开报。
     *  想临时关掉麦克风，把下面 5 行注释掉即可（驱动本身可以留着不登记）。 */
    g_mic_ok = (INMP441_Init() == 0u) ? 1u : 0u;
    if (g_mic_ok)
    {
        INMP441_Start();
        g_mic_pin_error = INMP441_PinCheck();
        (void)INMP441_Calibrate(); /* 定标左声道在缓冲里的奇偶（没接麦就白等 ~50ms） */
        INMP441_Diag(); /* 把时钟链和数据线的实况打出来 —— 出问题先看这几行 */
    }

    /* ----------  扫卡；启动先显示含固件版本的状态页，K-UP 可切图片 ---------- */
    Media_Init(g_sd_ret == SD_OK);
    lcd_gui_hardware();
    page = PAGE_AI;
    show_page(page, 0u);
    printf("[GUI] LCD GUI enabled: FACE / DEVICE / MIC / SERVO / CAMERA; FW %s\r\n",
           APP_FW_VERSION);

    printf("[PWM] 命令已就绪：/pwm [id|all] [0~180|off]（id = 模块丝印号 1~16）\r\n");
    printf("[MAIN] 舵机自动扫动 = %s\r\n",
           (PCA_AUTO_SWEEP != 0) ? "开（裸测模式）" : "关（AI/命令驱动）");

    printf("===== 自检结束，进入主循环（画面靠 PE3 或 K-UP 切）=====\r\n\r\n");

    /* ---------- 主循环：先处理按键，再处理周期任务 ---------- */
    last_1s = GetTick_ms();
    last_5s = last_1s;
    last_mic = last_1s;
#if PCA_AUTO_SWEEP
    last_servo = last_1s;
#endif

    while (1)
    {
        uint32_t now = GetTick_ms();
        int rx_result;
        uint8_t request;
        uint8_t iap_handled = 0u; /* @IAP: 命令本圈是否已被消费 */
        /*  字库这条线的"本行已被吃"**改用我自己的变量**。
         *   原先借的是 02 的 `iap_handled`（当时为了不改他那行）—— 那是**两个模块
         *   共用一个判据**：任一边改语义、换判据或把变量挪走，另一边都会**静默**跟着变
         *   （这类"共用判据"在本项目已经踩过不止一次）。各留一个，各归各的。 */
        uint32_t font_gen = FontStore_Gen();
        uint8_t font_handled = 0u; /* /font: 命令本圈是否已被消费 */

        /* TIM6已消抖：主循环切页；图片/文字页不被秒表覆盖。
         *  ：**K-UP 改成一键两用** —— 有待确认的 OTA 申请时它当
         *   确认键用，没有申请时才是切页键（用户不必记"这次该按哪个键"）。
         *  `KEY_UP_Scan()` **只能取一次**：key.c:66 里它是**消费**式的（取走就 ++consumed，
         *   第二次必返 0）⇒ 必须先按键、再在里层问 OTA。写成两句 Scan() 会让切页键**失灵**。 */
        /* 两路事件分别读取一次并同时消费；同时按下时合并为一次操作。 */
        {
            uint8_t up_key = KEY_UP_Scan(), extra_key = KEY_OTA_Scan();
            if (up_key || extra_key)
            {
                AILink_UserActivity();
                if (IAP_ConfirmRequest())
                {
                    /* 有待确认的OTA申请时，任一实体按键都可确认本次升级。 */
                    Media_SetCaption("OTA CONFIRMED\nWAITING FOR TRANSFER");
                    page = PAGE_AI;
                    paint_pending = 0u;
                    show_page(page, uptime_s); /* 实体确认优先显示，不沿用此前等待绘制的页面。 */
                    printf("[KEY] OTA approved by physical K-UP/PE3 key\r\n");
                }
                else
                {
                    uint32_t paint_start = GetTick_ms();
                    key_count++;
                    page = Media_NextPage(page);
                    paint_page(page, uptime_s); /* 流式期间只记账不刷屏 */
                    printf(
                        "[KEY] #%lu page=%u (0=device 4=face 5=mic 6=servo 7=camera) draw=%lums\r\n",
                        (unsigned long)key_count, (unsigned)page,
                        (unsigned long)(GetTick_ms() - paint_start));
                }
            }
        }
        rx_result = UART_GetLine(rx_line, sizeof(rx_line));
        if (rx_result > 0)
            AILink_UserActivity();
        if (rx_result > 0 && MicLink_IsRecording())
        {
            if (!strncmp(rx_line, "/font", 5))
                printf("[FONT] ERR 4 (recording busy)\r\n");
            else
                printf("[RX] busy: recording; command rejected\r\n");
            rx_result = 0;
        }
        if (rx_result > 0)
        {
            if (strncmp(rx_line, "/font d ", 8))
                printf("[RX] %s\r\n", rx_line);
            /*  录音命令**必须先截**：Media_Command 对任何不认识的 `/xxx`
             *   都会回一句 "Unknown command" 并把行吃掉，轮不到 micrec。
             *   它不是录音命令时才返回 0，再交给 media 走原来那套。 */
            /* ===  ===
             *  IAP 命令**必须排在整条链最前面**：`@IAP:...` 不以 `/` 开头，
             *   落到 Media_Command 的兜底分支会被当成文字**上屏**且不报错。
             *  擦写中把命令**拒掉**（只拦 `@` 开头，`/xxx` 照旧）—— 不排队：
             *   排队会把命令拖到擦除之后才执行，语义就变了。 */
            if (FontUpload_Active() && !strncmp(rx_line, "@IAP:", 5))
            {
                printf("[IAP] ERR: font upload busy\r\n");
                iap_handled = 1u;
            }
            else
                iap_handled = IAP_AppCommand(rx_line);
            if (iap_handled && !strncmp(rx_line, "@IAP:REQUEST,", 13))
            {
                /* 提示可通过PE3或K-UP实体按键确认，串口命令本身不代替人工确认。 */
                Media_SetCaption("OTA REQUESTED\nPRESS PE3 OR K-UP");
                page = PAGE_AI;
                paint_pending = 0u;
                show_page(page, uptime_s); /* 串口发起的OTA申请同样立即显示提示，不等待普通舵机动作结束。 */
            }
            if (!iap_handled && IAP_IsBusy() && rx_line[0] == '@')
            {
                iap_handled = 1u;
                printf("[IAP] busy(%s)：命令被拒（擦写中 CPU 会停 1~4s，不排队）\r\n",
                       IAP_StateName());
            }
            /* ===  === */

            if (IAP_IsBusy() && !strncmp(rx_line, "/font", 5))
            {
                printf("[FONT] ERR 4 (IAP busy)\r\n");
                font_handled = 1u;
            }
            else
                font_handled = FontUpload_Command(rx_line);
            if (font_handled && FontStore_Gen() != font_gen)
                paint_page(page, uptime_s);

            /*  busy 闸：会话进行中（`begin` 之后、`end`/`abort` 之前）**别的命令一律拒掉**。
             *   判据用现成的 `FontUpload_Active()`（**能给判据就别给纪律**），不另记标志。
             *   为什么不排队、也不放行：
             *      一笔上传是**严格有序**的一串 `/font d`，PC 端靠"回包干净 + 每条回包
             *        带下一个期望 off"来自纠（丢包/重发/重复都能自愈）⇒ 中间插进来的命令
             *        会把自己的回包**混进这条应答流**，自纠的前提就没了；
             *      那些命令可能**长阻塞**（`/rec` 十几秒），把主循环卡住 ⇒ 后面的 `/font d`
             *        堆在 USART2 的环里、PC 端一条条超时重发，白白拖慢整包。
             *   拦的条件是 `rx_line[0] == '/'`，跟 02 那边"只拦 `@`"同一个理由：
             *   **只拦走这条链的东西**，别的模块的前缀（`@` 开头那些）不碰。
             *    `/font abort` 走上面那行、**不受本闸影响** ⇒ 会话卡住时永远解得开，
             *      这道闸不会把人锁死。 */
            if (!font_handled && (FontUpload_Active() != 0u) && rx_line[0] == '/')
            {
                font_handled = 1u;
                printf("[FONT] busy：一次上传正在进行中，本条命令被拒（不排队；"
                       "要中止用 /font abort）\r\n");
            }
            /*  不要将上面这道闸理解成"主循环被上传占住了" —— 没有：
             *   单条 `/font d` 只做 112 字节编程 + 回读（约 1ms），整包 1141 条
             *   是**一条一条过主循环**的；真阻塞只有 `begin` 的擦除（≤4s）和
             *   `end` 的 CRC（≈0.2s），那两处是在**这一行内部**卡住的。 */
            /* ===  === */
            /* 级联里多一个 `!font_handled`（只加这一个 token，语义不变）：
             *   被字库那行吃掉的 `/font` 命令**不能**再掉进下面的兜底分支。 */
            if (!iap_handled && !font_handled && !MicRec_Command(rx_line))
            {
                /*  顺序不能改：Media_Command 对任何不认识的 "/xxx" 都会回一句
                 *   "Unknown command" 并把**整行吃掉**，轮不到后面的模块。
                 *   ⇒ 新模块的命令必须串在 Media_Command **之前**。 */
                if (!PCA9685_Command(rx_line))
                {
                    request = Media_Command(rx_line);
                    if (request != PAGE_KEEP)
                    {
                        page = request;
                        paint_page(page, uptime_s); /* 流式期间只记账 */
                    }
                }
            }
        }
        else if (rx_result < 0)
            printf("[RX] Line discarded: overflow/error or >255 bytes; resend\r\n");

        /* --- ESP 下行帧：取尽 + 手势步进 + 静默窗合并重绘 ---
         *  跟 Media_Command 一个套路：返回 PAGE_KEEP = 不用切页；
         *   返回 PAGE_AI = "这批帧攒齐了，该重画 AI 页了"。
         *  顺序放在 UART_GetLine 之后：两条线互不相干，但先取完调试口
         *   的命令（优先级高、是人在敲），再处理 ESP 的批量帧。 */
        request = AILink_Poll();
        if (request != PAGE_KEEP)
        {
            page = request;
            paint_page(page, uptime_s); /* 流式期间只记账不刷屏 */
        }

        /* 统一推进自动语音触发和手动录音请求；返回值用于请求页面切换。
         * 录音期间的状态反馈由下方的局部文字条更新，避免持续整页刷新。 */
        request = MicLink_Tick(now);
        /* 录音状态变化且没有OTA占用时，切到表情页显示监听状态。
         * paint_page在音频或相机占用时只暂存请求，结束搬运后再绘制；
         * 录音结束时若仍在表情页则刷新，避免覆盖用户随后选择的其他页面。 */
        if (g_gui_audio_active != MicLink_IsRecording() && !IAP_IsBusy() && !AILink_OtaPending())
        {
            g_gui_audio_active = MicLink_IsRecording();
            if (g_gui_audio_active)
            {
                page = PAGE_AI;
                paint_page(page, uptime_s);
            }
            else if (page == PAGE_AI)
                paint_page(page, uptime_s);
        }
        lcd_audio_banner(0u);
        if (request != PAGE_KEEP)
        {
            page = request;
            paint_page(page, uptime_s);
        }
        /* ===  === */
        /* ===  ===
         * --- IAP / OTA 状态机推进（时基驱动）---
         *  **每圈都要调**，不是"只在 OTA 时"：`@IAP:BEGIN` 之后收数据、校验、
         *   提交、以及**延迟擦除**（到点才擦，不在中断里擦）都靠它推进。
         *   非 OTA 时它只读一个 static 就返回，代价可忽略。
         *  擦除就在**这一句里面**发生 —— CPU 会在这里被停住 1~4 秒。
         * ================================================================ */
        IAP_AppTick(now);
        /* ===  === */

        /* 方向驱动确认切换后，作废旧页面标记并按新横竖屏坐标重绘。
         * 字库传输期间不调用方向更新，录音、相机及OTA占用由驱动内部检查。 */
        if (!FontUpload_Active() && Gui_OrientationTick(GetTick_ms()))
        {
            g_painted_page = PAGE_KEEP;
            paint_page(page, uptime_s);
        }

        /* DHT11 采样成功后同时打印 USART2 并发送 @SENSOR: 给 ESP；录音期间
         * 禁止读取，保证 64ms 音频 DMA 环不会被 30ms 的单总线时序打断。 */
        if (!CameraLink_Busy() && !MicAud_IsStreaming() && !IAP_IsBusy() && !AILink_ServoBusy())
            DHT11_sys();

        /* --- 每 1 秒刷新屏上的秒表（只重画那一行，不整屏重画以免闪） --- */
        if (now - last_1s >= 1000u)
        {
            last_1s = now;
            uptime_s = (uint32_t)(now / 1000u); /* TIM6 从开机起算，直接除 */
            if (page == PAGE_STATUS && !CameraLink_Busy() && !MicAud_IsStreaming() && !IAP_IsBusy())
            {
                lcd_update_uptime(uptime_s);
                lcd_update_modules();
            }
            else if (!CameraLink_Busy() && !MicAud_IsStreaming() && !IAP_IsBusy())
                Gui_UpdateParameters(g_painted_page);
        }

        /* 每500ms输出麦克风音量、均方根和DMA进度，供接线与采样诊断。
         * 相机、流式录音、舵机动作及OTA占用时跳过本段，避免日志增加调度开销。
         * 音频结束后可结合下方的周期摘要检查最终块数和错误计数。 */
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
                 *  ndtr = DMA 还剩几个半字没搬完。**半块=0 而 ndtr 在跳** ⇒ 数据其实在搬，
                 *   问题在中断那条路，**别去查接线**（就在这儿栽过一次）。 */
                printf("[MIC] 音量=%u%% rms=%lu 峰峰=%ld 半块=%lu ndtr=%u\r\n",
                       (unsigned)INMP441_Level(), (unsigned long)INMP441_Rms(),
                       (long)INMP441_PeakToPeak(), (unsigned long)INMP441_Blocks(),
                       (unsigned)INMP441_DmaLeft());
            }
        }

        /* --- 每 5 秒重读一次扇区 0：证明 SD 是**持续**可读，不是碰巧过了一次 --- */
        if (now - last_5s >= 5000u && !CameraLink_Busy() && !MicAud_IsStreaming() &&
            !FontUpload_Active() && !AILink_ServoBusy() && !IAP_IsBusy())
        {
            uint32_t sig = 0;
            uint8_t ret;

            last_5s = now;

            /* 空闲时每5秒输出音频摘要；外层条件在录音、相机或OTA占用时跳过本段。
             * 观察已确认块数、重传和丢半块计数；有重传表示发生过重试，
             * 是否成功仍需结合最终块数和结束原因判断。 */
            MicAud_Diag();

            /* 只有已就绪的SD卡才周期读取；这里再次确认未处于流式录音状态。
             * 扇区读取是同步操作，不得移到音频占用检查之外。
             * 若占用使整个周期分支推迟，释放后按毫秒时基重新判断执行。 */
            if ((g_sd_ret == SD_OK) && (MicAud_IsStreaming() == 0u))
            {
                ret = read_sector0_sig(&sig);
                if (ret == SD_OK)
                {
                    printf("[MAIN] 重读扇区 0：末两字节 %04X %s\r\n", (unsigned)sig,
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
                    printf("[MAIN] 重读扇区 0 失败 ret=%u；停止周期读卡，复位后重试\r\n",
                           (unsigned)ret);
                    if (page == PAGE_STATUS)
                        lcd_show_status();
                }
            }
            printf("[MAIN] UP=%lus SD=%u PCA=%s PA0=%u KEY=%lu\r\n",
                   (unsigned long)(GetTick_ms() / 1000u), (unsigned)g_sd_ret,
                   g_pca_ok ? "OK" : "FAIL", (unsigned)KEY_UP_Level(), (unsigned long)key_count);
        }

        if (paint_pending && !page_work_deferred())
        {
            paint_pending = 0u;
            show_page(paint_want, uptime_s);
        }
        /* ===  === */

        /* 状态栏与动画按小片推进，各自检查音频、相机、OTA及字库占用后再访问SD/LCD。 */
        Gui_StatusTick(GetTick_ms(), 0u);
        Media_AnimationTick(g_painted_page, GetTick_ms());

#if PCA_AUTO_SWEEP
        /* --- 每 2 秒把 id 1/2/3 在 0° 和 180° 之间来回（老固件的裸测动作） ---
         *  默认关：见文件头 PCA_AUTO_SWEEP 那段。开着会把 AI 的角度扫走。 */
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
