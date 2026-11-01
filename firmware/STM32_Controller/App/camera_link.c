/*
 * 文件用途：退役的MCU拍照协议兼容入口。
 * 当前摄像头由ESP32-S3通过Wi-Fi直传电脑，不经过F407或ESP8266。
 * 保留这些空接口供原有音频/OTA/调度器调用，不访问GPIO，不采图、不上传。
 * 旧设备误发拍照命令时返回不可用，避免启动已拆除的并口摄像头。
 */
#include "camera_link.h"
uint8_t CameraLink_Busy(void) {return 0u;}
uint8_t CameraLink_Request(uint16_t session) {(void)session;return 0u;}
void CameraLink_Ack(uint16_t next,const uint8_t *p,uint16_t len)
{(void)next;(void)p;(void)len;}
void CameraLink_Complete(uint16_t session,uint8_t success) {(void)session;(void)success;}
void CameraLink_Tick(uint32_t now) {(void)now;}
const char *CameraLink_State(void) {return "EXTERNAL S3";}
uint32_t CameraLink_Bytes(void) {return 0u;}
