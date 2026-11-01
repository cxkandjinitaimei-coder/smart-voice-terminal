/* SD 卡驱动：STM32F407 SDIO 原生模式，轮询读取，无 DMA/中断。
 * 引脚与冲突见 sd.h；printf 需先 UART_Init()。
 * CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3(RCA) -> CMD9 -> CMD7 -> CMD13
 * -> SDSC CMD16 -> 400kHz 读验证 -> 工作时钟读验证。
 * CMD58 属于 SPI 模式，本驱动不发送。OCR 直接来自 ACMD41 的 R3。
 */
#include "sd.h"
#include <stdio.h>
#include <string.h>

#define SD_CMD_TIMEOUT  0x00080000u
#define SD_DATA_TIMEOUT 0x00400000u
#define SD_ACMD41_RETRY 2000u
#define SD_ACMD41_VDD   0x00FF8000u
#define SD_ACMD41_HCS   0x40000000u
#define SD_R1_ERRORS    0xFDFFE008u /* ST BSP 的 R1 错误位，不能套给 R6/R7/R3 */
#define SD_CMD_FLAGS (SDIO_FLAG_CCRCFAIL | SDIO_FLAG_CTIMEOUT | SDIO_FLAG_CMDREND | SDIO_FLAG_CMDSENT)
#define SD_DATA_ERRORS (SDIO_FLAG_DCRCFAIL | SDIO_FLAG_DTIMEOUT | SDIO_FLAG_RXOVERR | SDIO_FLAG_STBITERR)
#define SD_DATA_FLAGS (SD_DATA_ERRORS | SDIO_FLAG_DATAEND | SDIO_FLAG_DBCKEND)
/* ★ 写方向的错误位和读方向不一样：RXOVERR（接收溢出）换成 TXUNDERR（发送下溢，
 *   即主机往 FIFO 灌数太慢、卡等不到数据放弃了）。所以 SD_DATA_ERRORS 不能拿来
 *   判断写，必须单独一套；SD_DATA_FLAGS 也清不掉 TXUNDERR，得显式加进去清。 */
#define SD_WRITE_ERRORS (SDIO_FLAG_DCRCFAIL | SDIO_FLAG_DTIMEOUT | SDIO_FLAG_TXUNDERR | SDIO_FLAG_STBITERR)

static u8 s_card_type, s_ready;
static u16 s_rca;
static u32 s_ocr, s_capacity_kb, s_sector_count;

/* 上电与初始化重试使用的粗延时，不依赖应用时基。 */
static void sd_delay_ms(u16 nms)
{
    volatile u32 i;
    while(nms--)
        for(i = 0; i < 14000; i++);
}

static void sd_gpio_config(void)
{
    GPIO_InitTypeDef gpio;

    RCC_AHB1PeriphClockCmd(SD_GPIO_CLK, ENABLE);

    GPIO_PinAFConfig(SD_CK_PORT,  GPIO_PinSource12, SD_GPIO_AF);
    GPIO_PinAFConfig(SD_CMD_PORT, GPIO_PinSource2,  SD_GPIO_AF);
    GPIO_PinAFConfig(SD_D0_PORT,  GPIO_PinSource8,  SD_GPIO_AF);
#if SD_BUS_WIDTH_4BIT
    GPIO_PinAFConfig(SD_D1_PORT,  GPIO_PinSource9,  SD_GPIO_AF);
    GPIO_PinAFConfig(SD_D2_PORT,  GPIO_PinSource10, SD_GPIO_AF);
    GPIO_PinAFConfig(SD_D3_PORT,  GPIO_PinSource11, SD_GPIO_AF);
#endif

    gpio.GPIO_Mode  = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    /* 板上只有 100K 上拉（偏弱），这里把 MCU 内部 40K 上拉也打开补强 */
    gpio.GPIO_PuPd  = GPIO_PuPd_UP;

    gpio.GPIO_Pin = SD_CK_PIN;
    GPIO_Init(SD_CK_PORT, &gpio);
    gpio.GPIO_Pin = SD_CMD_PIN;
    GPIO_Init(SD_CMD_PORT, &gpio);
    gpio.GPIO_Pin = SD_D0_PIN;
    GPIO_Init(SD_D0_PORT, &gpio);
#if SD_BUS_WIDTH_4BIT
    gpio.GPIO_Pin = SD_D1_PIN | SD_D2_PIN | SD_D3_PIN;   /* 都在 GPIOC */
    GPIO_Init(SD_D1_PORT, &gpio);
#endif
}

