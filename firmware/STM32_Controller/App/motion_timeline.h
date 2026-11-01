#ifndef MOTION_TIMELINE_H
#define MOTION_TIMELINE_H
#include <stdint.h>
#include <string.h>

/* v1 header: version/count/repeat/reserved/duration_le32.
 * 18-byte step: at_le32/kind/id/angle_le16/speed_le16/face[8].
 * A whole 32-step packet is 584 bytes; no partially received sequence runs.
 * Callbacks are foreground only; timestamps are relative to acceptance. */
#define TL_MAX_STEPS 32u
#define TL_MAX_BYTES (8u + TL_MAX_STEPS * 18u)
typedef struct {
    uint8_t data[TL_MAX_BYTES];
    uint32_t elapsed, last, duration;
    uint16_t seq, bytes, mask;
    uint8_t active, paused, pos, count, cycle, repeat;
} MotionTimeline;

static uint16_t TL_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static uint32_t TL_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint8_t TL_validate(const uint8_t *p, uint16_t n)
{
    uint8_t i, j;
    uint32_t prev=0u, duration;
    if(n<8u || p[0]!=1u || !p[1] || p[1]>TL_MAX_STEPS || !p[2] || p[2]>20u || p[3]) return 0u;
    duration=TL_u32(p+4);
    if(!duration || duration>60000u || duration*p[2]>120000u || n!=8u+18u*p[1]) return 0u;
    for(i=0u; i<p[1]; i++) {
        const uint8_t *s=p+8u+18u*i;
        uint32_t at=TL_u32(s);
        if(at<prev || at>=duration) return 0u;
        prev=at;
        if(s[4]==1u) {
            if(!s[5] || s[5]>16u || TL_u16(s+6)>180u || TL_u16(s+8)>360u) return 0u;
            for(j=10u; j<18u; j++) if(s[j]) return 0u;
        } else if(s[4]==2u) {
            uint8_t ended=0u;
            if(s[5] || TL_u16(s+6) || TL_u16(s+8) || !s[10]) return 0u;
            for(j=10u; j<18u; j++) {
                uint8_t c=s[j];
                if(!c) ended=1u;
                else if(ended || !((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_')) return 0u;
            }
        } else return 0u;
        for(j=0u; j<i; j++) {
            const uint8_t *o=p+8u+18u*j;
            if(s[4]==o[4] && (s[4]==2u || s[5]==o[5]) && at==TL_u32(o)) return 0u;
            if(s[4]==2u && o[4]==2u && at-TL_u32(o)<500u) return 0u;
        }
    }
    if(p[2]>1u) {
        uint32_t first=duration, last=0u;
        for(i=0u; i<p[1]; i++) if(p[8u+18u*i+4u]==2u) {
            uint32_t at=TL_u32(p+8u+18u*i);
            if(first==duration) first=at;
            last=at;
        }
        if(first!=duration && duration-last+first<500u) return 0u;
    }
    return 1u;
}

/* Returns 0 invalid (old task retained), 1 installed, 2 duplicate retry. */
static uint8_t TL_load(MotionTimeline *t, const uint8_t *p, uint16_t n, uint16_t seq, uint32_t now)
{
    if(!TL_validate(p,n)) return 0u;
    if(t->bytes && t->seq==seq) return (t->bytes==n && !memcmp(t->data,p,n)) ? 2u : 0u;
    memcpy(t->data,p,n); t->bytes=n; t->seq=seq;
    t->duration=TL_u32(p+4); t->count=p[1]; t->repeat=p[2];
    t->active=1u; t->paused=0u; t->pos=0u; t->cycle=1u;
    t->elapsed=0u; t->last=now; t->mask=0u;
    return 1u;
}

/* One distinct timestamp per poll. A late poll preserves subsequent gaps
 * instead of rapidly overwriting all overdue expressions. Equal timestamps
 * still execute together (face + multiple axes). Final ramp is allowed to finish. */
static uint8_t TL_tick(MotionTimeline *t, uint32_t now, uint8_t pause, uint8_t busy,
                      void (*execute)(const uint8_t *, uint32_t))
{
    uint32_t dt=now-t->last;
    t->last=now;
    if(!t->active) return 0u;
    if(pause) { t->paused=1u; return 0u; }
    if(t->paused) { t->paused=0u; dt=0u; }
    t->elapsed+=dt;
    if(t->pos<t->count) {
        uint32_t at=TL_u32(t->data+8u+18u*t->pos);
        if(t->elapsed<at) return 0u;
        t->elapsed=at;
        do {
            const uint8_t *s=t->data+8u+18u*t->pos;
            if(s[4]==1u) t->mask|=(uint16_t)(1u<<(s[5]-1u));
            execute(s,now); t->pos++;
        } while(t->pos<t->count && TL_u32(t->data+8u+18u*t->pos)==at);
        return 1u;
    }
    if(t->elapsed<t->duration || busy) return 0u;
    if(t->cycle<t->repeat) { t->cycle++; t->pos=0u; t->elapsed=0u; return 1u; }
    t->active=0u;
    return 2u;
}
#endif
