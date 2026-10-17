/**
 * @file camera_ov7725.c
 * 用途：OV7725 SCCB 寄存器配置、帧同步、外置 FIFO 读取及色条诊断。
 * 使用：按 pins 头连接 8 位数据与控制线，Init 后请求一帧，等待 VSYNC 再控制 WEN/FIFO 读出。
 * 时序与边界：是带 FIFO 模块，不是直接 DCMI 连线；飞线时序/接触不良仍会花屏。不要把单帧测试说成实时视频。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "camera_ov7725.h"
#include "camera_ov7725_pins.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_gpio.h"
#include <stddef.h>
#include "stm32f4xx_exti.h"
#include "stm32f4xx_syscfg.h"
#include "misc.h"
#include "time.h"

#define CAM_MID 0x7FA2u
#define CAM_PID 0x7721u
#define CAM_REG_GAIN 0x00u
#define CAM_REG_PID 0x0Au
#define CAM_REG_VER 0x0Bu
#define CAM_REG_CLKRC 0x11u
#define CAM_REG_COM7 0x12u
#define CAM_REG_COM8 0x13u
#define CAM_REG_MIDH 0x1Cu
#define CAM_REG_MIDL 0x1Du
#define CAM_REG_HSTART 0x17u
#define CAM_REG_HSIZE 0x18u
#define CAM_REG_VSTRT 0x19u
#define CAM_REG_VSIZE 0x1Au
#define CAM_REG_HREF 0x32u
#define CAM_REG_HOUT 0x29u
#define CAM_REG_VOUT 0x2Cu
#define CAM_REG_COM3 0x0Cu
#define CAM_REG_DSP1 0x64u
#define CAM_REG_DSP2 0x65u
#define CAM_REG_COM4 0x0Du
#define CAM_REG_COM6 0x0Fu
#define CAM_REG_COM9 0x14u

typedef struct
{
    uint8_t reg;
    uint8_t value;
} Camera_Reg;

/* Full RGB565/QVGA register sequence from the supplied F103 case.
 * Only the GPIO/SCCB transport changes; no JPEG is produced by this sensor. */
static const Camera_Reg g_init_table[] = {
    /* Divide the sensor clock by two: conservative FIFO write timing. */
    {0x11u, 0x01u}, {0x12u, 0x46u}, {0x17u, 0x3Fu}, {0x18u, 0x50u}, {0x19u, 0x03u}, {0x1Au, 0x78u},
    {0x32u, 0x00u}, {0x29u, 0x50u}, {0x2Cu, 0x78u}, {0x42u, 0x7Fu}, {0x4Du, 0x09u}, {0x63u, 0xE0u},
    {0x64u, 0xFFu}, {0x65u, 0x00u}, {0x66u, 0x00u}, {0x67u, 0x00u}, {0x13u, 0xF0u}, {0x0Du, 0x81u},
    {0x0Fu, 0xC5u}, {0x14u, 0x11u}, {0x22u, 0x7Fu}, {0x23u, 0x03u}, {0x24u, 0x40u}, {0x25u, 0x30u},
    {0x26u, 0xA1u}, {0x2Bu, 0x9Eu}, {0x6Bu, 0xAAu}, {0x13u, 0xFFu}, {0x90u, 0x08u}, {0x91u, 0x01u},
    {0x92u, 0x03u}, {0x93u, 0x00u}, {0x94u, 0xB0u}, {0x95u, 0x9Du}, {0x96u, 0x13u}, {0x97u, 0x16u},
    {0x98u, 0x7Bu}, {0x99u, 0x91u}, {0x9Au, 0x1Eu}, {0x9Bu, 0x08u}, {0x9Cu, 0x20u}, {0x9Eu, 0x81u},
    {0xA6u, 0x06u}, {0xA7u, 0x65u}, {0xA8u, 0x65u}, {0xA9u, 0x80u}, {0xAAu, 0x80u}, {0x7Eu, 0x0Cu},
    {0x7Fu, 0x16u}, {0x80u, 0x2Au}, {0x81u, 0x4Eu}, {0x82u, 0x61u}, {0x83u, 0x6Fu}, {0x84u, 0x7Bu},
    {0x85u, 0x86u}, {0x86u, 0x8Eu}, {0x87u, 0x97u}, {0x88u, 0xA4u}, {0x89u, 0xAFu}, {0x8Au, 0xC5u},
    {0x8Bu, 0xD7u}, {0x8Cu, 0xE8u}, {0x8Du, 0x20u}, {0x0Cu, 0x50u}, {0x0Eu, 0xF5u},
};

