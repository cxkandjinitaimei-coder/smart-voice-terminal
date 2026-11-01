#ifndef BRIDGE_TTS_PLAYBACK_H
#define BRIDGE_TTS_PLAYBACK_H

/*
 * bridge_tts_playback.h — ESP8266 语音播放桥
 *
 * 用法：AI 回复得到屏幕文字后调用 request(text)，主循环每圈调用 tick()，
 * F407 回传 0x08 ACK 时调用 ack(seq, payload, len)。本模块不解码 MP3，
 * 云端必须返回 16 kHz / 16 bit / mono 的 PCM WAV；ESP 只跳过 WAV 头，
 * 按协议分片转发 PCM 给 F407 的 MAX98357/MAX9835 I2S 驱动。
 *
 * 协议帧：A5 5A | type | seq(LE) | len(LE) | payload | XOR
 *   05 BEGIN：payload = rate(uint32 LE), bits, channels, reserved[2]
 *   06 PCM：payload = 16-bit little-endian mono PCM
 *   07 END：无 payload；F407 对每帧返回 08 + 状态（0 接收、1 缓冲满、2 错误）。
 */

#include <ESP8266WiFi.h>
#include <ArduinoJson.h>

class TtsPlayback {
  WiFiClient client;
  String pending;
  String headers;
  uint8_t wavHeader[44];
  uint8_t pcm[1024];
  uint16_t seq = 0;
  uint16_t waitingSeq = 0;
  uint16_t pcmLen = 0;
  uint16_t wavGot = 0;
  uint8_t waitingType = 0;
  bool retryBusy = false;
  uint32_t retryAt = 0, busyAt = 0;
  uint32_t bodyLeft = 0;
  uint32_t lastAt = 0;
  bool active = false;
  bool connecting = false;
  bool headerDone = false;
  bool wavDone = false;
  bool waitingAck = false;
  bool ending = false;
  const char *errorText = "ready";

