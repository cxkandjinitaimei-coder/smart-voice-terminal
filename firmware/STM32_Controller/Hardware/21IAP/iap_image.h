/**
 * @file iap_image.h
 * 用途：应用镜像向量、栈地址与入口地址的共用检查辅助接口。
 * 使用：Bootloader 或应用在信任待执行镜像前调用，和总长度/CRC 校验配合使用。
 * 时序与边界：向量检查不是签名验证；不能据此宣称固件防篡改或安全启动。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef IAP_IMAGE_H
#define IAP_IMAGE_H
#include <stdint.h>
#include "iap_flash.h"
/* Validate a Slot-A linked image even when its bytes are staged in Slot B. */
static uint8_t IAP_ImageVectorsOK(uint32_t sp, uint32_t pc, uint32_t size)
{
    uint32_t entry = pc & ~1u;
    if (size < 8u || size > IAP_FW_SIZE_MAX || (sp & 7u) || !(pc & 1u))
        return 0u;
    if (!((sp > 0x20000000u && sp <= 0x20020000u) || (sp > 0x10000000u && sp <= 0x10010000u)))
        return 0u;
    return (entry >= IAP_SLOT_A_BASE + 8u && entry < IAP_SLOT_A_BASE + size) ? 1u : 0u;
}
#endif
