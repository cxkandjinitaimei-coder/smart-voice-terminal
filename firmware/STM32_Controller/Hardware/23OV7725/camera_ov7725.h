/**
 * @file camera_ov7725.h
 * 用途：OV7725 SCCB 寄存器配置、帧同步、外置 FIFO 读取及色条诊断。
 * 使用：按 pins 头连接 8 位数据与控制线，Init 后请求一帧，等待 VSYNC 再控制 WEN/FIFO 读出。
 * 时序与边界：是带 FIFO 模块，不是直接 DCMI 连线；飞线时序/接触不良仍会花屏。不要把单帧测试说成实时视频。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef CAMERA_OV7725_H
#define CAMERA_OV7725_H

#include "stm32f4xx.h"

typedef enum
{
    CAMERA_OV7725_OK = 0,
    CAMERA_OV7725_ERR_ARG = 1,
    CAMERA_OV7725_ERR_BUS = 2,
    CAMERA_OV7725_ERR_MID = 3,
    CAMERA_OV7725_ERR_PID = 4,
    CAMERA_OV7725_ERR_TIMEOUT = 5,
    CAMERA_OV7725_ERR_NOT_READY = 6,
    CAMERA_OV7725_ERR_BUSY = 7,
    CAMERA_OV7725_ERR_CONFIG = 8
} Camera_OV7725_Status;

typedef struct
{
    uint16_t manufacturer_id;
    uint16_t product_id;
    uint16_t width;
    uint16_t height;
    uint8_t rgb565;
    uint8_t fifo_present;
    uint8_t initialized;
    uint8_t frame_ready;
} Camera_OV7725_Info;

/* Initialized on demand by the camera test link; no camera is required at boot. */
Camera_OV7725_Status Camera_OV7725_Init(Camera_OV7725_Info *info);
Camera_OV7725_Status Camera_OV7725_Probe(Camera_OV7725_Info *info);
uint32_t Camera_OV7725_PinCheck(void);

/* FIFO capture is deliberately poll-driven so the caller can yield to audio,
 * ESP UART and OTA between calls. It never allocates a full image buffer. */
Camera_OV7725_Status Camera_OV7725_CaptureStart(uint32_t timeout_ms);
Camera_OV7725_Status Camera_OV7725_CapturePoll(uint32_t now);
Camera_OV7725_Status Camera_OV7725_ReadChunk(uint8_t *dst, uint16_t capacity, uint16_t *read_count);
Camera_OV7725_Status Camera_OV7725_CaptureStop(void);
uint8_t Camera_OV7725_IsFrameReady(void);
const Camera_OV7725_Info *Camera_OV7725_GetInfo(void);
const char *Camera_OV7725_StatusName(Camera_OV7725_Status status);

#ifdef CAMERA_FIFO_FAST_IO
/* Fast FIFO path used by the full robot and the isolated LCD experiment.
 * Rewind/colour bars are diagnostic controls; normal capture stays full QVGA. */
void Camera_OV7725_SetReadSafe(uint8_t safe);
Camera_OV7725_Status Camera_OV7725_Rewind(void);
Camera_OV7725_Status Camera_OV7725_SetColorBars(uint8_t enabled);
#endif

#endif
