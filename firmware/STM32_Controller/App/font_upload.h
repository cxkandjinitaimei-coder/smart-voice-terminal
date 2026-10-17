/**
 * @file font_upload.h
 * 用途：接收上位机字库分块并写入 W25Q，提供进度和失败恢复接口。
 * 使用：前台解析上传命令/帧，Tick 推进写入；完整性检查通过后再让显示层使用新字库。
 * 时序与边界：SPI Flash 擦除与页写存在忙周期；上传期间需让出会竞争同一总线或阻塞前台的任务。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef __FONT_UPLOAD_H
#define __FONT_UPLOAD_H

#include "stm32f4xx.h"

/* 命令入口。返回 1 = 是我的命令（已处理）；0 = 不是，交回调用者。 */
uint8_t FontUpload_Command(const char *line);

/* 1 = 有一笔上传正在进行（begin 之后、end/abort 之前）。给别的模块做互斥用。 */
uint8_t FontUpload_Active(void);

#endif /* __FONT_UPLOAD_H */