static Camera_OV7725_Info g_info;
static uint8_t g_gpio_ready;
static volatile uint8_t g_capture_active, g_capture_edge;
static uint32_t g_capture_deadline, g_read_bytes;

#ifdef CAMERA_FIFO_FAST_IO
static uint8_t g_read_safe;
#define CAM_NOP8()                                                                                 \
    do                                                                                             \
    {                                                                                              \
        __NOP();                                                                                   \
        __NOP();                                                                                   \
        __NOP();                                                                                   \
        __NOP();                                                                                   \
        __NOP();                                                                                   \
        __NOP();                                                                                   \
        __NOP();                                                                                   \
        __NOP();                                                                                   \
    } while (0)
static void cam_fifo_wait(void)
{
    /* 168MHz: retain settling after the APB write; SCCB still uses cam_delay. */
    __DSB();
    CAM_NOP8();
    if (g_read_safe)
    {
        CAM_NOP8();
        CAM_NOP8();
        CAM_NOP8();
        CAM_NOP8();
        CAM_NOP8();
        CAM_NOP8();
        CAM_NOP8();
    }
}
static void cam_fifo_clock(uint8_t high)
{
#ifdef CAMERA_FIFO_IO_TEST
    GPIO_WriteBit(CAM_RCLK_PORT, CAM_RCLK_PIN, high ? Bit_SET : Bit_RESET);
#else
    /* Atomic pin writes: never touch other signals on GPIOD (LCD/FIFO). */
    if (high)
        CAM_RCLK_PORT->BSRRL = CAM_RCLK_PIN;
    else
        CAM_RCLK_PORT->BSRRH = CAM_RCLK_PIN;
#endif
    cam_fifo_wait();
}
void Camera_OV7725_SetReadSafe(uint8_t safe)
{
    g_read_safe = (safe != 0u);
}
#endif

static void cam_delay(void)
{
    volatile uint32_t n;
    /* Leave margin for module propagation and jumper-wire settling at 168MHz. */
    for (n = 0u; n < 96u; n++)
    {
    }
}

static void cam_sda_output(void)
{
    GPIO_InitTypeDef io;
    io.GPIO_Pin = CAM_SDA_PIN;
    io.GPIO_Mode = GPIO_Mode_OUT;
    io.GPIO_OType = GPIO_OType_OD;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    io.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(CAM_SDA_PORT, &io);
}

static void cam_sda_input(void)
{
    GPIO_InitTypeDef io;
    io.GPIO_Pin = CAM_SDA_PIN;
    io.GPIO_Mode = GPIO_Mode_IN;
    io.GPIO_OType = GPIO_OType_OD;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    io.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(CAM_SDA_PORT, &io);
}

static void cam_gpio_set(GPIO_TypeDef *port, uint16_t pin, BitAction value)
{
    GPIO_WriteBit(port, pin, value);
}

static void cam_scl(uint8_t high)
{
    cam_gpio_set(CAM_SCL_PORT, CAM_SCL_PIN, high ? Bit_SET : Bit_RESET);
    cam_delay();
}

static void cam_sda(uint8_t high)
{
    cam_gpio_set(CAM_SDA_PORT, CAM_SDA_PIN, high ? Bit_SET : Bit_RESET);
    cam_delay();
}

