/* 时间轴可靠传输：完整保存一段32步动作，主循环tick发出，收到同序号
 * @TIMELINE回执才结束等待。每32字节让出1ms，避免921600波特率连续突发；
 * 600ms未确认重发原序号，最多8次。F407按序号去重，不会重放已执行的步骤。
 * 步骤时间、角度和速度仍由F407本地时基执行，不靠HTTP轮询计时。
 * submit只接受完整合法hex；tick的blocked用于录音/OTA资源互斥。
 */
#ifndef BRIDGE_TIMELINE_H
#define BRIDGE_TIMELINE_H
#include "bridge_motion.h"
#include <stdio.h>
class BridgeTimeline {
    uint8_t data_[584];
    uint16_t size_=0, seq_=0;
    uint8_t attempts_=0;
    bool waiting_=false;
    uint32_t due_=0, duration_=0, ack_at_=0;
    char phase_[16]="idle", id_[41]="", board_[100]="";
public:
    bool pending() const {return waiting_;}
    /* timeout 不再算 executing：8 次重发都没回执，或运行中超过整段时长+5s 没有
     * 新回执，就说明这条二进制时间轴已经失联。继续把它当“忙”会让 step_playback
     * 永远进不来、普通指令队列也永远不弹出（现场表现就是“下发直接卡住”）。 */
    bool executing() const {return waiting_ || !strcmp(phase_,"running") || !strcmp(phase_,"paused") ||
                                   !strcmp(phase_,"stopping");}
    unsigned attempts() const {return attempts_;}
    uint16_t sequence() const {return seq_;}
    const char *phase() const {return phase_;}
    const char *id() const {return id_;}
    const char *board() const {return board_;}
    void cancel(uint32_t now) {waiting_=false;due_=now+5000u;ack_at_=now;strcpy(phase_,"stopping");}
    bool submit(const char *hex,uint16_t seq,const char *id,uint32_t now) {
        const size_t n=strlen(hex);
        if(!n || (n&1u) || n>1168u || waiting_)return false;
        for(size_t i=0;i<n;i++)if(motionHexDigit(hex[i])<0)return false;
        for(size_t i=0;i<n/2;i++)data_[i]=(uint8_t)((motionHexDigit(hex[2*i])<<4)|motionHexDigit(hex[2*i+1]));
        size_=(uint16_t)(n/2);seq_=seq;attempts_=0;waiting_=true;due_=now+100u;ack_at_=now;
        /* v1 头：version/count/repeat/reserved/duration_le32；用于运行看门狗。 */
        duration_=size_>=8 ? ((uint32_t)data_[4]|((uint32_t)data_[5]<<8)|
                    ((uint32_t)data_[6]<<16)|((uint32_t)data_[7]<<24)) : 0u;
        snprintf(id_,sizeof(id_),"%s",id?id:"");board_[0]=0;strcpy(phase_,"waiting_ack");return true;
    }
    template<class Port> void tick(Port &port,uint32_t now,bool blocked,void (*pace)()) {
        if(!waiting_) {
            /* Lost stop ACK is ambiguous: hold later clips until explicit status or stop timeout. */
            if(!strcmp(phase_,"stopping") && (int32_t)(now-due_)>=0)strcpy(phase_,"timeout");
            /* 运行中长时间没有任何新回执：整段时长+5s 后判失联，释放后续动作。 */
            else if(duration_ && (!strcmp(phase_,"running")||!strcmp(phase_,"paused")) &&
                    (int32_t)(now-ack_at_)>(int32_t)(duration_+5000u))strcpy(phase_,"timeout");
            return;
        }
        if(blocked) {due_=now+100u;return;}
        if((int32_t)(now-due_)<0)return;
        if(attempts_>=8u) {waiting_=false;strcpy(phase_,"timeout");return;}
        uint8_t h[7]={0xA5,0x5A,0x20,(uint8_t)seq_,(uint8_t)(seq_>>8),(uint8_t)size_,(uint8_t)(size_>>8)},sum=0;
        for(unsigned i=2;i<7;i++)sum^=h[i];
        port.write(h,7);
        for(unsigned i=0;i<size_;i++) {
            port.write(data_[i]);sum^=data_[i];
            if((i&31u)==31u && pace)pace();
        }
        port.write(sum);attempts_++;due_=now+600u;
    }
    bool ack(const char *text,uint32_t now=0) {
        char state[16];unsigned seq;
        if(!attempts_ || sscanf(text,"%15[^,],%u",state,&seq)!=2 || seq!=seq_)return false;
        if(strcmp(state,"running") && strcmp(state,"paused") && strcmp(state,"done") &&
           strcmp(state,"stopped") && strcmp(state,"invalid") && strcmp(state,"limits_error"))return false;
        /* 完成后的迟到进度不能把终态改回运行中；新任务由submit重置。 */
        if(!strcmp(phase_,"done") || !strcmp(phase_,"stopped") ||
           !strcmp(phase_,"invalid") || !strcmp(phase_,"limits_error") ||
           !strcmp(phase_,"cancelled"))return false;
        waiting_=false;ack_at_=now;snprintf(phase_,sizeof(phase_),"%s",state);
        snprintf(board_,sizeof(board_),"%s",text);return true;
    }
};
#endif
