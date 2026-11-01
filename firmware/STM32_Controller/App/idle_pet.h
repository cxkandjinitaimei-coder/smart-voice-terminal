#ifndef IDLE_PET_H
#define IDLE_PET_H
#include "motion_timeline.h"
/* One atomic 0x23 snapshot, <=708B. Whole packet validated before installation.
 * header: v/count/enabled/reserved/revision/limits_revision/idle/sleep/gap (LE32).
 * up to 3 clips: kind(1 awake,2 sleep)/reserved/length_le16/timeline_v1.
 * Absolute uint32 subtraction is wrap-safe for these <=1h intervals. */
#define PET_TRACKS 3u
#define PET_STEPS 12u
#define PET_CLIP_BYTES (8u+18u*PET_STEPS)
typedef struct {
    uint8_t clips[PET_TRACKS][PET_CLIP_BYTES], kind[PET_TRACKS];
    uint16_t bytes[PET_TRACKS];
    uint32_t revision, limits, idle_ms, sleep_ms, gap_ms, activity_at, finished_at;
    uint8_t enabled, count, running, sleeping, cursor;
} IdlePet;
static uint8_t Pet_Valid(const uint8_t *p,uint16_t len)
{
    uint16_t offset=24u,n; uint8_t i,sleep=0u;
    if(len<24u || p[0]!=1u || p[1]>PET_TRACKS || p[2]>1u || p[3] ||
       !TL_u32(p+4) || !TL_u32(p+8) ||
       TL_u32(p+12)<5000u || TL_u32(p+12)>3600000u ||
       TL_u32(p+16)<TL_u32(p+12) || TL_u32(p+16)>3600000u ||
       TL_u32(p+20)<5000u || TL_u32(p+20)>3600000u || (p[2] && !p[1])) return 0u;
    for(i=0u;i<p[1];i++) {
        if(offset+4u>len || p[offset]<1u || p[offset]>2u || p[offset+1])return 0u;
        if(p[offset]==2u && sleep++)return 0u;
        n=TL_u16(p+offset+2);offset+=4u;
        if(n>PET_CLIP_BYTES || offset+n>len || !TL_validate(p+offset,n) ||
           p[offset+2]!=1u)return 0u;
        offset+=n;
    }
    return (uint8_t)(offset==len);
}
static void Pet_Activity(IdlePet *p,uint32_t now)
{
    p->activity_at=p->finished_at=now; p->running=p->sleeping=0u;
}
static void Pet_Load(IdlePet *p,const uint8_t *data,uint32_t now)
{
    uint8_t i;uint16_t off=24u;
    p->count=data[1];p->enabled=data[2];p->revision=TL_u32(data+4);p->limits=TL_u32(data+8);
    p->idle_ms=TL_u32(data+12);p->sleep_ms=TL_u32(data+16);p->gap_ms=TL_u32(data+20);
    p->cursor=0u;Pet_Activity(p,now);
    for(i=0u;i<p->count;i++) {
        p->kind[i]=data[off];p->bytes[i]=TL_u16(data+off+2);off+=4u;
        memcpy(p->clips[i],data+off,p->bytes[i]);off+=p->bytes[i];
    }
}
static uint8_t Pet_Select(IdlePet *p,uint32_t now,uint8_t occupied)
{
    uint8_t i,k;
    if(!p->enabled || p->running || p->sleeping || occupied ||
       now-p->activity_at<p->idle_ms || now-p->finished_at<p->gap_ms)return 0u;
    if(now-p->activity_at>=p->sleep_ms)
        for(i=0u;i<p->count;i++)if(p->kind[i]==2u)return (uint8_t)(i+1u);
    for(i=0u;i<p->count;i++) {
        k=(uint8_t)((p->cursor+i)%p->count);
        if(p->kind[k]==1u) { p->cursor=(uint8_t)((k+1u)%p->count);return (uint8_t)(k+1u); }
    }
    return 0u;
}
static void Pet_Finished(IdlePet *p,uint32_t now)
{
    if(p->running && p->kind[p->running-1u]==2u)p->sleeping=1u;
    p->running=0u;p->finished_at=now;
}
#endif