/* SDIO 外设复位（只在 Sd_Init 最初调一次；之后改速率/位宽不能复位，
 * 否则会打断与卡之间已经协商好的状态） */
static void sd_periph_reset(void)
{
    RCC_APB2PeriphResetCmd(SD_SDIO_CLK, ENABLE);
    RCC_APB2PeriphResetCmd(SD_SDIO_CLK, DISABLE);
    RCC_APB2PeriphClockCmd(SD_SDIO_CLK, ENABLE);

    SDIO_SetPowerState(SDIO_PowerState_ON);
    /* 注意：这里**不**开 SDIO 时钟。CLKCR 复位后的分频是 0（48/2 = 24MHz），
     * 先开时钟会往卡上打一段 24MHz，违反"初始化期间必须 ≤400kHz"。
     * 调用者要先把 400kHz 的分频写进 CLKCR，再 SDIO_ClockCmd(ENABLE)。 */
}

/* 配 SDIO 时钟分频与总线宽度（可重复调用：提速、切 4 位都走它）
 * clk_div 用 u8，和 SDIO_InitTypeDef.SDIO_ClockDiv 的类型一致，避免隐式截断告警 */
static void sd_periph_config(u8 clk_div, u32 bus_wide)
{
    SDIO_InitTypeDef init;

    SDIO_StructInit(&init);
    init.SDIO_ClockDiv  = clk_div;          /* SDIO_CK = 48MHz / (clk_div + 2) */
    init.SDIO_ClockEdge = SDIO_ClockEdge_Rising;
    init.SDIO_BusWide   = bus_wide;
    SDIO_Init(&init);
}

/* 命令层只清命令标志：CMD17 发出前 DPSM 已开启，不能清数据错误。
 * 只有 R3 无 CRC；R7(CMD8) 有 CRC，必须检查。
 * R1/R6/R7 的应答命令号应匹配；R2/R3 的保留字段不作命令号检查。
 *
 * quiet 参数：把"命令超时"当**正常情况**处理，不打印 [SD] CMDn failed 那行。
 *   ★ 什么场合需要：卡**还在忙**的时候 —— 刚写完一个数据块，它正把数据写进
 *     内部 flash，这期间会拉住 DAT0 且对任何命令都不应答（SD 规范里每条命令
 *     的命令超时只有 64 个 SDIO_CK，等不到这个忙）。写入路径靠反复发 CMD13
 *     探它"好没好"，那时超时是意料之中的，打出来只会刷屏。
 *   ★ 曾经想用 CMD 寄存器的 WAITPEND（SDIO_Wait_Pend）让 CPSM 硬件去等，
 *     结果**上机直接挂**：RM0090 里 WAITPEND 等的是"**数据传输**结束"，
 *     而那条路径上 DPSM 已经关掉、没有传输可等 ⇒ CPSM 永远等下去，
 *     STA 停在 CMDACT(0x800)、CMD13 根本没发出去，SD 初始化直接失败。
 *     所以只能用"发了超时再重试"这种笨办法。
 */
