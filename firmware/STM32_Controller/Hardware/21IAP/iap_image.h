#ifndef IAP_IMAGE_H
#define IAP_IMAGE_H
#include <stdint.h>
#include "iap_flash.h"
/* Validate a Slot-A linked image even when its bytes are staged in Slot B. */
static uint8_t IAP_ImageVectorsOK(uint32_t sp, uint32_t pc, uint32_t size)
{
    uint32_t entry = pc & ~1u;
    if (size < 8u || size > IAP_FW_SIZE_MAX || (sp & 7u) || !(pc & 1u)) return 0u;
    if (!((sp > 0x20000000u && sp <= 0x20020000u) ||
          (sp > 0x10000000u && sp <= 0x10010000u))) return 0u;
    return (entry >= IAP_SLOT_A_BASE + 8u && entry < IAP_SLOT_A_BASE + size) ? 1u : 0u;
}
#endif
