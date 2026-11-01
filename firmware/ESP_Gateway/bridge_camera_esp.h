/*
 * 文件用途：兼容现有网关调用的相机空接口。
 * 相机已迁到ESP32-S3，8266只负责机器人命令、音频、遥测及OTA。
 * 使用方法：主循环仍可调用tick/busy；这里不轮询拍照任务、不写Flash、不搬运图片。
 * 保留帧/文本丢弃入口，旧MCU报文不会触发相机上传或占用网络。
 */
#ifndef BRIDGE_CAMERA_ESP_H
#define BRIDGE_CAMERA_ESP_H
class EspCamera {
public:
 bool busy() const {return false;}
 void tick(bool allowed) {(void)allowed;}
 void frame(uint8_t t,uint16_t seq,const uint8_t *p,uint16_t n)
 {(void)t;(void)seq;(void)p;(void)n;}
 bool text(const String &line) {return line.startsWith("@CAM:");}
};
static EspCamera camera;
static bool cameraBusy() {return false;}
#endif
