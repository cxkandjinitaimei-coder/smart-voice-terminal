#pragma once

// 配置入口：本地存在 gateway_config.h 时优先使用；公开仓库只提供占位默认值。
#include "firmware_version.h"
#if __has_include("gateway_config.h")
#include "gateway_config.h"
#else
#include "gateway_config.example.h"
#endif
