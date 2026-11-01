#ifndef BRIDGE_PET_H
#define BRIDGE_PET_H
#include <stdint.h>
#include <stdio.h>
struct BridgePose {
    uint32_t uptime=0u, revision=0u, seen=0u;
    uint16_t angle[3]={0u,0u,0u};
    uint8_t mask=0u, moving=0u, idle=0u;
    bool valid=false;
    bool accept(const char *text,uint32_t now) {
        unsigned long up,rev;unsigned m,a,b,c,mov,id;char extra;
        if(sscanf(text,"@POSE:%lu,%u,%u,%u,%u,%u,%u,%lu%c",&up,&m,&a,&b,&c,&mov,&id,&rev,&extra)!=8 ||
           up>0xffffffffUL || rev>0xffffffffUL || m>7u || a>180u || b>180u || c>180u || mov>1u || id>4u)return false;
        uptime=(uint32_t)up;revision=(uint32_t)rev;seen=now;mask=(uint8_t)m;
        angle[0]=(uint16_t)a;angle[1]=(uint16_t)b;angle[2]=(uint16_t)c;moving=(uint8_t)mov;idle=(uint8_t)id;valid=true;
        return true;
    }
};
#endif
