/**
 * @file product_config.h
 * 用途：集中配置屏幕显示名称，产品命名与驱动、协议和工程路径无关。
 * 使用：修改 ROBOT_DISPLAY_NAME 后重新编译应用；不修改串口命令或设备 ID。
 * 约束：状态栏宽度有限，建议两个汉字或四个 ASCII 字符；字库外字符会显示问号。
 */
#ifndef PRODUCT_CONFIG_H
#define PRODUCT_CONFIG_H
#define ROBOT_DISPLAY_NAME "伙伴"
#endif