static uint8_t cam_sda_read(void)
{
    return GPIO_ReadInputDataBit(CAM_SDA_PORT, CAM_SDA_PIN) ? 1u : 0u;
}

static void cam_sccb_start(void)
{
    cam_sda_output();
    cam_sda(1u);
    cam_scl(1u);
    cam_sda(0u);
    cam_scl(0u);
}

static void cam_sccb_stop(void)
{
    cam_sda_output();
    cam_sda(0u);
    cam_scl(1u);
    cam_sda(1u);
}

static uint8_t cam_sccb_write_byte(uint8_t value)
{
    uint8_t i;
    uint8_t ack;
    cam_sda_output();
    for (i = 0u; i < 8u; i++)
    {
        cam_sda((value & 0x80u) != 0u);
        cam_scl(1u);
        cam_scl(0u);
        value <<= 1;
    }
    cam_sda_input();
    cam_scl(1u);
    ack = cam_sda_read();
    cam_scl(0u);
    return ack == 0u ? 1u : 0u;
}

static uint8_t cam_sccb_read_byte(uint8_t ack)
{
    uint8_t i;
    uint8_t value = 0u;
    cam_sda_input();
    for (i = 0u; i < 8u; i++)
    {
        cam_scl(1u);
        value = (uint8_t)((value << 1) | cam_sda_read());
        cam_scl(0u);
    }
    cam_sda_output();
    cam_sda(ack ? 0u : 1u);
    cam_scl(1u);
    cam_scl(0u);
    return value;
}

static uint8_t cam_reg_write(uint8_t reg, uint8_t value)
{
    uint8_t ok;
    cam_sccb_start();
    ok = cam_sccb_write_byte(0x42u);
    if (ok)
        ok = cam_sccb_write_byte(reg);
    if (ok)
        ok = cam_sccb_write_byte(value);
    cam_sccb_stop();
    return ok;
}

static uint8_t cam_reg_read(uint8_t reg, uint8_t *value)
{
    uint8_t ok;
    if (value == NULL)
        return 0u;
    cam_sccb_start();
    ok = cam_sccb_write_byte(0x42u);
    if (ok)
        ok = cam_sccb_write_byte(reg);
    cam_sccb_stop();
    if (!ok)
        return 0u;
    cam_sccb_start();
    ok = cam_sccb_write_byte(0x43u);
    if (ok)
        *value = cam_sccb_read_byte(0u);
    cam_sccb_stop();
    return ok;
}

static void cam_pin_output(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef io;
    io.GPIO_Pin = pin;
    io.GPIO_Mode = GPIO_Mode_OUT;
    io.GPIO_OType = GPIO_OType_PP;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    io.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(port, &io);
}

