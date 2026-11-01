/* 屏幕文字可靠下发：submit保存正文，主循环tick发送，ack确认F407已接收。
 * 仅保存一条待确认文字；pending不再暂停网页轮询/音频就绪。正文在动作发送后
 * 延后80ms单独发送，丢帧/丢ACK每350ms重试，最多60次；仅OTA/升级忙时暂停计数。
 * 确认只表示正文已入板端内存，实际绘制仍服从GUI的录音互斥。
 * 使用现有二进制信封：0x24文字(UTF-8 <=168B)，0x25回执(0收到/1忙/2非法)。
 */
#ifndef BRIDGE_TEXT_H
#define BRIDGE_TEXT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

class BridgeText {
    char text_[169];
    uint16_t size_=0, seq_=0;
    uint32_t due_=0;
    uint8_t attempts_=0, state_=0;
public:
    bool pending() const { return state_==1; }
    uint16_t sequence() const { return seq_; }
    unsigned attempts() const { return attempts_; }
    const char *phase() const {
        return state_==1?"waiting_ack":(state_==2?"received":(state_==3?"timeout":(state_==4?"rejected":"idle")));
    }
    void submit(const char *text,uint32_t now) {
        /* 按完整UTF-8字符裁剪，不能把三字节汉字切成半个；换行仅折成空格。 */
        char cleaned[169];uint16_t size=0;
        const unsigned char *p=(const unsigned char *)text;
        while(p && *p) {
            unsigned n=*p<0x80?1:((*p>=0xC2 && *p<=0xDF)?2:((*p>=0xE0 && *p<=0xEF)?3:((*p>=0xF0 && *p<=0xF4)?4:0)));
            if(!n) {p++;continue;}
            bool valid=true;
            for(unsigned i=1;i<n;i++) {if(!p[i] || (p[i]&0xC0)!=0x80) {valid=false;break;}}
            if(!valid) {p++;continue;}
            if(size+n>168)break;
            if(n==1 && (*p=='\r' || *p=='\n'))cleaned[size++]=' ';
            else if(n!=1 || *p>=32) {memcpy(cleaned+size,p,n);size+=(uint16_t)n;}
            p+=n;
        }
        if(!size)return; // 空/非法的新输入不能破坏正在等确认的上一条正文。
        memcpy(text_,cleaned,size);size_=size;text_[size_]=0;
        if(!++seq_)++seq_;
        attempts_=0;state_=1;due_=now+80u;
    }
    template<class Port> void tick(Port &port,uint32_t now,bool blocked) {
        if(!pending())return;
        if(blocked) {due_=now+80u;return;}
        if((int32_t)(now-due_)<0)return;
        if(attempts_>=60) {state_=3;return;}
        uint8_t h[7]={0xA5,0x5A,0x24,(uint8_t)seq_,(uint8_t)(seq_>>8),(uint8_t)size_,0};
        uint8_t sum=0;
        for(unsigned i=2;i<7;i++)sum^=h[i];
        port.write(h,7);
        for(unsigned i=0;i<size_;i++) {port.write((uint8_t)text_[i]);sum^=(uint8_t)text_[i];}
        port.write(sum);attempts_++;due_=now+350u;
    }
    void ack(uint16_t seq,const uint8_t *p,uint16_t n) {
        /* 旧回执、错长度和未发送的回执不能确认新正文。忙保持待重试。 */
        if(!pending() || !attempts_ || seq!=seq_ || n!=1 || !p)return;
        if(p[0]==0)state_=2;
        else if(p[0]==2)state_=4;
    }
};
#endif