static u8 sd_send_cmd(u8 cmd, u32 arg, u32 kind, u32 *resp, u8 no_crc, u8 quiet)
{
    SDIO_CmdInitTypeDef c;
    u32 t = SD_CMD_TIMEOUT, sta = 0, value;
    u8 err = SD_ERR_TIMEOUT;
    if(resp) *resp = 0;
    SDIO->ICR = SD_CMD_FLAGS;
    c.SDIO_Argument = arg;
    c.SDIO_CmdIndex = cmd;
    c.SDIO_Response = kind;
    c.SDIO_Wait = SDIO_Wait_No;   /* ★ 不要改成 SDIO_Wait_Pend，见上面的说明 */
    c.SDIO_CPSM = SDIO_CPSM_Enable;
    SDIO_SendCommand(&c);
    while(t--)
    {
        sta = SDIO->STA;
        if(kind == SDIO_Response_No)
        {
            if(sta & SDIO_FLAG_CMDSENT) return SD_OK;
            continue;
        }
        if(sta & SDIO_FLAG_CTIMEOUT) break;
        if(!(sta & (SDIO_FLAG_CCRCFAIL | SDIO_FLAG_CMDREND))) continue;
        if((sta & SDIO_FLAG_CCRCFAIL) && !no_crc)
        {
            err = SD_ERR_READ;
            break;
        }
        value = SDIO_GetResponse(SDIO_RESP1);
        if(resp) *resp = value;
        if(kind == SDIO_Response_Short && !no_crc && SDIO_GetCommandResponse() != cmd)
        {
            err = SD_ERR_RESPONSE;
            break;
        }
        return SD_OK;
    }
    if(!quiet)
        printf("[SD] CMD%u failed: r=%u STA=0x%08lX RESPCMD=%u\r\n",
               (unsigned)cmd, (unsigned)err, (unsigned long)sta,
               (unsigned)SDIO_GetCommandResponse());
    return err;
}

/* R1 卡状态错误与传输成功是两回事：CMDREND 不代表卡接受了命令。 */
static u8 sd_r1(u8 cmd, u32 arg, u32 *resp)
{
    u32 value;
    u8 r = sd_send_cmd(cmd, arg, SDIO_Response_Short, &value, 0, 0);
    if(resp) *resp = value;
    if(r != SD_OK) return r;
    if(value & SD_R1_ERRORS)
    {
        printf("[SD] CMD%u card error: R1=0x%08lX\r\n", (unsigned)cmd, (unsigned long)value);
        return SD_ERR_RESPONSE;
    }
    return SD_OK;
}

/* CMD7 是 R1b；写完一块之后卡也会拉 DAT0 忙。
 * 用 CMD13 等到 READY_FOR_DATA 且 CURRENT_STATE=TRAN：
 * 0x1F00 同时罩住 bit8(READY_FOR_DATA) 和 bit12:9(CURRENT_STATE)，
 * 0x0900 = READY_FOR_DATA=1 且 CURRENT_STATE=4(TRAN)。
 *
 * ★★ 这里的 CMD13 **超时是正常的、必须当"还没好"重试**，不能当错误：
 *    卡在写内部 flash 时对任何命令都不应答，而命令超时只有 64 个 SDIO_CK。
 *    所以第一次探测十有八九是 CTIMEOUT。若直接把超时当失败返回，
 *    就会**数据其实写成功了、却报"写失败"**。
 *    （试过让 CPSM 硬件去等 —— SDIO_Wait_Pend —— 上机直接挂死，
 *      原因见 sd_send_cmd 上面的说明，那条路走不通。）
 * 只有非超时的错误才立刻返回。 */
static u8 sd_wait_transfer(void)
{
    u16 i;
    u32 value;
    u8 r;
    for(i = 0; i < 500; i++)
    {
        r = sd_send_cmd(13, (u32)s_rca << 16, SDIO_Response_Short, &value, 0, 1); /* quiet */
        if(r == SD_OK)
        {
            if(value & SD_R1_ERRORS)
            {
                printf("[SD] CMD13 card error: R1=0x%08lX\r\n", (unsigned long)value);
                return SD_ERR_RESPONSE;
            }
            if((value & 0x1F00u) == 0x0900u) return SD_OK;
        }
        else if(r != SD_ERR_TIMEOUT)
        {
            return r;                    /* 真错误（CRC/应答号不对）立刻上报 */
        }
        /* r == SD_ERR_TIMEOUT ⇒ 卡还忙，睡 1ms 再探 */
        sd_delay_ms(1);
    }
    printf("[SD] CMD13 not ready: R1=0x%08lX\r\n", (unsigned long)value);
    return SD_ERR_TIMEOUT;
}

