/**
 * @file camera_link.h
 * 用途：把一次拍照请求连接到 OV7725 FIFO 采集及 ESP 图片传输。
 * 使用：初始化后由命令置请求，主循环 Tick 执行采集、分块发送和结束处理。
 * 时序与边界：图像占用串口期间须与录音、OTA 协作避让；发送成功不代表上位机已经完成人脸识别。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef CAMERA_LINK_H
#define CAMERA_LINK_H
#include <stdint.h>
/* Camera binary namespace 0x30..0x33, independent of audio/IAP/motion. */
uint8_t CameraLink_Busy(void);
uint8_t CameraLink_Request(uint16_t session);
void CameraLink_Ack(uint16_t next, const uint8_t *p, uint16_t len);
void CameraLink_Complete(uint16_t session, uint8_t success);
void CameraLink_Tick(uint32_t now);
const char *CameraLink_State(void);
uint32_t CameraLink_Bytes(void);
#endif
