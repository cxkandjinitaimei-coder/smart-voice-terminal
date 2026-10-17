/*
 * 文件：firmware_version.h
 *
 * 职责：统一定义ESP对外固件版本及构建标识，供启动日志与状态上报使用。
 * 使用：主文件先包含此头，gateway_settings.h将ESP_FW_VERSION映射为FW_VERSION。
 *   ESP_FW_VERSION使用三段数字语义版本；ESP_BUILD_ID用于辨认这份源码构建。
 * 修改：正式发布时修改根VERSION并运行tools/set_version.py；注释/文档调整不另改固件版本，也不改变线协议或F407布局版本。
 * 注意：自动OTA只接受数字版本更高的镜像。公开版1.0.0低于旧开发版1.1.13，迁移须同步服务器。
 *   本文件不配置Wi-Fi、设备ID或产品名，不包含可执行更新逻辑。
 */

#ifndef ESP_FIRMWARE_VERSION_H
#define ESP_FIRMWARE_VERSION_H

// 状态上报和自动OTA比较使用此三段数字版本；不是F407的APP_FW_VERSION。
#define ESP_FW_VERSION "1.0.0"
// 用于日志定位源码构建；可独立于语义版本扩展，但不参与当前OTA大小比较。
#define ESP_BUILD_ID "ESP_Gateway-1.0.0"

#endif