static u8 sd_acmd41(u32 arg)
{
    u16 i;
    u32 value;
    u8 r;
    for(i = 0; i < SD_ACMD41_RETRY; i++)
    {
        r = sd_r1(55, 0, &value);
        if(r != SD_OK) return r;
        if(!(value & 0x20u)) return SD_ERR_RESPONSE; /* APP_CMD */
        r = sd_send_cmd(41, arg, SDIO_Response_Short, &value, 1, 0);
        if(r != SD_OK) return r;
        if(value & 0x80000000u)
        {
            s_ocr = value;
            printf("[SD] ACMD41 ready after %u polls: OCR=0x%08lX\r\n",
                   (unsigned)(i + 1), (unsigned long)value);
            return SD_OK;
        }
        sd_delay_ms(2);
    }
    printf("[SD] ACMD41 still busy: OCR=0x%08lX\r\n", (unsigned long)value);
    return SD_ERR_ACMD41;
}

static u32 sd_csd_bits(const u8 *csd, u8 msb, u8 lsb)
{
    u32 v = 0;
    int i;
    for(i = (int)msb; i >= (int)lsb; i--)
        v = (v << 1) | ((csd[15 - (i >> 3)] >> (i & 7)) & 1u);
    return v;
}

/* CMD9 必须在 CMD3 后、CMD7 前，参数为卡发布的 RCA << 16。 */
static u8 sd_read_capacity(void)
{
    u8 csd[16], i, r;
    u32 w[4], structure, size, mult, block;
    unsigned long long sectors;
    r = sd_send_cmd(9, (u32)s_rca << 16, SDIO_Response_Long, 0, 0, 0);
    if(r != SD_OK) return r;
    w[0] = SDIO_GetResponse(SDIO_RESP1);
    w[1] = SDIO_GetResponse(SDIO_RESP2);
    w[2] = SDIO_GetResponse(SDIO_RESP3);
    w[3] = SDIO_GetResponse(SDIO_RESP4);
    for(i = 0; i < 16; i++) csd[i] = (u8)(w[i / 4] >> (24 - 8 * (i % 4)));
    structure = sd_csd_bits(csd, 127, 126);
    if(structure == 1 && s_card_type == 3)
        sectors = ((unsigned long long)sd_csd_bits(csd, 69, 48) + 1) * 1024;
    else if(structure == 0 && s_card_type != 3)
    {
        size = sd_csd_bits(csd, 73, 62);
        mult = sd_csd_bits(csd, 49, 47);
        block = sd_csd_bits(csd, 83, 80);
        sectors = (((unsigned long long)size + 1) << (mult + 2 + block)) >> 9;
    }
    else
    {
        printf("[SD] CMD9 unsupported CSD structure=%lu card_type=%u\r\n",
               (unsigned long)structure, (unsigned)s_card_type);
        return SD_ERR_RESPONSE;
    }
    /* 当前接口/FatFs 是 32 位扇区计数，拒绝溢出而不是报容量 0。 */
    if(sectors == 0 || sectors > 0xFFFFFFFFu)
    {
        printf("[SD] CMD9 capacity outside 32-bit sector interface\r\n");
        return SD_ERR_PARAM;
    }
    s_sector_count = (u32)sectors;
    s_capacity_kb = s_sector_count / 2;
    return SD_OK;
}

/* CMD17 轮询接收：检查所有数据错误，支持 FatFs 的非对齐字节缓冲。
 * 失败/成功均关闭 DPSM、排空 FIFO、清数据标志，避免下一次读到旧数据。
 */