static void cam_gpio_init(void)
{
    GPIO_InitTypeDef io;
    EXTI_InitTypeDef exti;
    NVIC_InitTypeDef irq;
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOB | RCC_AHB1Periph_GPIOC |
                               RCC_AHB1Periph_GPIOD | RCC_AHB1Periph_GPIOE,
                           ENABLE);
    io.GPIO_Mode = GPIO_Mode_IN;
    io.GPIO_OType = GPIO_OType_PP;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    io.GPIO_PuPd = GPIO_PuPd_UP;
    io.GPIO_Pin = CAM_D0_PIN | CAM_D1_PIN | CAM_D2_PIN | CAM_D3_PIN;
    GPIO_Init(GPIOC, &io);
    io.GPIO_Pin = CAM_D4_PIN | CAM_D5_PIN | CAM_D6_PIN;
    GPIO_Init(GPIOE, &io);
    io.GPIO_Pin = CAM_D7_PIN;
    GPIO_Init(GPIOD, &io);
    io.GPIO_Pin = CAM_VSYNC_PIN;
    GPIO_Init(CAM_VSYNC_PORT, &io);
    cam_pin_output(CAM_SCL_PORT, CAM_SCL_PIN);
    cam_pin_output(CAM_RCLK_PORT, CAM_RCLK_PIN);
    cam_pin_output(CAM_WRST_PORT, CAM_WRST_PIN);
    cam_pin_output(CAM_WEN_PORT, CAM_WEN_PIN);
    cam_pin_output(CAM_RRST_PORT, CAM_RRST_PIN);
    cam_pin_output(CAM_OE_PORT, CAM_OE_PIN);
    cam_sda_output();
    cam_gpio_set(CAM_SCL_PORT, CAM_SCL_PIN, Bit_SET);
    cam_gpio_set(CAM_SDA_PORT, CAM_SDA_PIN, Bit_SET);
    cam_gpio_set(CAM_RCLK_PORT, CAM_RCLK_PIN, Bit_RESET);
    cam_gpio_set(CAM_WEN_PORT, CAM_WEN_PIN, Bit_RESET);
    cam_gpio_set(CAM_WRST_PORT, CAM_WRST_PIN, Bit_SET);
    cam_gpio_set(CAM_RRST_PORT, CAM_RRST_PIN, Bit_SET);
    cam_gpio_set(CAM_OE_PORT, CAM_OE_PIN, Bit_SET);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOA, EXTI_PinSource8);
    exti.EXTI_Line = EXTI_Line8;
    exti.EXTI_Mode = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Rising;
    exti.EXTI_LineCmd = ENABLE;
    EXTI_ClearITPendingBit(EXTI_Line8);
    EXTI_Init(&exti);
    irq.NVIC_IRQChannel = EXTI9_5_IRQn;
    irq.NVIC_IRQChannelPreemptionPriority = 2u;
    irq.NVIC_IRQChannelSubPriority = 1u;
    irq.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&irq);
    g_gpio_ready = 1u;
}

Camera_OV7725_Status Camera_OV7725_Probe(Camera_OV7725_Info *info)
{
    uint8_t hi, lo;
    if (!g_gpio_ready)
        cam_gpio_init();
    if (!cam_reg_read(CAM_REG_MIDH, &hi) || !cam_reg_read(CAM_REG_MIDL, &lo))
        return CAMERA_OV7725_ERR_BUS;
    g_info.manufacturer_id = (uint16_t)(((uint16_t)hi << 8) | lo);
    if (g_info.manufacturer_id != CAM_MID)
        return CAMERA_OV7725_ERR_MID;
    if (!cam_reg_read(CAM_REG_PID, &hi) || !cam_reg_read(CAM_REG_VER, &lo))
        return CAMERA_OV7725_ERR_BUS;
    g_info.product_id = (uint16_t)(((uint16_t)hi << 8) | lo);
    if (g_info.product_id != CAM_PID)
        return CAMERA_OV7725_ERR_PID;
    g_info.width = 320u;
    g_info.height = 240u;
    g_info.rgb565 = 1u;
    g_info.fifo_present = 1u;
    if (info != NULL)
        *info = g_info;
    return CAMERA_OV7725_OK;
}

