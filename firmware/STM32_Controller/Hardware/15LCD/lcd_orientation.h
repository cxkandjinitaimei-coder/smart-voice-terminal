/**
 * @file lcd_orientation.h
 * 用途：第三舵机角度与横竖屏方向的映射辅助规则。
 * 使用：上层取得板端估计角后调用，方向改变时更新 LCD 与 GUI 布局。
 * 时序与边界：机械标定会影响映射；第三舵机 90 度对应横屏，接口不直接驱动舵机。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef LCD_ORIENTATION_H
#define LCD_ORIENTATION_H
#include <stdint.h>
/* ILI9341 MADCTL: retain BGR in every direction, MV swaps logical axes. */
static uint8_t LCD_OrientationMadctl(uint8_t orientation)
{
    static const uint8_t values[4] = {0x48u, 0x28u, 0x88u, 0xE8u};
    return values[orientation & 3u];
}
static void LCD_OrientationSize(uint8_t orientation, uint16_t *width, uint16_t *height)
{
    *width = (orientation & 1u) ? 320u : 240u;
    *height = (orientation & 1u) ? 240u : 320u;
}
#endif
