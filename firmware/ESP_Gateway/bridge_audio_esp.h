#ifndef BRIDGE_AUDIO_ESP_H
#define BRIDGE_AUDIO_ESP_H
#include "bridge_audio.h"

class EspAudioTransport : public AudioTransport {
  WiFiClient socket;
  File spool, replay;
  String response;
  uint32_t responseDue=0, totalBytes=0, actualBytes=0, storedBytes=0;
  static const size_t AUDIO_QUEUE_SIZE=8192;
  static const size_t FLASH_QUEUE_SIZE=4096;
  static const char *spoolPath() { return "/mic-audio.pcm"; }
  uint8_t queue[AUDIO_QUEUE_SIZE];
  uint8_t flashQueue[FLASH_QUEUE_SIZE];
  size_t flashHead=0, flashTail=0, flashUsed=0;
  size_t queueHead=0, queueTail=0, queueUsed=0;
  uint32_t queuedBytes=0, sentBytes=0, lastProgress=0, captureEndedMs=0;
  uint32_t retryAt=0;
  uint16_t sessionId=0;
  uint8_t vadFlags=0, attempts=0;
  bool active=false, waiting=false, finishing=false, faulted=false;
  bool fsReady=false, recording=false, spill=false, closing=false;
  const char *lastError="none";
  uint32_t uartOverruns=0;
  uint8_t wireChunk[524];
  size_t wireLen=0, wireOff=0, wirePcm=0;
  bool wireFromQueue=false, wireFinal=false;

  // PCM acceptance/ACK only copies RAM. Flash can erase/program with stalls;
  // perform one 512B operation per loop, after UART has been drained.
  bool flushCapture() {
    if (flashUsed) {
      size_t n=flashUsed;
      if (n>512) n=512;
      if (n>FLASH_QUEUE_SIZE-flashTail) n=FLASH_QUEUE_SIZE-flashTail;
      const uint8_t *p=flashQueue+flashTail;
      if (spool.write(p,n)!=n) { lastError="flash write failed"; abort(); return false; }
      storedBytes+=n;
      if (!spill && n<=AUDIO_QUEUE_SIZE-queueUsed) {
        size_t first=AUDIO_QUEUE_SIZE-queueHead;
        if (first>n) first=n;
        memcpy(queue+queueHead,p,first);
        memcpy(queue,p+first,n-first);
        queueHead=(queueHead+n)%AUDIO_QUEUE_SIZE;
        queueUsed+=n;
      } else spill=true;
      flashTail=(flashTail+n)%FLASH_QUEUE_SIZE;
      flashUsed-=n;
    }
    if (closing && !flashUsed) {
      spool.close();
      replay=LittleFS.open(spoolPath(),"r");
      if (!replay || replay.size()!=storedBytes) { lastError="flash replay unavailable"; abort(); return false; }
      closing=false; finishing=true; lastProgress=millis();
      DBG.printf("[AUDIO] captured %lu bytes; durable stream drain\n",(unsigned long)storedBytes);
    }
    return true;
  }

  /* VAD does not know its final byte count until END. A complete HTTP chunk
   * is prepared before writing, and short TCP writes resume at wireOff. */
  void pollChunked() {
    if (!wireLen) {
      if (queueUsed || (finishing && sentBytes<actualBytes)) {
        uint8_t pcm[512];
        size_t n=queueUsed ? queueUsed : size_t(actualBytes-sentBytes);
        if (n>sizeof(pcm)) n=sizeof(pcm);
        wireFromQueue=queueUsed!=0;
        if (wireFromQueue) {
          for (size_t i=0;i<n;i++) pcm[i]=queue[(queueTail+i)%AUDIO_QUEUE_SIZE];
        } else {
          if (recording || closing) return;
          if (!replay.seek(sentBytes,SeekSet) || replay.read(pcm,n)!=(int)n) {
            abort(); return;
          }
        }
        int h=snprintf((char*)wireChunk,sizeof(wireChunk),"%X\r\n",(unsigned)n);
        if (h<0 || size_t(h)+n+2>sizeof(wireChunk)) { abort(); return; }
        memcpy(wireChunk+h,pcm,n);
        wireChunk[h+n]='\r'; wireChunk[h+n+1]='\n';
        wireLen=size_t(h)+n+2; wirePcm=n; wireOff=0; wireFinal=false;
      } else if (finishing && sentBytes==actualBytes) {
        memcpy(wireChunk,"0\r\n\r\n",5);
        wireLen=5; wirePcm=0; wireOff=0; wireFinal=true;
      } else return;
    }
    int room=socket.availableForWrite();
    if (room>0) {
      size_t n=wireLen-wireOff;
      if (n>size_t(room)) n=size_t(room);
      size_t wrote=socket.write(wireChunk+wireOff,n);
      if (wrote) { wireOff+=wrote; lastProgress=millis(); }
    }
    if (wireOff==wireLen) {
      if (wireFinal) {
        finishing=false; waiting=true; responseDue=millis()+15000;
      } else {
        if (wireFromQueue) {
          queueTail=(queueTail+wirePcm)%AUDIO_QUEUE_SIZE;
          queueUsed-=wirePcm;
        }
        sentBytes+=wirePcm;
      }
      wireLen=wireOff=wirePcm=0;
    }
    if (!waiting && uint32_t(millis()-lastProgress)>15000)
      retryStream("TCP stalled 15s");
  }

