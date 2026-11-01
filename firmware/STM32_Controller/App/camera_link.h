#ifndef CAMERA_LINK_H
#define CAMERA_LINK_H
#include <stdint.h>
/* 退役MCU拍照接口：始终空闲，拍照请求返回不可用；S3独立联网。 */
uint8_t CameraLink_Busy(void);
uint8_t CameraLink_Request(uint16_t session);
void CameraLink_Ack(uint16_t next,const uint8_t *p,uint16_t len);
void CameraLink_Complete(uint16_t session,uint8_t success);
void CameraLink_Tick(uint32_t now);
const char *CameraLink_State(void);
uint32_t CameraLink_Bytes(void);
#endif
