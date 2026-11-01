#pragma once

// 公开模板：复制为 gateway_config.h 后填写本地值；gateway_config.h 不提交。
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"
#define SERVER_PORT      5000
#define API_PATH         "/api/data"
#define API_TOKEN        "YOUR_API_TOKEN"
#define DISCOVER_PORT    5001
#define DISCOVER_MAGIC   "ARDP_DISCOVER"
#define DISCOVER_WAIT_MS 1500
#define DISCOVER_RETRY_MS 5000

static const char* SERVER_IPS[] = {"192.168.1.100"};

