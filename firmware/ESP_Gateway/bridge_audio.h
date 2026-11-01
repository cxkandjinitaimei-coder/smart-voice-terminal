#ifndef BRIDGE_AUDIO_H
#define BRIDGE_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Portable protocol core, shared by the actual ESP firmware and host tests.
class BridgeWire {
  uint8_t state = 0, check = 0, hdr[5];
  uint16_t pos = 0, textUsed = 0;
  uint32_t last = 0;
  bool discard = false;
  uint8_t utf8Left = 0;
public:
  uint8_t type = 0, payload[2048];
  uint16_t seq = 0, len = 0;
  char text[192];
  void reset() { state = 0; pos = textUsed = 0; discard = false; utf8Left = 0; }
  // 1=text, 2=frame, -1=damaged input, 0=need more bytes.
  int feed(uint8_t c, uint32_t now) {
    if (state && uint32_t(now-last) > 250) reset();
    last = now;
    if (!state) {
      if (c == 0xA5 && !utf8Left) { state=1; textUsed=0; discard=false; return 0; }
      if (c == '\r' || c == '\n') {
        int result = discard ? -1 : (textUsed ? 1 : 0);
        text[textUsed]=0; textUsed=0; discard=false; utf8Left=0; return result;
      }
      if (!c || textUsed >= sizeof(text)-1) discard=true;
      else if (!discard) {
        text[textUsed++]=char(c);
        if (utf8Left && (c & 0xC0)==0x80) utf8Left--;
        else if (c>=0xC2 && c<=0xDF) utf8Left=1;
        else if (c>=0xE0 && c<=0xEF) utf8Left=2;
        else if (c>=0xF0 && c<=0xF4) utf8Left=3;
        else utf8Left=0;
      }
      return 0;
    }
    if (state == 1) {
      if (c == 0x5A) { state=2; pos=0; check=0; }
      else state=(c == 0xA5) ? 1 : 0;
      return 0;
    }
    if (state == 2) {
      hdr[pos++]=c; check^=c;
      if (pos == 5) {
        type=hdr[0]; seq=uint16_t(hdr[1]) | uint16_t(hdr[2])<<8;
        len=uint16_t(hdr[3]) | uint16_t(hdr[4])<<8; pos=0;
        if (len > sizeof(payload)) { reset(); return -1; }
        state=len ? 3 : 4;
      }
      return 0;
    }
    if (state == 3) {
      payload[pos++]=c; check^=c;
      if (pos == len) state=4;
      return 0;
    }
    state=0;
    return c == check ? 2 : -1;
  }
};

struct AudioTransport {
  virtual bool open(uint16_t session, uint32_t total, uint8_t flags) = 0;
  virtual bool readyForPcm(uint16_t len) { (void)len; return true; }
  virtual bool pcm(const uint8_t *data, uint16_t len) = 0;
  virtual bool finish(uint32_t actualBytes) = 0;
  virtual void abort() = 0;
  virtual void ack(uint16_t next, uint8_t flags) = 0;
  virtual uint32_t clockNow(uint32_t fallback) { return fallback; }
  virtual void error(const char *why) { (void)why; }
};

class BridgeAudio {
  AudioTransport &io;
  uint8_t start[11], tail[2048], deferred[2048];
  uint16_t session=0, next=0, blocks=0;
  uint32_t touched=0;
  bool running=false, shortSentence=false, tailPending=false, deferredPending=false, endPending=false;
  static uint16_t u16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1])<<8; }
  static uint32_t u32(const uint8_t *p) { return uint32_t(u16(p)) | uint32_t(u16(p+2))<<16; }
public:
  explicit BridgeAudio(AudioTransport &transport) : io(transport) {}
  bool busy() const { return running; }
  uint16_t expected() const { return next; }
  void fail(const char *why="capture interrupted") { if (running) { running=false; deferredPending=endPending=false; io.error(why); io.abort(); io.ack(next,2); } }
  void badFrame() { if (running) io.ack(next,1); }
  void tick(uint32_t now) {
    if (!running) return;
    if (endPending && io.readyForPcm(2048)) {
      if (!io.pcm(tail,2048) || !io.finish(uint32_t(next)*2048)) { fail("final PCM could not be saved"); return; }
      endPending=false; running=false; return;
    }
    if (deferredPending && io.readyForPcm(2048)) {
      if (!io.pcm(tail,2048)) { fail(); return; }
      memcpy(tail,deferred,2048);
      deferredPending=false;
      next++; touched=now; io.ack(next,0);
    }
    if (uint32_t(now-touched)>2500) fail("MCU frames timed out 2500ms");
  }
  void frame(uint8_t type, uint16_t seq, const uint8_t *p, uint16_t n, uint32_t now) {
    if (type == 1) {
      if (n != 10 && n != 11) { if (running) fail(); else io.ack(0,2); return; }
      if (running) {
        if (seq == session && n == (shortSentence ? 11 : 10) && !memcmp(start,p,n)) { touched=now; io.ack(next,0); }
        else io.ack(0,2);
        return;
      }
      const uint16_t seconds=u16(p);
      const uint32_t total=u32(p+2);
      const uint8_t flags=(n==11) ? p[10] : 0;
      if ((n==11 && (flags & ~uint8_t(1))) || !seconds || seconds>10 || total != (uint32_t(seconds)*1000/64)*2048 ||
          u16(p+6)!=16000 || p[8]!=16 || p[9]!=1) { io.ack(0,2); return; }
      next=0; blocks=uint16_t(total/2048); session=seq;
      shortSentence=(flags&1)!=0; tailPending=false; deferredPending=endPending=false;
      memcpy(start,p,n);
      /* A previous sentence may still be uploading. Rejecting START must not
       * abort that transport session or delete its flash-backed PCM. */
      if (!io.open(seq,total,flags)) { io.ack(0,2); return; }
      running=true; touched=io.clockNow(now); io.ack(0,0); // F407 starts capture only now.
      return;
    }
    if (!running) return;
    touched=now;
    if (type == 2) {
      if (deferredPending) return; // F407 will retry; ACK only after the queued block is safe.
      if (seq < next && n == 2048) { io.ack(next,0); return; } // lost ACK: do not write twice
      if (seq != next || n != 2048 || next>=blocks) { io.ack(next,1); return; }
      // Always retain one block. The actual final block is known only in END
      // for VAD short sentences; a bad END still leaves the HTTP body incomplete.
      if (tailPending && !io.readyForPcm(2048)) {
        memcpy(deferred,p,2048);
        deferredPending=true;
        return;
      }
      if (tailPending && !io.pcm(tail,2048)) { fail(); return; }
      memcpy(tail,p,n); tailPending=true;
      next++; io.ack(next,0);
    } else if (type == 3) {
      if (n!=6) { fail("MCU END length invalid"); return; }
      const uint16_t actual=u16(p);
      if (u16(p+4)!=0) { fail("MCU reported lost microphone samples"); return; }
      if (seq!=session || actual==0 || actual>blocks || u16(p+2)!=actual ||
          next!=actual || (!shortSentence && actual!=blocks) || !tailPending) { fail("MCU ended before all blocks confirmed"); return; }
      if (!io.readyForPcm(2048)) { endPending=true; return; }
      if (!io.pcm(tail,2048) || !io.finish(uint32_t(actual)*2048)) { fail("final PCM could not be saved"); return; }
      running=false;
    }
  }
};
#endif
