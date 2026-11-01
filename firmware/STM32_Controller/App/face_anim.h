#ifndef FACE_ANIM_H
#define FACE_ANIM_H
#include <stdint.h>

typedef struct {
    const char *directory;
    uint16_t frames, frame, rows, interval_ms;
    uint32_t completed;
    uint8_t active, error;
    uint16_t measured_fps_x10; /* Last >=1 s sample; zero means not sampled. */
    uint8_t rgb565;
} FaceAnim_Info;
/* Prefer aligned SD RGB565 (R16); fall back to BMP, never buffer a full frame. */
uint8_t FaceAnim_Start(const char *name, uint16_t x, uint16_t y, uint32_t now);
void FaceAnim_Stop(void);
/* allow=0 performs no SD operation and no LCD draw, including file close. */
void FaceAnim_Tick(uint32_t now, uint8_t allow);
void FaceAnim_GetInfo(FaceAnim_Info *info);
uint8_t FaceAnim_SetFPS(uint8_t fps);
uint8_t FaceAnim_SetEnabled(uint8_t enabled);
uint8_t FaceAnim_IsEnabled(void);
#endif