static u8 sd_read_one(u32 sector, u8 *buf)
{
    SDIO_DataInitTypeDef d;
    u32 t = SD_DATA_TIMEOUT, sta = 0, word;
    u16 n = 0;
    u8 r;
    SDIO->DCTRL = 0;
    while(SDIO->STA & SDIO_FLAG_RXDAVL) (void)SDIO_ReadData();
    SDIO->ICR = SD_DATA_FLAGS;
    SDIO_DataStructInit(&d);
    d.SDIO_DataTimeOut = 0x001FFFFFu;
    d.SDIO_DataLength = 512;
    d.SDIO_DataBlockSize = SDIO_DataBlockSize_512b;
    d.SDIO_TransferDir = SDIO_TransferDir_ToSDIO;
    d.SDIO_TransferMode = SDIO_TransferMode_Block;
    d.SDIO_DPSM = SDIO_DPSM_Enable;
    SDIO_DataConfig(&d);
    r = sd_r1(17, s_card_type == 3 ? sector : sector << 9, 0);
    if(r != SD_OK) goto done;
    r = SD_ERR_TIMEOUT;
    while(t--)
    {
        sta = SDIO->STA;
        if(sta & SD_DATA_ERRORS)
        {
            r = (sta & SDIO_FLAG_DTIMEOUT) ? SD_ERR_TIMEOUT : SD_ERR_READ;
            break;
        }
        if(sta & SDIO_FLAG_RXDAVL)
        {
            if(n == 128) { r = SD_ERR_READ; break; }
            word = SDIO_ReadData();
            memcpy(buf + n * 4, &word, 4);
            n++;
        }
        /* DATAEND 仅表示计数归零；DBCKEND 才证明块 CRC 已校验完成。 */
        else if(sta & SDIO_FLAG_DBCKEND)
        {
            r = n == 128 ? SD_OK : SD_ERR_READ;
            break;
        }
    }
done:
    sta = SDIO->STA;
    SDIO->DCTRL = 0;
    while(SDIO->STA & SDIO_FLAG_RXDAVL) (void)SDIO_ReadData();
    SDIO->ICR = SD_DATA_FLAGS;
    if(r != SD_OK)
        printf("[SD] read sector %lu failed: r=%u words=%u STA=0x%08lX\r\n",
               (unsigned long)sector, (unsigned)r, (unsigned)n, (unsigned long)sta);
    return r;
}

/* 单块写（CMD24），512 字节。与 sd_read_one 的三处关键差别：
 *   ① 方向是 ToCard：主机往 FIFO **灌**数据（SDIO_WriteData），不是取；
 *      往 FIFO 里塞数据的时机是 TXFIFOHE（FIFO 至少空了一半）——
 *      此时一次塞 8 个字（32 字节）是安全的。
 *   ② 错位是 SD_WRITE_ERRORS（含 TXUNDERR），不是读那套。
 *   ③ ★★ 数据发完 ≠ 写完了：卡收下块和 CRC 后会**拉住 DAT0 表示忙**
 *      （内部在写 flash），这期间它对任何命令都不应答。所以判"真写完了"
 *      要等 **DBCKEND && !TXACT** —— DBCKEND 只说明块 CRC 被卡接受，
 *      而 TXACT 清掉才说明 DPSM 从 Busy 回到 Idle、卡的忙等结束了。
 *      ST 官方 BSP 的 SD_WaitWriteOperation() 等的就是 TXACT。 */