Camera_OV7725_Status Camera_OV7725_Init(Camera_OV7725_Info *info)
{
    static const Camera_Reg verify[] = {{CAM_REG_COM7, 0x46u},  {CAM_REG_HSIZE, 0x50u},
                                        {CAM_REG_VSIZE, 0x78u}, {CAM_REG_HOUT, 0x50u},
                                        {CAM_REG_VOUT, 0x78u},  {CAM_REG_CLKRC, 0x01u}};
    uint16_t i;
    uint8_t value;
    Camera_OV7725_Status status;
    g_info.initialized = 0u;
    if (!g_gpio_ready)
        cam_gpio_init();
    status = Camera_OV7725_Probe(info);
    if (status != CAMERA_OV7725_OK)
        return status;
    if (!cam_reg_write(CAM_REG_COM7, 0x80u))
        return CAMERA_OV7725_ERR_BUS;
    {
        uint32_t at = GetTick_ms();
        while (GetTick_ms() - at < 50u)
        {
        }
    }
    for (i = 0u; i < (uint16_t)(sizeof(g_init_table) / sizeof(g_init_table[0])); i++)
        if (!cam_reg_write(g_init_table[i].reg, g_init_table[i].value))
            return CAMERA_OV7725_ERR_BUS;
    for (i = 0u; i < (uint16_t)(sizeof(verify) / sizeof(verify[0])); i++)
    {
        if (!cam_reg_read(verify[i].reg, &value))
            return CAMERA_OV7725_ERR_BUS;
        if (value != verify[i].value)
            return CAMERA_OV7725_ERR_CONFIG;
    }
    g_info.initialized = 1u;
    if (info != NULL)
        *info = g_info;
    return CAMERA_OV7725_OK;
}

static uint8_t cam_data_read(void)
{
#ifdef CAMERA_FIFO_FAST_IO
    /* Current PCB map only: three snapshots replace eight library calls. */
    uint32_t pc = GPIOC->IDR, pe = GPIOE->IDR, pd = GPIOD->IDR;
    return (uint8_t)((pc & 7u) | ((pc >> 1) & 8u) | ((pe << 3) & 16u) | (pe & 96u) |
                     ((pd << 4) & 128u));
#else
    uint8_t value = 0u;
    if (GPIO_ReadInputDataBit(CAM_D0_PORT, CAM_D0_PIN))
        value |= 0x01u;
    if (GPIO_ReadInputDataBit(CAM_D1_PORT, CAM_D1_PIN))
        value |= 0x02u;
    if (GPIO_ReadInputDataBit(CAM_D2_PORT, CAM_D2_PIN))
        value |= 0x04u;
    if (GPIO_ReadInputDataBit(CAM_D3_PORT, CAM_D3_PIN))
        value |= 0x08u;
    if (GPIO_ReadInputDataBit(CAM_D4_PORT, CAM_D4_PIN))
        value |= 0x10u;
    if (GPIO_ReadInputDataBit(CAM_D5_PORT, CAM_D5_PIN))
        value |= 0x20u;
    if (GPIO_ReadInputDataBit(CAM_D6_PORT, CAM_D6_PIN))
        value |= 0x40u;
    if (GPIO_ReadInputDataBit(CAM_D7_PORT, CAM_D7_PIN))
        value |= 0x80u;
    return value;
#endif
}

#ifdef CAMERA_FIFO_FAST_IO
Camera_OV7725_Status Camera_OV7725_SetColorBars(uint8_t enabled)
{
    uint8_t value;
    if (!g_info.initialized)
        return CAMERA_OV7725_ERR_NOT_READY;
    if (g_capture_active)
        return CAMERA_OV7725_ERR_BUSY;
    /* COM3 bit0 is the sensor test pattern; keep mirror/byte-order bits. */
    value = (uint8_t)(0x50u | (enabled ? 1u : 0u));
    if (!cam_reg_write(CAM_REG_COM3, value) || !cam_reg_read(CAM_REG_COM3, &value))
        return CAMERA_OV7725_ERR_BUS;
    return value == (uint8_t)(0x50u | (enabled ? 1u : 0u)) ? CAMERA_OV7725_OK
                                                           : CAMERA_OV7725_ERR_CONFIG;
}
Camera_OV7725_Status Camera_OV7725_Rewind(void)
{
    if (!g_capture_active || g_capture_edge < 2u)
        return CAMERA_OV7725_ERR_NOT_READY;
    /* Assert OE before the reference-case reset/prime sequence. Same FIFO,
     * WEN remains disabled: no new sensor frame may replace its contents. */
    cam_gpio_set(CAM_OE_PORT, CAM_OE_PIN, Bit_RESET);
    cam_gpio_set(CAM_RRST_PORT, CAM_RRST_PIN, Bit_RESET);
    cam_fifo_clock(0u);
    cam_fifo_clock(1u);
    cam_fifo_clock(0u);
    cam_gpio_set(CAM_RRST_PORT, CAM_RRST_PIN, Bit_SET);
    cam_fifo_clock(1u);
    g_read_bytes = 0u;
    g_info.frame_ready = 1u;
    return CAMERA_OV7725_OK;
}
#endif

