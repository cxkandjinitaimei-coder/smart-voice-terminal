/* 本文件提供相机上电自动联网所需的默认配置。
 * 使用：填写与 ESP8266 相同的 2.4GHz Wi-Fi 名称和密码、电脑局域网服务器地址、
 * 网页口令及机器人 ID，再编译 WiFiCamera.ino。首次启动无需连接配网热点。
 * 热点配置页保存过的参数优先，后续修改可在该页面完成；首次合并镜像会清空旧参数。
 * Wi-Fi 密码和网页口令均属于私人配置，不应提交到公开 GitHub 仓库。
 */
#pragma once

#define CAMERA_WIFI_SSID ""
#define CAMERA_WIFI_PASSWORD ""
#define CAMERA_SERVER_URL ""
#define CAMERA_API_TOKEN ""
#define CAMERA_ROBOT_ID "esp-bridge"

// 私用交付填写唯一配置批次，上电应用一次；空值保持原有NVS配置优先。
#define CAMERA_CONFIG_REVISION ""

// 与ESP8266一样向UDP5001问路；HTTPS证书连接不被局域网发现覆盖。
#define CAMERA_LAN_DISCOVERY 1
