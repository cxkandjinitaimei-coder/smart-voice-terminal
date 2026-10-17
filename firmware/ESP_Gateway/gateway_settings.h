/*
 * 文件：gateway_settings.h
 *
 * 职责：连接用户可修改配置与现有业务符号，并集中定义板间通信/服务发现/调度常量。
 * 使用：主文件在ESP和JSON等库可见后、各bridge实现头之前包含一次。
 *   必须先按示例创建gateway_config.h；业务继续使用WIFI_SSID/API_TOKEN等统一入口。
 * 分层：GATEWAY_*由用户配置；DISCOVER_MAGIC、API_PATH等是双方约定的线协议，不能随工程改名。
 *   DBG固定Serial1，板间协议固定Serial；F407_BAUD若修改，F407 USART6必须同步更新。
 * 单位：*_MS及OTA_CHECK_INTERVAL/SEND_INTERVAL/HTTP_TIMEOUT_MS均为毫秒，F407_BAUD为波特率。
 * 保护：非空token编译检查只排除空字符串，不校验服务端一致性，也不替用户生成认证配置。
 * 依赖：firmware_version.h应先包含，以便FW_VERSION引用统一版本；本头还定义候选地址和token对象。
 *   这些对象目前沿用单翻译单元结构，不能不加区分地从多个.cpp重复包含。
 */

#ifndef GATEWAY_SETTINGS_H
#define GATEWAY_SETTINGS_H

// 配置缺失时主动报出下一步操作；示例仍参与后续语法解析，以减少无关连锁错误。
#if __has_include("gateway_config.h")
#include "gateway_config.h"
#else
#error "Copy gateway_config.example.h to gateway_config.h and configure Wi-Fi/server/token."
#include "gateway_config.example.h"
#endif

// 用户配置只在此映射，业务模块不依赖外层目录名或产品名称。
#define WIFI_SSID GATEWAY_WIFI_SSID
#define WIFI_PASSWORD GATEWAY_WIFI_PASSWORD
// 候选名单沿用数组形式；若后续扩展多地址，应仍由本配置层管理，不写入业务模块。
const char *SERVER_IPS[] = {GATEWAY_SERVER_HOST};
#define SERVER_PORT GATEWAY_SERVER_PORT
const char *API_TOKEN = GATEWAY_API_TOKEN;
#define DEVICE_ID GATEWAY_DEVICE_ID
#define OTA_DEVICE_NAME GATEWAY_OTA_HOSTNAME
// 该断言要求token配置为非空字符串常量；实际认证失败须检查服务端token是否一致。
static_assert(sizeof(GATEWAY_API_TOKEN) > 1, "GATEWAY_API_TOKEN must not be empty.");

// 服务发现和 HTTP 路由属于现有通信协议，工程改名时保持一致。
#define API_PATH "/api/data"
#define DISCOVER_PORT 5001
#define DISCOVER_MAGIC "ARDP_DISCOVER"
// 发现等待/重试限制，发现过程中仍应泵入UART，不能把1.5s等待写成不收串口的延时。
#define DISCOVER_WAIT_MS 1500
#define DISCOVER_RETRY_MS 5000

// 运行参数保持开发版数值；变更 UART 波特率须同步修改 F407。
#define FW_VERSION ESP_FW_VERSION
#define OTA_CHECK_INTERVAL 60000
#define F407_BAUD 921600
#define DBG Serial1
// 普通状态轮询周期；收到ACK/ALERT可提前上报，传输任务占用时轮询会推迟。
#define SEND_INTERVAL 1000
#define HTTP_TIMEOUT_MS 3000
// 设1时仅在上电发送PING自检；不强制舵机运动，也不会替代正常服务发现。
#define UART_SELFTEST 0

#endif