  static String enc(const String &s) {
    String out; const char *hex = "0123456789ABCDEF";
    for (size_t i=0; i<s.length(); ++i) {
      uint8_t c = (uint8_t)s[i];
      if (isalnum(c) || c == '-' || c == '_' || c == '.') out += (char)c;
      else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
  }

  bool frame(uint8_t type, uint16_t n, const uint8_t *data, uint16_t len) {
    if (!data && len) return false;
    uint8_t h[7] = {0xA5, 0x5A, type, (uint8_t)n, (uint8_t)(n >> 8),
                    (uint8_t)len, (uint8_t)(len >> 8)};
    uint8_t x = 0;
    for (uint8_t i = 2; i < 7; ++i) x ^= h[i];
    if (Serial.write(h, sizeof(h)) != sizeof(h)) return false;
    if (len && Serial.write(data, len) != len) return false;
    if (len) for (uint16_t i = 0; i < len; ++i) x ^= data[i];
    Serial.write(x);
    return true;
  }

  void fail(const char *why) {
    cancel();
    errorText = why; client.stop(); active = connecting = headerDone = wavDone =
      waitingAck = ending = false; pcmLen = 0; bodyLeft = 0;
    pending=""; retryBusy=false; wavGot=0;
    DBG.printf("[TTS] FAILED: %s\n", why);
  }

  bool connectServer() {
    if (WiFi.status() != WL_CONNECTED || !g_server_ip.length()) return false;
    client.stop(); client.setTimeout(500);
    if (!client.connect(g_server_ip.c_str(), SERVER_PORT)) return false;
    StaticJsonDocument<512> d; d["text"] = pending;
    String body; serializeJson(d, body);
    String req = "POST /api/tts/speech?token=" + enc(API_TOKEN) +
      " HTTP/1.1\r\nHost: " + g_server_ip + ":" + String(SERVER_PORT) +
      "\r\nContent-Type: application/json\r\nContent-Length: " +
      String(body.length()) + "\r\nConnection: close\r\n\r\n" + body;
    if (client.write((const uint8_t*)req.c_str(), req.length()) != req.length()) {
      client.stop(); return false;
    }
    headers = ""; headerDone = wavDone = waitingAck = ending = false;
    pcmLen = 0; wavGot=0; bodyLeft = 0; retryBusy=false; seq = (uint16_t)(millis() & 0xFFFFu);
    connecting = true; lastAt = millis();
    DBG.println("[TTS] request sent");
    return true;
  }

  bool readExact(uint8_t *dst, uint16_t want) {
    // WAV 头可能跨多次 TCP 到达，进度必须跨 tick 保留。
    while (wavGot < want && client.available()) {
      int n = client.read(dst + wavGot, want - wavGot);
      if (n <= 0) break;
      wavGot += (uint16_t)n; lastAt=millis();
    }
    return wavGot == want;
  }

  void parseWav() {
    if (memcmp(wavHeader, "RIFF", 4) || memcmp(wavHeader + 8, "WAVE", 4) ||
        memcmp(wavHeader+12,"fmt ",4) || wavHeader[16]!=16 || wavHeader[17] ||
        wavHeader[18] || wavHeader[19] || wavHeader[20]!=1 || wavHeader[21] ||
        memcmp(wavHeader+36,"data",4)) { fail("response is not canonical PCM WAV"); return; }
    uint16_t channels = (uint16_t)wavHeader[22] | ((uint16_t)wavHeader[23] << 8);
    uint32_t rate = (uint32_t)wavHeader[24] | ((uint32_t)wavHeader[25] << 8) |
                    ((uint32_t)wavHeader[26] << 16) | ((uint32_t)wavHeader[27] << 24);
    uint16_t bits = (uint16_t)wavHeader[34] | ((uint16_t)wavHeader[35] << 8);
    uint8_t p[8] = {(uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16), (uint8_t)(rate >> 24),
                    (uint8_t)bits, (uint8_t)channels, 0, 0};
    if (rate != 16000u || bits != 16u || channels != 1u) { fail("TTS WAV must be 16k/16bit/mono"); return; }
    bodyLeft=(uint32_t)wavHeader[40] | ((uint32_t)wavHeader[41]<<8) |
      ((uint32_t)wavHeader[42]<<16) | ((uint32_t)wavHeader[43]<<24);
    if(!bodyLeft || (bodyLeft&1u) || bodyLeft>32000u*60u) {fail("invalid PCM length");return;}
    memcpy(pcm,p,sizeof(p));pcmLen=sizeof(p);waitingType=0x05;
    waitingSeq = seq; waitingAck = true;
    if (!frame(0x05, seq, p, sizeof(p))) { fail("BEGIN UART failed"); return; }
    wavDone = true; lastAt = millis();
  }

public:
  void request(const String &text) {
    String s = text; s.trim();
    if (!s.length()) return;
    if (s.length() > 240) {
      unsigned cut=240;
      while(cut && ((uint8_t)s[cut]&0xC0)==0x80) --cut;
      s=s.substring(0,cut);
    }
    /* 播放期间只保留当前句，避免多个 AI 回复把 UART 音频帧挤爆。 */
    if (active) return;
    pending = s; errorText = "queued";
  }
  bool busy() const { return active; }
  const char *status() const { return errorText; }

  /* 维护/录音优先：终止当前句并清空待播，绝不在下一圈自动重连。
   * END 是正常收尾协议；旧 F407 根本不会通过能力门禁开始播放。 */
  void cancel() {
    if(active && wavDone) frame(0x07,++seq,nullptr,0);
    client.stop();pending="";active=connecting=headerDone=wavDone=waitingAck=ending=false;
    retryBusy=false;pcmLen=wavGot=0;bodyLeft=0;
  }

  void ack(uint16_t ackSeq, const uint8_t *data, uint16_t len) {
    if (!active || !waitingAck || ackSeq != waitingSeq || !data || !len) return;
    uint8_t status = data[0];
    if (status == 1u) {
      if(!retryBusy) busyAt=millis();
      retryBusy=true;retryAt=millis()+20u;return;
    }
    if (status != 0u) { fail("F407 rejected TTS frame"); return; }
    waitingAck = false; retryBusy=false; lastAt = millis();
  }

  void tick() {
    if (!active && pending.length()) {
      if (connectServer()) { active = true; connecting = true; }
      else {fail("TTS server unavailable; wait for next reply");return;}
    }
    if (!active) return;
    if (millis() - lastAt > (headerDone?15000u:60000u)) { cancel();fail("TTS timeout"); return; }
    if (connecting && !headerDone) {
      while (client.available()) {
        headers += (char)client.read();
        int split = headers.indexOf("\r\n\r\n");
        if (split >= 0) {
          if (!headers.startsWith("HTTP/1.1 200 ") && !headers.startsWith("HTTP/1.0 200 ")) { fail("TTS HTTP error"); return; }
          String lower=headers;lower.toLowerCase();
          if(lower.indexOf("transfer-encoding:")>=0) {fail("chunked WAV unsupported");return;}
          headerDone = true; connecting = false; headers = headers.substring(split + 4); lastAt = millis();
          break;
        }
        if (headers.length() > 2048) { fail("TTS headers too large"); return; }
      }
      if (!headerDone) return;
    }
    if (headerDone && !wavDone) {
      if (!readExact(wavHeader, sizeof(wavHeader))) return;
      parseWav(); if (!active) return;
      return;
    }
    if (waitingAck) {
      if(retryBusy) {
        if(millis()-busyAt>2000u) {cancel();fail("F407 playback buffer stalled");return;}
        if((int32_t)(millis()-retryAt)>=0) {
          frame(waitingType,waitingSeq,pcm,pcmLen);retryAt=millis()+50u;
        }
      }
      return;
    }
    if (!ending) {
      if (bodyLeft) {
        // 16-bit PCM 只发偶数字节；留在 TCP 缓冲中的单字节等下一次补齐。
        uint32_t available=(uint32_t)client.available();
        uint16_t want=(uint16_t)(bodyLeft>sizeof(pcm)?sizeof(pcm):bodyLeft);
        if(available<want)want=(uint16_t)available;
        want &= (uint16_t)~1u;
        int n = want?client.read(pcm,want):0;
        if (n > 0) {
          if(n&1) {cancel();fail("unaligned PCM short read");return;}
          bodyLeft-=(uint16_t)n;waitingType=0x06;
          pcmLen = (uint16_t)n; waitingSeq = ++seq; waitingAck = true;
          if (!frame(0x06, waitingSeq, pcm, pcmLen)) { fail("PCM UART failed"); return; }
          lastAt = millis(); return;
        }
      }
      if (!bodyLeft) {
        waitingType=0x07;pcmLen=0;
        waitingSeq = ++seq; waitingAck = true; ending = true;
        if (!frame(0x07, waitingSeq, nullptr, 0)) fail("END UART failed");
      } else if(!client.connected() && (uint32_t)client.available()<bodyLeft) {cancel();fail("truncated WAV");}
      return;
    }
    if (ending && !waitingAck) {
      DBG.println("[TTS] playback stream accepted");
      client.stop(); pending = ""; active = false; ending = false; errorText = "ready";
    }
  }
};

#endif