static u8 sd_write_one(u32 sector, const u8 *buf)
{
    SDIO_DataInitTypeDef d;
    u32 t = SD_DATA_TIMEOUT, sta = 0, word;
    u16 n = 0;
    u8 r;
    SDIO->DCTRL = 0;
    while(SDIO->STA & SDIO_FLAG_RXDAVL) (void)SDIO_ReadData();
    SDIO->ICR = SD_DATA_FLAGS | SDIO_FLAG_TXUNDERR;
    SDIO_DataStructInit(&d);
    d.SDIO_DataTimeOut = 0x001FFFFFu;
    d.SDIO_DataLength = 512;
    d.SDIO_DataBlockSize = SDIO_DataBlockSize_512b;
    d.SDIO_TransferDir = SDIO_TransferDir_ToCard;
    d.SDIO_TransferMode = SDIO_TransferMode_Block;
    d.SDIO_DPSM = SDIO_DPSM_Enable;
    SDIO_DataConfig(&d);
    r = sd_r1(24, s_card_type == 3 ? sector : sector << 9, 0);
    if(r != SD_OK) goto done;
    r = SD_ERR_TIMEOUT;
    while(t--)
    {
        sta = SDIO->STA;
        if(sta & SD_WRITE_ERRORS)
        {
            r = (sta & SDIO_FLAG_DTIMEOUT) ? SD_ERR_TIMEOUT : SD_ERR_READ;
            break;
        }
        if((n < 128u) && (sta & SDIO_FLAG_TXFIFOHE))
        {
            u8 k;
            for(k = 0; (k < 8u) && (n < 128u); k++)
            {
                memcpy(&word, buf + n * 4, 4);   /* memcpy：buf 不保证 4 字节对齐 */
                SDIO_WriteData(word);
                n++;
            }
            continue;
        }
        /* 缺一不可：DBCKEND = 块 CRC 被卡接受；!TXACT = 卡的内部写也结束了。 */
        if((sta & SDIO_FLAG_DBCKEND) && !(sta & SDIO_FLAG_TXACT))
        {
            r = n == 128u ? SD_OK : SD_ERR_READ;
            break;
        }
    }
done:
    sta = SDIO->STA;
    SDIO->DCTRL = 0;
    SDIO->ICR = SD_DATA_FLAGS | SDIO_FLAG_TXUNDERR;
    if(r != SD_OK)
        printf("[SD] write sector %lu failed: r=%u words=%u STA=0x%08lX\r\n",
               (unsigned long)sector, (unsigned)r, (unsigned)n, (unsigned long)sta);
    return r;
}

