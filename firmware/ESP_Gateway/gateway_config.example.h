/*
 * 文件：gateway_config.example.h
 *
 * 职责：本地网络与认证配置模板，仅含占位值，供复制填写，不直接作为运行配置。
 * 使用：复制为同目录gateway_config.h，填写Wi-Fi、服务端主机/端口、token和设备标识。
 *   gateway_settings.h会自动包含本地文件；没有该文件会给出编译错误，避免误用模板联网。
 * 参数：自动发现优先，GATEWAY_SERVER_HOST仅作为发现失败时的候选地址。
 *   API_TOKEN须与服务端一致，同时用作ESP ArduinoOTA密码；DEVICE_ID关联服务端命令队列。
 * 维护：外层目录/机器人名字可以改变，但不要随意改DEVICE_ID，否则会关联到另一设备队列。
 *   本地gateway_config.h已由.gitignore排除；不要把真实口令改回示例文件后公开上传。
 * 边界：引脚、帧协议和超时由gateway_settings.h统一定义，不在本模板中分散修改。
 */

#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

// 复制为 gateway_config.h 后填写；本地配置不要提交 Git。
// 填写2.4GHz Wi-Fi信息；SSID与密码保持C字符串写法，双引号/反斜杠须转义。
#define GATEWAY_WIFI_SSID "YOUR_WIFI_SSID"
#define GATEWAY_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// 自动发现优先；此地址只在发现失败时尝试，填写运行中转服务的电脑/服务器。
#define GATEWAY_SERVER_HOST "192.168.1.100"
// 运行服务的HTTP监听端口；局域网测试与云端使用的值须各自对应。
#define GATEWAY_SERVER_PORT 5000

// 与中转服务 token 一致，同时用作 ESP ArduinoOTA 密码；请设置非空私有值。
#define GATEWAY_API_TOKEN "REPLACE_WITH_YOUR_PRIVATE_TOKEN"

// 稳定设备 ID 对应服务器中的指令队列，不随文件夹或机器人名字改变。
#define GATEWAY_DEVICE_ID "esp-bridge"
// 仅是ArduinoOTA在局域网的可辨认名字，不影响服务端的DEVICE_ID队列。
#define GATEWAY_OTA_HOSTNAME "esp-gateway"

#endif
