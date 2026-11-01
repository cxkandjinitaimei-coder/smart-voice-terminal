/*
 * 文件用途：ESP8266 侧舵机配置快照的解码、CRC 校验和二进制下发。
 *
 * 调用方：ESP_Gateway.ino 在收到网页管理员配置或启动读取缓存后调用。
 * 输入：版本化的 0x22 快照十六进制文本；输出：校验通过后写入 F407 UART 的
 * 二进制帧。快照包含舵机编号、最小/回中/最大角、角色和 CRC，不能直接当作
 * 普通 MOVE 文本发送。
 *
 * 约束：先完整解码并校验，再发送任何 UART 字节；损坏或半写入缓存不得回放，
 * 也不得生成超出 F407 行解析器限制的长文本。校验失败只报告错误，不猜默认角度。
 */
#ifndef BRIDGE_MOTION_H
#define BRIDGE_MOTION_H
#include <stdint.h>
#include <string.h>
/* 阶段：检查长度、编号、限位和 CRC；缓存必须完整有效，半写入文件不能回放，
 * 也不能因为校验失败而偷偷套用一组看似正常的默认角度。 */
static uint32_t motionRead32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint32_t motionCrc32(const uint8_t *p,size_t n) {
    uint32_t crc=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) {
        crc^=p[i];
        for(unsigned b=0;b<8;b++) crc=(crc>>1)^((crc&1u)?0xEDB88320u:0u);
    }
    return crc^0xFFFFFFFFu;
}
static int motionHexDigit(char c) {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static inline bool motionProfileValid(const char *hex, uint32_t &revision) {
    size_t chars=strlen(hex);
    if(chars<24u || chars>204u || (chars&1u)) return false;
    uint8_t p[102];size_t n=chars/2u;
    for(size_t i=0;i<n;i++) {
        int hi=motionHexDigit(hex[2*i]),lo=motionHexDigit(hex[2*i+1]);
        if(hi<0 || lo<0) return false;
        p[i]=(uint8_t)((hi<<4)|lo);
    }
    if(p[0]!=1u || !p[1] || p[1]>16u || n!=6u+6u*p[1]) return false;
    uint16_t mask=0u;uint8_t roles=0u;
    for(unsigned i=0;i<p[1];i++) {
        const uint8_t *s=p+6u+6u*i;
        if(!s[0] || s[0]>16u || s[1]>s[2] || s[2]>180u || s[3]<s[1] || s[3]>s[2] || s[4]>2u || s[5]) return false;
        uint16_t bit=(uint16_t)(1u<<(s[0]-1u));
        if(mask & bit) return false;
        mask|=bit;
        if(s[4]) { uint8_t role=(uint8_t)(1u<<s[4]);if(roles&role) return false;roles|=role; }
    }
    uint32_t computed=motionCrc32(p+6,n-6);
    if(!computed) computed=1u;
    revision=motionRead32(p+2);
    return revision==computed;
}
/* 阶段：在发出帧头前完成整段编码校验；不向 F407 的 192 字节文本解析器发送
 * 超长十六进制行，发送失败时保持原缓存，等待下一次完整快照。 */
template<class Port>
static bool motionSendHex(Port &port, const char *hex, uint8_t type, uint16_t seq) {
    size_t chars=strlen(hex);
    if(!chars || (chars&1u) || chars>(type==0x23u ? 1416u : 1168u)) return false;
    for(size_t i=0;i<chars;i++) if(motionHexDigit(hex[i])<0) return false;
    uint16_t n=(uint16_t)(chars/2u);
    uint8_t h[7]={0xA5,0x5A,type,(uint8_t)seq,(uint8_t)(seq>>8),(uint8_t)n,(uint8_t)(n>>8)};
    uint8_t sum=0;
    for(unsigned i=2;i<7;i++) sum^=h[i];
    port.write(h,7);
    for(size_t i=0;i<chars;i+=2) {
        uint8_t b=(uint8_t)((motionHexDigit(hex[i])<<4)|motionHexDigit(hex[i+1]));
        port.write(b); sum^=b;
    }
    port.write(sum);
    return true;
}
#endif