  /* WiFiClient::write() is allowed to accept only part of a large buffer.
   * A single short write used to leave half of an HTTP chunk on the wire;
   * Werkzeug then correctly rejected it as "Invalid chunk header". */
  bool writeAll(const uint8_t *p, size_t n) {
    while (n) {
      size_t sent=socket.write(p,n);
      if (!sent) return false;
      p+=sent; n-=sent;
      delay(0);
    }
    return true;
  }
  static String urlEncode(const String &s) {
    String out;
    const char *hex="0123456789ABCDEF";
    for (size_t i=0;i<s.length();i++) {
      uint8_t c=s[i];
      if (isalnum(c) || c=='-' || c=='_' || c=='.') out+=char(c);
      else { out+='%'; out+=hex[c>>4]; out+=hex[c&15]; }
    }
    return out;
  }
  bool connectStream() {
    if (WiFi.status()!=WL_CONNECTED || !g_server_ip.length()) return false;
    socket.stop();
    socket.setTimeout(500);
    attempts++;
    if (!socket.connect(g_server_ip.c_str(),SERVER_PORT)) return false;
    socket.setNoDelay(true);
    String sid=String(ESP.getChipId(),HEX)+"-"+String(micros(),HEX)+
               "-"+String(sessionId)+"-"+String(attempts);
    String header="POST /api/audio/stream?token="+urlEncode(API_TOKEN)+"&sess="+sid+
      "&device_id="+urlEncode(DEVICE_ID)+"&rate=16000&bits=16&ch=1&total="+String(totalBytes)+
      ((vadFlags&1)?"&vad=1":"")+" HTTP/1.1\r\nHost: "+g_server_ip+
      ":"+String(SERVER_PORT)+"\r\nContent-Type: application/octet-stream\r\n"+
      ((vadFlags&1)?"Transfer-Encoding: chunked\r\n":
                    "Content-Length: "+String(totalBytes)+"\r\n")+
      "Connection: close\r\n\r\n";
    if (!writeAll((const uint8_t*)header.c_str(),header.length())) {
      socket.stop(); return false;
    }
    socket.setTimeout(5);
    sentBytes=0;
    wireLen=wireOff=wirePcm=0;
    response="";
    waiting=false;
    lastProgress=millis();
    DBG.printf("[AUDIO] POST attempt=%u session=%s bytes=%lu\n",
               (unsigned)attempts,sid.c_str(),(unsigned long)totalBytes);
    return true;
  }
  void retryStream(const char *reason) {
    lastError=reason;
    DBG.printf("[AUDIO] network retry: %s stored=%lu sent=%lu/%lu\n",
               reason,(unsigned long)storedBytes,(unsigned long)sentBytes,
               (unsigned long)totalBytes);
    socket.stop();
    waiting=false;
    response="";
    queueHead=queueTail=queueUsed=0;
    wireLen=wireOff=wirePcm=0;
    spill=true; // restart the POST from byte zero, using the durable PCM copy
    if (!recording && !closing) finishing=true;
    retryAt=millis()+1000;
  }
  void complete() {
    uint32_t lagMs=millis()-captureEndedMs;
    DBG.printf("[AUDIO] PASS received=%lu/%lu upload lag=%lu ms; saved PCM cleared\n",
               (unsigned long)actualBytes,(unsigned long)totalBytes,(unsigned long)lagMs);
    socket.stop();
    spool.close(); replay.close();
    LittleFS.remove(spoolPath());
    active=waiting=finishing=recording=closing=false;
    response="";
    Serial.printf("@AUDIO:READY,%lu\n",(unsigned long)lagMs);
  }
public:
  bool busy() const { return active; }
  bool failed() const { return faulted; }
  uint32_t clockNow(uint32_t) override { return millis(); }
  void error(const char *why) override { lastError=why; }
  void uartOverrun() { uartOverruns++; }
  const char *phase() const { return faulted?"failed":recording?"recording":active?"uploading":"ready"; }
  const char *failureReason() const { return lastError; }
  uint32_t accepted() const { return queuedBytes; }
  uint32_t stored() const { return storedBytes; }
  uint32_t sent() const { return sentBytes; }
  uint32_t total() const { return totalBytes; }
  uint32_t overrunCount() const { return uartOverruns; }
  uint8_t retries() const { return attempts ? attempts-1 : 0; }
  void begin() {
    fsReady=LittleFS.begin();
    DBG.printf("[AUDIO] LittleFS %s; public-network PCM spool\n",
               fsReady ? "ready" : "mount failed");
  }
  bool open(uint16_t session, uint32_t total, uint8_t flags) override {
    if (active || !fsReady) {
      DBG.printf("[AUDIO] START rejected: busy=%u flash=%u\n",
                 active ? 1 : 0, fsReady ? 1 : 0);
      return false;
    }
    if (WiFi.status()!=WL_CONNECTED || !g_server_ip.length()) {
      DBG.println("[AUDIO] START rejected: no Wi-Fi/server route");
      return false;
    }
    LittleFS.remove(spoolPath());
    FSInfo info;
    if (!LittleFS.info(info) || info.totalBytes-info.usedBytes < total+8192u) {
      DBG.println("[AUDIO] START rejected: not enough flash for full recording");
      return false;
    }
    spool=LittleFS.open(spoolPath(),"w");
    if (!spool) return false;
    totalBytes=total; actualBytes=storedBytes=0; response="";
    flashHead=flashTail=flashUsed=0; closing=false; lastError="none";
    queueHead=queueTail=queueUsed=0;
    queuedBytes=sentBytes=0; active=waiting=finishing=faulted=false;
    sessionId=session; vadFlags=flags; attempts=0;
    recording=active=true;
    /* Always keep the complete PCM on flash, but start the HTTP POST before
     * READY. Network writes are bounded in poll(); UART receive stays first
     * in loop(). If TCP falls behind, capture continues on flash. */
    spill=false;
    if (!connectStream()) {
      spill=true;
      retryAt=millis()+1000;
      DBG.println("[AUDIO] server unavailable; recording safely to flash");
    }
    Serial.print("@AUDIO:BUSY\n");
    DBG.printf("[AUDIO] READY bytes=%lu mode=flash-backed stream\n",
               (unsigned long)total);
    return true;
  }
  bool readyForPcm(uint16_t n) override {
    return active && recording && !faulted && spool && queuedBytes+n<=totalBytes && n<=FLASH_QUEUE_SIZE-flashUsed;
  }
  bool pcm(const uint8_t *p, uint16_t n) override {
    if (!readyForPcm(n))
      return false;
    size_t first=FLASH_QUEUE_SIZE-flashHead;
    if (first>n) first=n;
    memcpy(flashQueue+flashHead,p,first);
    memcpy(flashQueue,p+first,n-first);
    flashHead=(flashHead+n)%FLASH_QUEUE_SIZE;
    flashUsed+=n; queuedBytes+=n;
    return true;
  }
  bool finish(uint32_t actual) override {
    if (actual > totalBytes || queuedBytes != actual || finishing || waiting || closing ||
        (!(vadFlags&1) && actual!=totalBytes)) return false;
    actualBytes=(vadFlags&1)?actual:totalBytes;
    recording=false;
    captureEndedMs=millis();
    closing=true;
    lastProgress=millis();
    DBG.printf("[AUDIO] captured %lu bytes to flash; upload continues\n",
               (unsigned long)queuedBytes);
    return true;
  }
  void abort() override {
    DBG.printf("[AUDIO] FAILED: interrupted/incomplete, stored=%lu sent=%lu/%lu\n",
               (unsigned long)storedBytes,(unsigned long)sentBytes,
               (unsigned long)totalBytes);
    socket.stop(); spool.close(); replay.close();
    LittleFS.remove(spoolPath());
    active=waiting=finishing=recording=closing=false; faulted=true; response="";
    flashHead=flashTail=flashUsed=0;
    queueHead=queueTail=queueUsed=0;
    wireLen=wireOff=wirePcm=0;
    Serial.print("@AUDIO:FAIL\n");
  }
  void ack(uint16_t next, uint8_t flags) override {
    uint8_t frame[]={0xA5,0x5A,4,uint8_t(next),uint8_t(next>>8),1,0,flags,0};
    for (uint8_t i=2;i<8;i++) frame[8]^=frame[i];
    Serial.write(frame,sizeof(frame));
    if (next==0 || flags!=0)
      DBG.printf("[AUDIO] ACK -> F407 next=%u flags=%u\n",next,flags);
  }
  void poll() {
    if (!active) return;
    if (!flushCapture()) return;
    if (waiting) {
      while (socket.available()) {
        if (response.length()>=4096) { retryStream("response too large"); return; }
        response+=char(socket.read());
      }
      if (socket.connected() && int32_t(millis()-responseDue)<0) return;
      int split=response.indexOf("\r\n\r\n");
      DynamicJsonDocument doc(1536);
      bool ok=split>=0 && (response.startsWith("HTTP/1.1 200 ") ||
                            response.startsWith("HTTP/1.0 200 "));
      if (ok) ok=!deserializeJson(doc,response.substring(split+4));
      if (ok) ok=(doc["complete"]==true &&
                 doc["received"].as<uint32_t>()==actualBytes);
      if (ok) { complete(); return; }
      retryStream("server did not confirm complete recording");
      return;
    }
    if (!socket.connected()) {
      if (recording || closing) {
        if (!spill) retryStream("disconnected during capture");
        return;
      }
      if (int32_t(millis()-retryAt)<0) return;
      if (attempts>=10) { abort(); return; }
      if (!connectStream()) { retryAt=millis()+2000; return; }
      queueHead=queueTail=queueUsed=0;
      wireLen=wireOff=wirePcm=0;
      spill=true;
    }
    if (vadFlags&1) {
      pollChunked();
      return;
    }
    if (queueUsed || (finishing && sentBytes<totalBytes)) {
      int room=socket.availableForWrite();
      if (room>0) {
        static const uint8_t silence[512]={0};
        uint8_t fromFlash[512];
        const uint8_t *source=queue+queueTail;
        size_t n=queueUsed ? queueUsed : size_t(totalBytes-sentBytes);
        if (n>512) n=512;
        if (queueUsed && n>AUDIO_QUEUE_SIZE-queueTail) n=AUDIO_QUEUE_SIZE-queueTail;
        if (n>size_t(room)) n=size_t(room);
        if (!queueUsed && sentBytes<storedBytes) {
          if (n>storedBytes-sentBytes) n=storedBytes-sentBytes;
          if (recording || closing) return; // replay only after all pending PCM is durable
          if (!replay.seek(sentBytes,SeekSet) || replay.read(fromFlash,n)!=(int)n) {
            abort(); return;
          }
          source=fromFlash;
        } else if (!queueUsed) source=silence;
        if (n) {
          size_t sent=socket.write(source,n);
          if (sent) {
            if (queueUsed) {
              queueTail=(queueTail+sent)%AUDIO_QUEUE_SIZE;
              queueUsed-=sent;
            }
            sentBytes+=sent;
            lastProgress=millis();
          }
        }
      }
      if (uint32_t(millis()-lastProgress)>15000) {
        retryStream("TCP stalled 15s");
        return;
      }
    }
    if (finishing && sentBytes==totalBytes) {
      finishing=false; waiting=true; responseDue=millis()+15000;
    }
  }
};
static BridgeWire bridgeWire;
static EspAudioTransport audioTransport;
static BridgeAudio audioBridge(audioTransport);
static bool audioBusy() { return audioBridge.busy() || audioTransport.busy(); }
#endif

