/**
 * @file camera_link.c
 * 用途：把一次拍照请求连接到 OV7725 FIFO 采集及 ESP 图片传输。
 * 使用：初始化后由命令置请求，主循环 Tick 执行采集、分块发送和结束处理。
 * 时序与边界：图像占用串口期间须与录音、OTA 协作避让；发送成功不代表上位机已经完成人脸识别。
 * 接口说明：对外接口见同名头文件或上层调用处；静态函数只在本模块内部使用。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#include "camera_link.h"
#include "camera_ov7725.h"
#include "ailink.h"
#include "esp_uart.h"
#include "miclink.h"
#include "micaud.h"
#include "iap_app.h"
#include "font_upload.h"
#include "time.h"
#include <stdio.h>
#include <string.h>

/* No image buffer: retain only the current 512-byte packet for ACK retry. */
static uint8_t state, waiting, retries;
static uint16_t session, next, last_seq, last_len;
static uint8_t packet[516];
static uint32_t last_sent, started, bytes, checksum, settle_at;
static uint8_t last_type;
enum
{
    CAM_IDLE,
    CAM_INIT,
    CAM_CAPTURE,
    CAM_START,
    CAM_DATA,
    CAM_END,
    CAM_UPLOAD,
    CAM_SETTLE
};
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}
static uint32_t crc(uint32_t c, const uint8_t *p, uint16_t n)
{
    uint8_t i;
    while (n--)
    {
        c ^= *p++;
        for (i = 0u; i < 8u; i++)
            c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
    }
    return c;
}
static void finish(uint8_t ok, const char *why)
{
    char line[96];
    (void)Camera_OV7725_CaptureStop();
    sprintf(line, "@CAM:%s,%u,%s\r\n", ok ? "DONE" : "ERROR", session, why);
    ESP_UART_SendStr(line);
    printf("[CAM] %s bytes=%lu\r\n", line, (unsigned long)bytes);
    state = CAM_IDLE;
    waiting = 0u;
}
uint8_t CameraLink_Busy(void)
{
    return state != CAM_IDLE;
}
uint32_t CameraLink_Bytes(void)
{
    return bytes;
}
const char *CameraLink_State(void)
{
    static const char *names[] = {"IDLE",         "INITIALIZING", "CAPTURING", "START",
                                  "TRANSFERRING", "VERIFYING",    "UPLOADING", "SETTLING"};
    return names[state];
}
uint8_t CameraLink_Request(uint16_t sid)
{
    if (!sid || CameraLink_Busy() || MicLink_IsRecording() || MicAud_IsStreaming() ||
        !strcmp(MicLink_AudioStateName(), "UPLOADING") || IAP_IsBusy() || AILink_OtaPending() ||
        FontUpload_Active() || AILink_ServoBusy())
        return 0u;
    session = sid;
    state = CAM_INIT;
    started = GetTick_ms();
    bytes = 0u;
    next = 0u;
    checksum = 0xFFFFFFFFu;
    waiting = retries = 0u;
    AILink_UserActivity();
    return 1u;
}
static void send(uint8_t type, uint16_t seq, uint16_t length, uint32_t now)
{
    last_type = type;
    last_seq = seq;
    last_len = length;
    ESP_UART_SendFrame(type, seq, packet, length);
    last_sent = now;
    waiting = 1u;
    retries = 0u;
}
void CameraLink_Ack(uint16_t seq, const uint8_t *p, uint16_t len)
{
    uint16_t sid;
    if (!waiting || len != 3u)
        return;
    sid = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    if (sid != session)
        return;
    if (p[2] == 2u)
    {
        finish(0u, "ESP_REJECTED");
        return;
    }
    if (p[2] != 0u)
        return; /* NACK waits for bounded retransmit. */
    if (state == CAM_START && seq == 0u)
    {
        waiting = 0u;
        state = CAM_DATA;
    }
    else if (state == CAM_DATA && seq == (uint16_t)(next + 1u))
    {
        waiting = 0u;
        next++;
        bytes += 512u;
        if (bytes == 153600u)
            state = CAM_END;
    }
    else if (state == CAM_END && seq == 301u)
    {
        waiting = 0u;
        state = CAM_UPLOAD;
    }
}
void CameraLink_Complete(uint16_t sid, uint8_t ok)
{
    if (state && sid == session && (!ok || state == CAM_UPLOAD))
        finish(ok, ok ? "STORED" : "UPLOAD_FAILED");
}
void CameraLink_Tick(uint32_t now)
{
    Camera_OV7725_Status result;
    uint16_t got;
    if (!state)
        return;
    if (IAP_IsBusy() || AILink_OtaPending() || FontUpload_Active())
    {
        finish(0u, "PRIORITY_OTA");
        return;
    }
    if (now - started > 50000u)
    {
        finish(0u, "TIMEOUT");
        return;
    }
    if (waiting)
    {
        if (now - last_sent >= 600u)
        {
            if (retries++ >= 4u)
            {
                finish(0u, "ACK_TIMEOUT");
                return;
            }
            ESP_UART_SendFrame(last_type, last_seq, packet, last_len);
            last_sent = now;
        }
        return;
    }
    if (state == CAM_INIT)
    {
        result = Camera_OV7725_GetInfo()->initialized ? CAMERA_OV7725_OK : Camera_OV7725_Init(0);
        if (result != CAMERA_OV7725_OK)
        {
            finish(0u, Camera_OV7725_StatusName(result));
            return;
        }
        /* Yield while initial exposure/white-balance frames settle. */
        settle_at = GetTick_ms();
        state = CAM_SETTLE;
        return;
    }
    if (state == CAM_SETTLE)
    {
        if ((uint32_t)(now - settle_at) < 300u)
            return;
        result = Camera_OV7725_CaptureStart(2500u);
        if (result != CAMERA_OV7725_OK)
        {
            finish(0u, Camera_OV7725_StatusName(result));
            return;
        }
        state = CAM_CAPTURE;
        return;
    }
    if (state == CAM_CAPTURE)
    {
        result = Camera_OV7725_CapturePoll(now);
        if (result == CAMERA_OV7725_ERR_BUSY)
            return;
        if (result != CAMERA_OV7725_OK)
        {
            finish(0u, Camera_OV7725_StatusName(result));
            return;
        }
        state = CAM_START;
    }
    if (state == CAM_START)
    {
        packet[0] = 1u;
        put16(packet + 1, session);
        put16(packet + 3, 320u);
        put16(packet + 5, 240u);
        packet[7] = 1u;
        put32(packet + 8, 153600u);
        send(0x30u, 0u, 12u, now);
    }
    else if (state == CAM_DATA)
    {
        result = Camera_OV7725_ReadChunk(packet, 512u, &got);
        if (result != CAMERA_OV7725_OK || got != 512u)
        {
            finish(0u, "FIFO_READ");
            return;
        }
        checksum = crc(checksum, packet, 512u);
        put32(packet + 512, crc(0xFFFFFFFFu, packet, 512u) ^ 0xFFFFFFFFu);
        send(0x31u, next, 516u, now);
    }
    else if (state == CAM_END)
    {
        put16(packet, session);
        put32(packet + 2, bytes);
        put32(packet + 6, checksum ^ 0xFFFFFFFFu);
        send(0x32u, 300u, 10u, now);
    }
}
