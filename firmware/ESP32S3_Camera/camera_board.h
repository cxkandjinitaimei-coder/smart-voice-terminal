/* 本文件固定本项目提供的 ESP32-S3-CAM 案例板相机引脚。
 * 用法：由 WiFiCamera.ino 引入。不是所有 S3 摄像头板都采用这组接线；
 * 这里沿用原案例 CAMERA_MODEL_ESP32S3_EYE，不能套用到 F407/OV7725。
 */
#pragma once
#define CAM_XCLK 15
#define CAM_SDA 4
#define CAM_SCL 5
#define CAM_D0 11
#define CAM_D1 9
#define CAM_D2 8
#define CAM_D3 10
#define CAM_D4 12
#define CAM_D5 18
#define CAM_D6 17
#define CAM_D7 16
#define CAM_VSYNC 6
#define CAM_HREF 7
#define CAM_PCLK 13
