#ifndef SERVO_MOTION_H
#define SERVO_MOTION_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "pca9685.h"

/* PWM target interpolation, not feedback about the physical shaft position.
 * No delay or I2C from interrupts. A slow main loop never catches up in a jump. */
typedef struct {
    uint32_t current, target, updated_at, written;
    uint16_t speed, sent; /* sent is the last ACKed PWM count, not integer degrees. */
    uint8_t failures;
} ServoMotion;
static ServoMotion servo_motion[16];
static uint32_t servo_tick_at;
static uint16_t servo_default_speed;

static void ServoMotion_Init(uint32_t now)
{
    uint8_t i;
    memset(servo_motion, 0, sizeof(servo_motion));
    for (i=0; i<16u; i++) servo_motion[i].sent = 0xFFFFu;
    servo_tick_at = now;
    servo_default_speed = 0u; /* Old commands stay immediate until configured. */
}

static uint8_t ServoMotion_Busy(uint8_t ch)
{
    return (ch < 16u && (servo_motion[ch].current != servo_motion[ch].target ||
                        servo_motion[ch].failures != 0u)) ? 1u : 0u;
}

static void ServoMotion_Stop(uint16_t mask)
{
    uint8_t i;
    for(i=0u; i<16u; i++) if(mask & (uint16_t)(1u<<i)) {
        servo_motion[i].target=servo_motion[i].current;
        servo_motion[i].failures=0u;
    }
}

static void ServoMotion_Write(uint8_t ch, uint8_t force)
{
    ServoMotion *m = &servo_motion[ch];
    /* Convert millidegrees directly: retain the old floor mapping at every
     * integer endpoint while using every available 12-bit PWM step en route. */
    uint16_t count = (uint16_t)(PCA9685_CNT_AT_0DEG +
        (m->current * PCA9685_CNT_SPAN) / (PCA9685_ANGLE_MAX_DEG * 1000u));
    if ((!force && count == m->sent) || PCA9685_TrySetPWM(ch, count) == 0u) {
        m->sent = count; m->written = m->current; m->failures = 0u;
    } else if (++m->failures >= 3u) {
        /* A disconnected controller must not keep the whole main loop busy.
         * Stop at the last ACKed command estimate; a new command may retry. */
        m->current = m->target = m->written; m->failures = 0u;
        printf("[SERVO] id %u stopped: I2C write failed 3 times\r\n",
               (unsigned)PCA_CH_TO_ID(ch));
    }
}

static void ServoMotion_Move(uint8_t ch, uint16_t angle, uint16_t speed, uint32_t now)
{
    ServoMotion *m;
    if (ch >= 16u || angle > 180u || speed > 360u) return;
    m = &servo_motion[ch]; m->target = (uint32_t)angle * 1000u; m->speed = speed;
    m->updated_at = now; m->failures = 0u;
    /* No position feedback before the first acknowledged command. Do not
     * sweep from a fictitious 0 degrees through an unsafe mechanical range. */
    if (m->sent == 0xFFFFu || speed == 0u || m->target == m->current)
    {
        m->current = m->target;
        ServoMotion_Write(ch, 1u); /* Immediate commands reassert the output as before. */
    }
}

static void ServoMotion_Tick(uint32_t now, uint8_t paused)
{
    uint32_t elapsed = now - servo_tick_at;
    uint8_t i;
    if (paused) {
        servo_tick_at = now;
        for (i=0; i<16u; i++) servo_motion[i].updated_at = now;
        return;
    }
    if ((int32_t)elapsed < 20) return;
    servo_tick_at = now;
    for (i=0; i<16u; i++)
    {
        ServoMotion *m = &servo_motion[i];
        uint32_t step, dt;
        if (!ServoMotion_Busy(i)) continue;
        dt = now - m->updated_at;
        if ((int32_t)dt < 0) continue; /* A caller's old timestamp is not a 40ms step. */
        m->updated_at = now;
        if (dt > 40u) dt = 40u;
        step = (uint32_t)m->speed * dt;
        if (m->current < m->target)
            m->current += step < m->target - m->current ? step : m->target - m->current;
        else
            m->current -= step < m->current - m->target ? step : m->current - m->target;
        ServoMotion_Write(i, 0u);
    }
}
#endif