/* First VSYNC starts writing, second stops: WEN is HIGH-active in
 * the user's FIFO case. IRQ catches narrow sync even during UART work. */
void EXTI9_5_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line8) != RESET)
    {
        EXTI_ClearITPendingBit(EXTI_Line8);
        if (g_capture_active && g_capture_edge == 0u)
        {
            cam_gpio_set(CAM_WRST_PORT, CAM_WRST_PIN, Bit_RESET);
            cam_delay();
            cam_gpio_set(CAM_WRST_PORT, CAM_WRST_PIN, Bit_SET);
            cam_gpio_set(CAM_WEN_PORT, CAM_WEN_PIN, Bit_SET);
            g_capture_edge = 1u;
        }
        else if (g_capture_active && g_capture_edge == 1u)
        {
            cam_gpio_set(CAM_WEN_PORT, CAM_WEN_PIN, Bit_RESET);
            /* Match the supplied FIFO case: park its write pointer after WEN
             * goes low. It stays disabled throughout all packet/ACK pauses. */
            cam_gpio_set(CAM_WRST_PORT, CAM_WRST_PIN, Bit_RESET);
            cam_delay();
            cam_gpio_set(CAM_WRST_PORT, CAM_WRST_PIN, Bit_SET);
            g_capture_edge = 2u;
        }
    }
}
Camera_OV7725_Status Camera_OV7725_CaptureStart(uint32_t timeout_ms)
{
    if (!g_info.initialized)
        return CAMERA_OV7725_ERR_NOT_READY;
    if (g_capture_active)
        return CAMERA_OV7725_ERR_BUSY;
    if (!timeout_ms || timeout_ms > 5000u)
        return CAMERA_OV7725_ERR_ARG;
    cam_gpio_set(CAM_WEN_PORT, CAM_WEN_PIN, Bit_RESET);
    g_info.frame_ready = 0u;
    g_read_bytes = 0u;
    g_capture_edge = 0u;
    g_capture_deadline = GetTick_ms() + timeout_ms;
    EXTI_ClearITPendingBit(EXTI_Line8);
    g_capture_active = 1u;
    return CAMERA_OV7725_OK;
}
Camera_OV7725_Status Camera_OV7725_CapturePoll(uint32_t now)
{
    if (!g_capture_active)
        return CAMERA_OV7725_ERR_NOT_READY;
    if (g_info.frame_ready)
        return CAMERA_OV7725_OK;
    if (g_capture_edge < 2u)
    {
        if ((int32_t)(now - g_capture_deadline) >= 0)
        {
            (void)Camera_OV7725_CaptureStop();
            return CAMERA_OV7725_ERR_TIMEOUT;
        }
        return CAMERA_OV7725_ERR_BUSY;
    }
#ifdef CAMERA_FIFO_FAST_IO
    return Camera_OV7725_Rewind();
#else
    /* AL422 read reset must see RCLK while RRST is low (same as case). */
    cam_gpio_set(CAM_RRST_PORT, CAM_RRST_PIN, Bit_RESET);
    cam_gpio_set(CAM_RCLK_PORT, CAM_RCLK_PIN, Bit_RESET);
    cam_delay();
    cam_gpio_set(CAM_RCLK_PORT, CAM_RCLK_PIN, Bit_SET);
    cam_delay();
    cam_gpio_set(CAM_RCLK_PORT, CAM_RCLK_PIN, Bit_RESET);
    cam_delay();
    cam_gpio_set(CAM_RRST_PORT, CAM_RRST_PIN, Bit_SET);
    cam_gpio_set(CAM_RCLK_PORT, CAM_RCLK_PIN, Bit_SET);
    cam_delay();
    cam_gpio_set(CAM_OE_PORT, CAM_OE_PIN, Bit_RESET);
    g_info.frame_ready = 1u;
    return CAMERA_OV7725_OK;
#endif
}

