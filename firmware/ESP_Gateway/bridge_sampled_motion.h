/* 录制动作转发器：ESP 不再按 dt 本地计时，只把服务器下发的步骤按批写进
 * F407 的本地队列（@STEPQ）。F407 收齐后 START，由 F407 按 at_ms 执行。
 * 这样 HTTP 轮询、录音/OTA、Wi-Fi 抖动都不会让时间轴停住或越跑越慢。
 * 进度以 F407 回的 running/done 为准，ESP 只搬运和上报。
 */
#ifndef BRIDGE_SAMPLED_MOTION_H
#define BRIDGE_SAMPLED_MOTION_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct SampledStep { uint16_t at, angle[3], speed[3]; };

class BridgeSampledMotion {
public:
  BridgeSampledMotion(): total_(0), accepted_(0), active_(false), started_(false),
    awaiting_(false), awaiting_start_(0), awaiting_n_(0), board_pos_(0), seen_(0), await_at_(0),
    phase_("idle"), board_phase_("idle") { id_[0]=0; }

  template<class Port>
  bool receive(Port &port, const char *id, uint16_t total, uint16_t start,
               const SampledStep *steps, uint8_t n, uint32_t now) {
    if(!id || strlen(id)!=32 || !total || total>302 || n>12 || start+n>total) return false;
    for(uint8_t i=0;i<n;i++) {
      if(steps[i].at>30000) return false;
      for(uint8_t a=0;a<3;a++) if(steps[i].angle[a]>180 || !steps[i].speed[a] || steps[i].speed[a]>360) return false;
    }
    if(strcmp(id_,id)) {
      if(active_ || start) return false;
      snprintf(id_,sizeof(id_),"%s",id);
      total_=total; accepted_=0; active_=true; started_=false; awaiting_=false;
      board_pos_=0; seen_=now; phase_="transferring"; board_phase_="idle";
      char line[40]; snprintf(line,sizeof(line),"@STEPQ:RESET,%u\n",(unsigned)total_); port.print(line);
    }
    if(!active_ || total_!=total) return false;
    /* 同一批未收到 F407 accepted 前，服务器重发同一窗口：不重复写 UART。 */
    if(awaiting_ && start==awaiting_start_) return true;
    if(start!=accepted_) return false;
    for(uint8_t i=0;i<n;i++) {
      char line[96];
      snprintf(line,sizeof(line),"@STEPQ:%u,%u,%u,%u,%u,%u,%u,%u\n",
        (unsigned)(start+i),(unsigned)steps[i].at,
        (unsigned)steps[i].angle[0],(unsigned)steps[i].speed[0],
        (unsigned)steps[i].angle[1],(unsigned)steps[i].speed[1],
        (unsigned)steps[i].angle[2],(unsigned)steps[i].speed[2]);
      port.print(line);
    }
    char end[32]; snprintf(end,sizeof(end),"@STEPQ:END,%u\n",(unsigned)total_); port.print(end);
    awaiting_=true; awaiting_start_=start; awaiting_n_=n; seen_=now; await_at_=now;
    return true;
  }

  template<class Port>
  void tick(Port &port, uint32_t now, bool link) {
    if(!active_) return;
    if(!link || (uint32_t)(now-seen_)>12000u) {
      char stop[32]; snprintf(stop,sizeof(stop),"@STEPQ:STOP\n"); port.print(stop);
      active_=false; phase_=link?"expired":"link_lost"; board_phase_=phase_; return;
    }
    /* END 行丢了：1 秒后补发一次 END，F407 会回 accepted，避免整批永远等下去。 */
    if(awaiting_ && (uint32_t)(now-await_at_)>1000u) {
      char end[32]; snprintf(end,sizeof(end),"@STEPQ:END,%u\n",(unsigned)total_);
      port.print(end); await_at_=now;
    }
    if(accepted_>=total_ && !started_) {
      port.print("@STEPQ:START\n"); started_=true; phase_="starting"; board_phase_="starting";
    }
  }

  template<class Port>
  void cancel(Port &port, const char *id) {
    if(active_ && (!id || !*id || !strcmp(id_,id))) {
      port.print("@STEPQ:STOP\n"); active_=false; phase_="stopped"; board_phase_="stopped";
    }
  }

  /* F407 回执行：accepted / running / done / stopped / error。 */
  void onBoardLine(const char *p) {
    unsigned a=0,b=0;
    if(!p) return;
    if(sscanf(p,"accepted,%u,%u",&a,&b)==2) {
      if(b==total_) { accepted_=(uint16_t)a; awaiting_=false; if(accepted_>=total_) phase_="starting"; }
    } else if(sscanf(p,"running,%u,%u",&a,&b)==2) {
      board_pos_=(uint16_t)a; phase_="running"; board_phase_="running";
    } else if(sscanf(p,"done,%u",&a)==1) {
      board_pos_=(uint16_t)a; active_=false; phase_="done"; board_phase_="done";
    } else if(!strncmp(p,"stopped",7)) {
      active_=false; phase_="stopped"; board_phase_="stopped";
    } else if(!strncmp(p,"error",5)) {
      active_=false; phase_="error"; board_phase_="error";
    }
  }

  bool active() const { return active_; }
  const char* id() const { return id_; }
  const char* phase() const { return phase_; }
  const char* boardPhase() const { return board_phase_; }
  uint16_t accepted() const { return accepted_; }
  uint16_t sent() const { return accepted_; }
  uint16_t total() const { return total_; }
  uint16_t boardPos() const { return board_pos_; }

private:
  char id_[33];
  uint16_t total_, accepted_;
  bool active_, started_, awaiting_;
  uint16_t awaiting_start_, awaiting_n_;
  uint16_t board_pos_;
  uint32_t seen_, await_at_;
  const char* phase_;
  const char* board_phase_;
};
#endif