u8 Sd_Init(void)
{
    u32 resp, bus = SDIO_BusWide_1b;
    u8 r, candidate, pass;
    static const u8 dividers[]={SD_TRANSFER_CLK_DIV,2u,6u};
    static u8 slow[512], fast[512];
    s_ready = 0;
    s_card_type = 0;
    s_rca = 0;
    s_ocr = s_capacity_kb = s_sector_count = 0;
    printf("\r\n[SD] native SDIO fix 2026-09-14: CMD2/CMD3/CMD9/CMD7\r\n");
    printf("[SD] PC12=CK PD2=CMD PC8=D0; unplug HC-SR04\r\n");
    sd_gpio_config();
    sd_periph_reset();
    sd_periph_config(SD_INIT_CLK_DIV, SDIO_BusWide_1b);
    SDIO_ClockCmd(ENABLE);
    sd_delay_ms(5);
    r = sd_send_cmd(0, 0, SDIO_Response_No, 0, 0, 0);
    if(r != SD_OK) goto fail;
    printf("[SD] CMD0 sent (no card response expected)\r\n");
    r = sd_send_cmd(8, 0x1AA, SDIO_Response_Short, &resp, 0, 0);
    if(r == SD_ERR_TIMEOUT)
    {
        s_card_type = 1;
        printf("[SD] CMD8 timeout: try SD v1 with voltage window\r\n");
    }
    else
    {
        if(r != SD_OK) goto fail;
        if((resp & 0xFFFu) != 0x1AAu)
        {
            printf("[SD] CMD8 bad echo: 0x%08lX\r\n", (unsigned long)resp);
            r = SD_ERR_CMD8; goto fail;
        }
        s_card_type = 2;
        printf("[SD] CMD8 OK: 0x1AA\r\n");
    }
    r = sd_acmd41(SD_ACMD41_VDD | (s_card_type == 2 ? SD_ACMD41_HCS : 0));
    if(r != SD_OK) goto fail;
    if(!(s_ocr & SD_ACMD41_VDD)) { r = SD_ERR_VOLTAGE; goto fail; }
    if(s_card_type == 2 && (s_ocr & SD_ACMD41_HCS)) s_card_type = 3;
    r = sd_send_cmd(2, 0, SDIO_Response_Long, 0, 0, 0);
    if(r != SD_OK) goto fail;
    printf("[SD] CMD2 CID OK\r\n");
    r = sd_send_cmd(3, 0, SDIO_Response_Short, &resp, 0, 0);
    if(r != SD_OK) goto fail;
    /* R6 高 16 位是 RCA，不能用 R1 的高位错误掩码去检查。 */
    s_rca = (u16)(resp >> 16);
    if((resp & 0xE000u) || !s_rca)
    {
        printf("[SD] CMD3 invalid R6: 0x%08lX\r\n", (unsigned long)resp);
        r = SD_ERR_RESPONSE; goto fail;
    }
    printf("[SD] CMD3 RCA=0x%04X\r\n", (unsigned)s_rca);
    r = sd_read_capacity();
    if(r != SD_OK) goto fail;
    printf("[SD] CMD9 CSD OK\r\n");
    r = sd_r1(7, (u32)s_rca << 16, 0);
    if(r != SD_OK) goto fail;
    r = sd_wait_transfer();
    if(r != SD_OK) goto fail;
    printf("[SD] CMD7/CMD13 selected, TRAN ready\r\n");
    if(s_card_type != 3)
    {
        r = sd_r1(16, 512, 0);
        if(r != SD_OK) goto fail;
    }
    r = sd_read_one(0, slow);
    if(r != SD_OK) goto fail;
    printf("[SD] sector 0 OK at 400kHz/1-bit; signature=%02X %02X\r\n", slow[510], slow[511]);
#if SD_BUS_WIDTH_4BIT
    r = sd_r1(55, (u32)s_rca << 16, &resp);
    if(r != SD_OK) goto fail;
    if(!(resp & 0x20u)) { r = SD_ERR_RESPONSE; goto fail; }
    r = sd_r1(6, 2, 0);
    if(r != SD_OK) goto fail;
    bus = SDIO_BusWide_4b; /* 卡与主机必须同步切换位宽。 */
#endif
    /* Standard-speed SD (<=25 MHz), still one data wire. Compare three
     * reads with the 400 kHz reference before accepting each clock. */
    for(candidate=0u;candidate<sizeof(dividers);candidate++) {
        sd_periph_config(dividers[candidate], bus);
        for(pass=0u;pass<3u;pass++) {
            r=sd_read_one(0,fast);
            if(r!=SD_OK)break;
            if(memcmp(slow,fast,512)) { r=SD_ERR_READ;break; }
        }
        if(r==SD_OK)break;
        printf("[SD] clock %lu kHz failed verification; trying slower clock\r\n",
               (unsigned long)(48000u/(dividers[candidate]+2u)));
    }
    if(r!=SD_OK)goto fail;
    printf("[SD] sector 0 verified x3 at %lu kHz/%u-bit\r\n",
           (unsigned long)(48000u/(dividers[candidate]+2u)),SD_BUS_WIDTH_4BIT?4u:1u);
    s_ready = 1;
    Sd_Get_Card_Info();
    return SD_OK;
fail:
    SDIO->DCTRL = 0;
    s_ready = 0;
    s_card_type = 0;
    s_rca = 0;
    s_ocr = s_capacity_kb = s_sector_count = 0;
    printf("[SD] init FAILED r=%u (see command/data error above)\r\n", (unsigned)r);
    return r;
}

u8 Sd_Read_Sector(u32 sector, u8 *buf, u16 cnt)
{
    u8 r;
    if(!buf || !cnt) return SD_ERR_PARAM;
    if(!s_ready) return SD_ERR_NOCARD;
    if(sector >= s_sector_count || cnt > s_sector_count - sector) return SD_ERR_PARAM;
    while(cnt--)
    {
        r = sd_read_one(sector++, buf);
        if(r != SD_OK) return r;
        buf += 512;
    }
    return SD_OK;
}

/* 写扇区：内部就是一个扇区一条 CMD24（和读一样不做 CMD25 多块 + CMD12 停止，
 * 少一条命令就少一处出错的地方），每条后面跟一次 sd_wait_transfer()。
 * ★ 那次 CMD13 不是多余的：CMD24 的 R1 只说明卡**收下了命令**，
 *   "块真的写进 flash 了"要等卡释放 DAT0 忙线后才成立。 */
