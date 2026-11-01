#ifndef LCD_ORIENTATION_H
#define LCD_ORIENTATION_H
#include <stdint.h>
/* ILI9341 MADCTL: retain BGR in every direction, MV swaps logical axes. */
static uint8_t LCD_OrientationMadctl(uint8_t orientation)
{
    static const uint8_t values[4]={0x48u,0x28u,0x88u,0xE8u};
    return values[orientation&3u];
}
static void LCD_OrientationSize(uint8_t orientation, uint16_t *width, uint16_t *height)
{
    *width=(orientation&1u)?320u:240u;
    *height=(orientation&1u)?240u:320u;
}
#endif