Camera_OV7725_Status Camera_OV7725_ReadChunk(uint8_t *dst, uint16_t capacity, uint16_t *read_count)
{
    uint16_t i;
    if (read_count != NULL)
        *read_count = 0u;
    if (dst == NULL || read_count == NULL || capacity == 0u)
        return CAMERA_OV7725_ERR_ARG;
    if (!g_capture_active || !g_info.frame_ready)
        return CAMERA_OV7725_ERR_NOT_READY;
    if (capacity > 153600u - g_read_bytes)
        return CAMERA_OV7725_ERR_ARG;
    for (i = 0u; i < capacity; i++)
    {
#ifdef CAMERA_FIFO_FAST_IO
        cam_fifo_clock(0u);
        dst[i] = cam_data_read();
        cam_fifo_clock(1u);
#else
        cam_gpio_set(CAM_RCLK_PORT, CAM_RCLK_PIN, Bit_RESET);
        cam_delay();
        dst[i] = cam_data_read();
        cam_gpio_set(CAM_RCLK_PORT, CAM_RCLK_PIN, Bit_SET);
        cam_delay();
#endif
    }
    g_read_bytes += capacity;
    *read_count = capacity;
    return CAMERA_OV7725_OK;
}

Camera_OV7725_Status Camera_OV7725_CaptureStop(void)
{
    if (!g_capture_active)
        return CAMERA_OV7725_ERR_NOT_READY;
    g_capture_active = 0u;
    cam_gpio_set(CAM_WEN_PORT, CAM_WEN_PIN, Bit_RESET);
    cam_gpio_set(CAM_OE_PORT, CAM_OE_PIN, Bit_SET);
    g_capture_active = 0u;
    g_info.frame_ready = 0u;
    return CAMERA_OV7725_OK;
}

uint32_t Camera_OV7725_PinCheck(void)
{
    uint32_t bad = 0u;
    if (!g_gpio_ready)
        return 0xFFFFFFFFu;
    if (((CAM_SCL_PORT->MODER >> 16) & 3u) != 1u)
        bad |= 1u;
    if (((CAM_SDA_PORT->MODER >> 22) & 3u) != 1u)
        bad |= 2u;
    if (((CAM_VSYNC_PORT->MODER >> 16) & 3u) != 0u)
        bad |= 4u;
    return bad;
}

uint8_t Camera_OV7725_IsFrameReady(void)
{
    return g_info.frame_ready;
}
const Camera_OV7725_Info *Camera_OV7725_GetInfo(void)
{
    return &g_info;
}
const char *Camera_OV7725_StatusName(Camera_OV7725_Status status)
{
    switch (status)
    {
    case CAMERA_OV7725_OK:
        return "OK";
    case CAMERA_OV7725_ERR_BUS:
        return "SCCB_BUS";
    case CAMERA_OV7725_ERR_MID:
        return "MID";
    case CAMERA_OV7725_ERR_PID:
        return "PID";
    case CAMERA_OV7725_ERR_TIMEOUT:
        return "TIMEOUT";
    case CAMERA_OV7725_ERR_NOT_READY:
        return "NOT_READY";
    case CAMERA_OV7725_ERR_CONFIG:
        return "CONFIG_READBACK";
    case CAMERA_OV7725_ERR_ARG:
        return "ARG";
    default:
        return "ERROR";
    }
}