u8 Sd_Write_Sector(u32 sector, const u8 *buf, u16 cnt)
{
    u8 r;
    if(!buf || !cnt) return SD_ERR_PARAM;
    if(!s_ready) return SD_ERR_NOCARD;
    if(sector >= s_sector_count || cnt > s_sector_count - sector) return SD_ERR_PARAM;
    while(cnt--)
    {
        r = sd_write_one(sector++, buf);
        if(r != SD_OK) return r;
        r = sd_wait_transfer();
        if(r != SD_OK) return r;
        buf += 512;
    }
    return SD_OK;
}

/* 自测图案：掺进扇区号，这样"写到别的扇区去了"也能查出来；
 * 每字节都不一样，防止卡上残留的旧内容碰巧全等而假通过。 */
static u8 sd_test_pattern(u16 i, u32 sector)
{
    return (u8)((u16)(i * 7u) + (u8)sector + (u8)(i >> 3) + 0x5Au);
}

/* 写能力自测：**非破坏性**（读完原内容、测完写回去）。
 * 用卡上最后一个扇区：FAT 的数据几乎不会用到卡尾；万一中途掉电，
 * 损失也是卡尾这一个扇区，而不是文件系统的元数据。
 * ★ 但这不是"绝对安全"：若这卡被格式化到了满容量、且最后一个扇区正好
 *   属于某个文件，测试期间那份数据会被临时覆盖（正常结束时已还原回去）。
 * 覆盖三步：写图案→回读比对（证明"写进去了"）→写回原内容→再回读比对
 * （证明"还原干净了"）。返回 SD_OK 才算这块卡的写能力真的可用。 */
u8 Sd_WriteSelfTest(void)
{
    static u8 orig[512], work[512];
    u32 sector;
    u16 i, bad = 0;
    u8 r;

    if(!s_ready) return SD_ERR_NOCARD;
    if(s_sector_count < 64u) return SD_ERR_PARAM;
    sector = s_sector_count - 1u;

    r = sd_read_one(sector, orig);
    if(r != SD_OK) return r;

    for(i = 0; i < 512u; i++) work[i] = sd_test_pattern(i, sector);
    r = sd_write_one(sector, work);
    if(r != SD_OK) return r;
    r = sd_wait_transfer();
    if(r != SD_OK) return r;

    memset(work, 0, 512);
    r = sd_read_one(sector, work);
    if(r != SD_OK) return r;
    for(i = 0; i < 512u; i++)
        if(work[i] != sd_test_pattern(i, sector)) bad++;

    r = sd_write_one(sector, orig);        /* 还原 */
    if(r != SD_OK) return r;
    r = sd_wait_transfer();
    if(r != SD_OK) return r;

    printf("[SD] 写自测: 扇区 %lu/%lu ", (unsigned long)sector, (unsigned long)s_sector_count);
    if(bad)
    {
        printf("FAIL 回读与写入不符 %u 字节\r\n", (unsigned)bad);
        return SD_ERR_READ;
    }
    memset(work, 0, 512);
    r = sd_read_one(sector, work);
    if(r != SD_OK) return r;
    for(i = 0; i < 512u; i++)
        if(work[i] != orig[i]) bad++;
    if(bad)
    {
        printf("FAIL 还原后与原内容不符 %u 字节\r\n", (unsigned)bad);
        return SD_ERR_READ;
    }
    printf("OK 写→回读→还原 全部一致\r\n");
    return SD_OK;
}

u32 Sd_Get_Sector_Count(void) { return s_sector_count; }

const char *Sd_Get_Card_Name(void)
{
    switch(s_card_type)
    {
        case 3: return "SDHC/SDXC";
        case 2: return "SDSC v2.0";
        case 1: return "SDSC v1.x";
        default: return "not initialized";
    }
}

void Sd_Get_Card_Info(void)
{
    printf("[SD] %s: %lu MB, %lu sectors (512B)\r\n", Sd_Get_Card_Name(),
           (unsigned long)(s_capacity_kb / 1024), (unsigned long)s_sector_count);
}
