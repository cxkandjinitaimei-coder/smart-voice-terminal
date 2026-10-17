/*
 * 文件：bridge_audio_esp.h
 *
 * 职责：实现 AudioTransport，将 F407 的 PCM 经 RAM 缓冲、LittleFS留底后上传服务端。
 * 使用：在主程序配置/共享状态定义后包含；setup调用audioTransport.begin()挂载文件系统。
 *   主循环先排空UART，再调用audioBridge.tick()和audioTransport.poll()；audioBusy()覆盖两层占用。
 *   open/pcm/finish/ack由BridgeAudio调用，业务代码不要绕过会话状态直接写缓存。
 * 数据路径：2048B串口块 → 4KiB待落盘环 → 每轮最多512B写Flash → 8KiB网络环 → TCP。
 *   每份PCM同时留底；网络跟不上时进入spill，录完后从文件补发，避免只靠小RAM环承受公网延迟。
 * 文件：/mic-audio.pcm仅为本次传输缓存；只有服务端确认完整后清理，失败中止也统一清理。
 * 上传：固定长度录音使用Content-Length，VAD实际长度到END才确定，因此使用chunked。
 * 状态：active表示整个会话占用；recording表示接收PCM；closing表示排空落盘队列；
 *   finishing表示发送剩余正文；waiting表示等服务端确认；faulted保留失败状态供上报。
 * 重试：连接中断后保留Flash副本；重开POST从第0字节回放，不从旧TCP中间偏移接续。
 *   短写只推进实际发送字节，完整chunk及终止块不能重复或漏发。
 * 依赖：Arduino/WiFiClient/LittleFS/ArduinoJson，以及主文件先定义的DBG、API_TOKEN、
 *   SERVER_PORT和g_server_ip；实现头只在主文件包含一次。
 * 边界：READY在上传完整确认后发出，不意味着云端ASR/AI已处理完；低速网络仍可能需要补传。
 */

#ifndef BRIDGE_AUDIO_ESP_H
#define BRIDGE_AUDIO_ESP_H
#include "bridge_audio.h"

// 音频传输：RAM 接收、LittleFS 留底、TCP 分段发送；上传确认前保持 busy。
class EspAudioTransport : public AudioTransport {
  // 单会话资源：网络连接、写入文件和回放文件随事务创建/关闭，不跨录音并用。
  WiFiClient socket;
  File spool, replay;
  String response;
  uint32_t responseDue = 0, totalBytes = 0, actualBytes = 0, storedBytes = 0;
  static const size_t AUDIO_QUEUE_SIZE = 8192;
  static const size_t FLASH_QUEUE_SIZE = 4096;
  static const char *spoolPath() {
    return "/mic-audio.pcm";
  }
  // queue用于低延迟发送；flashQueue用于快速接收后延后落盘，两者头尾指针分别维护。
  uint8_t queue[AUDIO_QUEUE_SIZE];
  uint8_t flashQueue[FLASH_QUEUE_SIZE];
  size_t flashHead = 0, flashTail = 0, flashUsed = 0;
  size_t queueHead = 0, queueTail = 0, queueUsed = 0;
  // queuedBytes为已交给适配器的PCM，storedBytes为已落盘，sentBytes为本次POST已发送。
  // 这三种进度含义不同；TCP写入成功还不是服务端已保存确认。
  uint32_t queuedBytes = 0, sentBytes = 0, lastProgress = 0, captureEndedMs = 0;
  uint32_t retryAt = 0;
  uint16_t sessionId = 0;
  uint8_t vadFlags = 0, attempts = 0;
  // active占用整个事务；waiting/finishing/faulted区分确认、正文排空和失败。
  bool active = false, waiting = false, finishing = false, faulted = false;
  bool fsReady = false, recording = false, spill = false, closing = false;
  const char *lastError = "none";
  uint32_t uartOverruns = 0;
  // chunked暂存包含十六进制长度行、最多512B PCM和尾部CRLF；wireOff用于短写续传。
  uint8_t wireChunk[524];
  size_t wireLen = 0, wireOff = 0, wirePcm = 0;
  bool wireFromQueue = false, wireFinal = false;

  // 每轮最多写 512B Flash，放在串口接收之后执行；ACK 路径只复制 RAM。
  bool flushCapture() {
    // 优先消化待落盘环，单次限制512B，缩短Flash写入对下一轮UART接收的影响。
    if (flashUsed) {
      size_t n = flashUsed;
      if (n > 512)
        n = 512;
      if (n > FLASH_QUEUE_SIZE - flashTail)
        n = FLASH_QUEUE_SIZE - flashTail;
      const uint8_t *p = flashQueue + flashTail;
      if (spool.write(p, n) != n) {
        lastError = "flash write failed";
        abort();
        return false;
      }
      storedBytes += n;
      // 只有网络环装得下才继续实时推送；进入spill后后续数据统一靠完整Flash副本补发。
      if (!spill && n <= AUDIO_QUEUE_SIZE - queueUsed) {
        size_t first = AUDIO_QUEUE_SIZE - queueHead;
        if (first > n)
          first = n;
        memcpy(queue + queueHead, p, first);
        memcpy(queue, p + first, n - first);
        queueHead = (queueHead + n) % AUDIO_QUEUE_SIZE;
        queueUsed += n;
      } else
        spill = true;
      flashTail = (flashTail + n) % FLASH_QUEUE_SIZE;
      flashUsed -= n;
    }
    // END 后先排空待落盘数据，再打开回放文件用于补传。
    if (closing && !flashUsed) {
      spool.close();
      replay = LittleFS.open(spoolPath(), "r");
      if (!replay || replay.size() != storedBytes) {
        lastError = "flash replay unavailable";
        abort();
        return false;
      }
      closing = false;
      finishing = true;
      lastProgress = millis();
      DBG.printf("[AUDIO] captured %lu bytes; durable stream drain\n", (unsigned long)storedBytes);
    }
    return true;
  }

  // VAD 长度在 END 时确定，使用 HTTP chunked；短写从 wireOff 续传。
  void pollChunked() {
    // 没有未发完的HTTP块时才准备新块；上次短写的块必须先续完。
    if (!wireLen) {
      if (queueUsed || (finishing && sentBytes < actualBytes)) {
        uint8_t pcm[512];
        size_t n = queueUsed ? queueUsed : size_t(actualBytes - sentBytes);
        if (n > sizeof(pcm))
          n = sizeof(pcm);
        wireFromQueue = queueUsed != 0;
        // 实时队列可能跨环边界，先复制为连续小块；录完补传则从文件按sentBytes读取。
        if (wireFromQueue) {
          for (size_t i = 0; i < n; i++)
            pcm[i] = queue[(queueTail + i) % AUDIO_QUEUE_SIZE];
        } else {
          // Flash回放只在写文件关闭后开始，避免把尚未耐久的数据当成完整正文读取。
          if (recording || closing)
            return;
          if (!replay.seek(sentBytes, SeekSet) || replay.read(pcm, n) != (int)n) {
            abort();
            return;
          }
        }
        int h = snprintf((char *)wireChunk, sizeof(wireChunk), "%X\r\n", (unsigned)n);
        if (h < 0 || size_t(h) + n + 2 > sizeof(wireChunk)) {
          abort();
          return;
        }
        memcpy(wireChunk + h, pcm, n);
        wireChunk[h + n] = '\r';
        wireChunk[h + n + 1] = '\n';
        wireLen = size_t(h) + n + 2;
        wirePcm = n;
        wireOff = 0;
        wireFinal = false;
      } else if (finishing && sentBytes == actualBytes) {
        memcpy(wireChunk, "0\r\n\r\n", 5);
        wireLen = 5;
        wirePcm = 0;
        wireOff = 0;
        wireFinal = true;
      } else
        return;
    }
    // 按当前TCP可写空间发送，不把write返回值假定为请求长度。
    int room = socket.availableForWrite();
    if (room > 0) {
      size_t n = wireLen - wireOff;
      if (n > size_t(room))
        n = size_t(room);
      size_t wrote = socket.write(wireChunk + wireOff, n);
      if (wrote) {
        wireOff += wrote;
        lastProgress = millis();
      }
    }
    // 仅完整写出一个 HTTP 块后推进 PCM 计数；终止块写完转等待响应。
    if (wireOff == wireLen) {
      // chunked终止块0 CRLF CRLF全部发出后，才能转入等待服务端确认。
      if (wireFinal) {
        finishing = false;
        waiting = true;
        responseDue = millis() + 15000;
      } else {
        if (wireFromQueue) {
          queueTail = (queueTail + wirePcm) % AUDIO_QUEUE_SIZE;
          queueUsed -= wirePcm;
        }
        sentBytes += wirePcm;
      }
      wireLen = wireOff = wirePcm = 0;
    }
    if (!waiting && uint32_t(millis() - lastProgress) > 15000)
      retryStream("TCP stalled 15s");
  }

  // 请求头允许 TCP 短写；每次让出 Wi-Fi 执行时间，写零字节即失败。
  bool writeAll(const uint8_t *p, size_t n) {
    while (n) {
      size_t sent = socket.write(p, n);
      if (!sent)
        return false;
      p += sent;
      n -= sent;
      delay(0);
    }
    return true;
  }
  // token和会话参数按URL编码，避免特殊字符改变查询参数边界。
  static String urlEncode(const String &s) {
    String out;
    const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.length(); i++) {
      uint8_t c = s[i];
      if (isalnum(c) || c == '-' || c == '_' || c == '.')
        out += char(c);
      else {
        out += '%';
        out += hex[c >> 4];
        out += hex[c & 15];
      }
    }
    return out;
  }
  // 建立带认证的上传会话；VAD 用 chunked，固定录音用 Content-Length。
  bool connectStream() {
    if (WiFi.status() != WL_CONNECTED || !g_server_ip.length())
      return false;
    socket.stop();
    socket.setTimeout(500);
    attempts++;
    if (!socket.connect(g_server_ip.c_str(), SERVER_PORT))
      return false;
    socket.setNoDelay(true);
    // 每次POST生成独立上传会话标识，网络重试不会接着旧HTTP半包发送。
    String sid = String(ESP.getChipId(), HEX) + "-" + String(micros(), HEX) + "-" +
                 String(sessionId) + "-" + String(attempts);
    String header = "POST /api/audio/stream?token=" + urlEncode(API_TOKEN) + "&sess=" + sid +
                    "&rate=16000&bits=16&ch=1&total=" + String(totalBytes) +
                    ((vadFlags & 1) ? "&vad=1" : "") + " HTTP/1.1\r\nHost: " + g_server_ip + ":" +
                    String(SERVER_PORT) + "\r\nContent-Type: application/octet-stream\r\n" +
                    ((vadFlags & 1) ? "Transfer-Encoding: chunked\r\n"
                                    : "Content-Length: " + String(totalBytes) + "\r\n") +
                    "Connection: close\r\n\r\n";
    if (!writeAll((const uint8_t *)header.c_str(), header.length())) {
      socket.stop();
      return false;
    }
    socket.setTimeout(5);
    sentBytes = 0;
    wireLen = wireOff = wirePcm = 0;
    response = "";
    waiting = false;
    lastProgress = millis();
    DBG.printf("[AUDIO] POST attempt=%u session=%s bytes=%lu\n", (unsigned)attempts, sid.c_str(),
               (unsigned long)totalBytes);
    return true;
  }
  // 重试清空网络队列，从 Flash 的第零字节重传；保留已采集的 PCM。
  void retryStream(const char *reason) {
    lastError = reason;
    DBG.printf("[AUDIO] network retry: %s stored=%lu sent=%lu/%lu\n", reason,
               (unsigned long)storedBytes, (unsigned long)sentBytes, (unsigned long)totalBytes);
    socket.stop();
    waiting = false;
    response = "";
    queueHead = queueTail = queueUsed = 0;
    wireLen = wireOff = wirePcm = 0;
    // 丢弃仅供实时发送的网络环，不删除Flash；新连接须从头发送完整正文。
    spill = true;
    if (!recording && !closing)
      finishing = true;
    retryAt = millis() + 1000;
  }
  // 只有服务器确认实际字节数完整后，才删除 PCM 并发 READY 释放录音。
  void complete() {
    uint32_t lagMs = millis() - captureEndedMs;
    DBG.printf("[AUDIO] PASS received=%lu/%lu upload lag=%lu ms; saved PCM cleared\n",
               (unsigned long)actualBytes, (unsigned long)totalBytes, (unsigned long)lagMs);
    socket.stop();
    spool.close();
    replay.close();
    LittleFS.remove(spoolPath());
    active = waiting = finishing = recording = closing = false;
    response = "";
    Serial.printf("@AUDIO:READY,%lu\n", (unsigned long)lagMs);
  }

public:
  // active包含录制、补传及等响应；仅凭MCU停止采样不能重新开放下一段录音。
  bool busy() const {
    return active;
  }
  bool failed() const {
    return faulted;
  }
  uint32_t clockNow(uint32_t) override {
    return millis();
  }
  void error(const char *why) override {
    lastError = why;
  }
  void uartOverrun() {
    uartOverruns++;
  }
  const char *phase() const {
    return faulted ? "failed" : recording ? "recording" : active ? "uploading" : "ready";
  }
  const char *failureReason() const {
    return lastError;
  }
  uint32_t accepted() const {
    return queuedBytes;
  }
  uint32_t stored() const {
    return storedBytes;
  }
  uint32_t sent() const {
    return sentBytes;
  }
  uint32_t total() const {
    return totalBytes;
  }
  uint32_t overrunCount() const {
    return uartOverruns;
  }
  uint8_t retries() const {
    return attempts ? attempts - 1 : 0;
  }
  // 只挂载LittleFS并保存结果；不会因挂载失败自动格式化或擦掉舵机配置。
  void begin() {
    fsReady = LittleFS.begin();
    DBG.printf("[AUDIO] LittleFS %s; public-network PCM spool\n",
               fsReady ? "ready" : "mount failed");
  }
  // 申请：拒绝占用、未挂载或无路由；空间须足够保存整段 PCM 及余量。
  bool open(uint16_t session, uint32_t total, uint8_t flags) override {
    if (active || !fsReady) {
      DBG.printf("[AUDIO] START rejected: busy=%u flash=%u\n", active ? 1 : 0, fsReady ? 1 : 0);
      return false;
    }
    if (WiFi.status() != WL_CONNECTED || !g_server_ip.length()) {
      DBG.println("[AUDIO] START rejected: no Wi-Fi/server route");
      return false;
    }
    // 只在已确认没有旧active会话后清理上次临时音频，再核查整段所需空间。
    LittleFS.remove(spoolPath());
    FSInfo info;
    // 预留整段录音容量及8KiB余量；空间不足不向F407发开始成功ACK。
    if (!LittleFS.info(info) || info.totalBytes - info.usedBytes < total + 8192u) {
      DBG.println("[AUDIO] START rejected: not enough flash for full recording");
      return false;
    }
    spool = LittleFS.open(spoolPath(), "w");
    if (!spool)
      return false;
    totalBytes = total;
    actualBytes = storedBytes = 0;
    response = "";
    flashHead = flashTail = flashUsed = 0;
    closing = false;
    lastError = "none";
    queueHead = queueTail = queueUsed = 0;
    queuedBytes = sentBytes = 0;
    active = waiting = finishing = faulted = false;
    sessionId = session;
    vadFlags = flags;
    attempts = 0;
    // 占用：先准备留底文件，再尝试连接服务器；连接失败仍可落盘补传。
    recording = active = true;

    spill = false;
    if (!connectStream()) {
      spill = true;
      retryAt = millis() + 1000;
      DBG.println("[AUDIO] server unavailable; recording safely to flash");
    }
    Serial.print("@AUDIO:BUSY\n");
    DBG.printf("[AUDIO] READY bytes=%lu mode=flash-backed stream\n", (unsigned long)total);
    return true;
  }
  // 一次完整块必须同时满足会话仍接收、总长未超限、Flash待写环有空间。
  bool readyForPcm(uint16_t n) override {
    return active && recording && !faulted && spool && queuedBytes + n <= totalBytes &&
           n <= FLASH_QUEUE_SIZE - flashUsed;
  }
  // 接收：只复制到 Flash 待写环，不在 ACK 临界路径擦写 Flash。
  bool pcm(const uint8_t *p, uint16_t n) override {
    if (!readyForPcm(n))
      return false;
    // 跨环写分为尾部与开头两段memcpy，确认前已完成RAM复制，不保留外部指针。
    size_t first = FLASH_QUEUE_SIZE - flashHead;
    if (first > n)
      first = n;
    memcpy(flashQueue + flashHead, p, first);
    memcpy(flashQueue, p + first, n - first);
    flashHead = (flashHead + n) % FLASH_QUEUE_SIZE;
    flashUsed += n;
    queuedBytes += n;
    return true;
  }
  // 收尾：固定录音长度必须匹配；VAD 采用实际长度，然后等待落盘完成。
  bool finish(uint32_t actual) override {
    if (actual > totalBytes || queuedBytes != actual || finishing || waiting || closing ||
        (!(vadFlags & 1) && actual != totalBytes))
      return false;
    actualBytes = (vadFlags & 1) ? actual : totalBytes;
    // 停止接受新PCM，记录采集结束时刻；closing等待剩余RAM数据全部落盘。
    recording = false;
    captureEndedMs = millis();
    closing = true;
    lastProgress = millis();
    DBG.printf("[AUDIO] captured %lu bytes to flash; upload continues\n",
               (unsigned long)queuedBytes);
    return true;
  }
  // 中止：关闭连接与文件、清空队列并通知 F407；faulted 保留错误状态。
  void abort() override {
    DBG.printf("[AUDIO] FAILED: interrupted/incomplete, stored=%lu sent=%lu/%lu\n",
               (unsigned long)storedBytes, (unsigned long)sentBytes, (unsigned long)totalBytes);
    socket.stop();
    spool.close();
    replay.close();
    LittleFS.remove(spoolPath());
    active = waiting = finishing = recording = closing = false;
    faulted = true;
    response = "";
    flashHead = flashTail = flashUsed = 0;
    queueHead = queueTail = queueUsed = 0;
    wireLen = wireOff = wirePcm = 0;
    Serial.print("@AUDIO:FAIL\n");
  }
  // ACK是串口接收侧确认，只说明可推进数据块；不能用于判断ASR或服务端处理完成。
  void ack(uint16_t next, uint8_t flags) override {
    uint8_t frame[] = {0xA5, 0x5A, 4, uint8_t(next), uint8_t(next >> 8), 1, 0, flags, 0};
    for (uint8_t i = 2; i < 8; i++)
      frame[8] ^= frame[i];
    Serial.write(frame, sizeof(frame));
    if (next == 0 || flags != 0)
      DBG.printf("[AUDIO] ACK -> F407 next=%u flags=%u\n", next, flags);
  }
  // 推进一轮：落盘 → 响应校验/重连 → 有界发送；UART 在主循环先处理。
  void poll() {
    if (!active)
      return;
    if (!flushCapture())
      return;
    // 确认：HTTP 200 且 complete=true、received=实际长度，才视为上传成功。
    if (waiting) {
      while (socket.available()) {
        if (response.length() >= 4096) {
          retryStream("response too large");
          return;
        }
        response += char(socket.read());
      }
      if (socket.connected() && int32_t(millis() - responseDue) < 0)
        return;
      int split = response.indexOf("\r\n\r\n");
      DynamicJsonDocument doc(1536);
      bool ok = split >= 0 &&
                (response.startsWith("HTTP/1.1 200 ") || response.startsWith("HTTP/1.0 200 "));
      if (ok)
        ok = !deserializeJson(doc, response.substring(split + 4));
      // 要求服务端同时确认完整标记和实际长度，只有HTTP 200不能作为录音成功。
      if (ok)
        ok = (doc["complete"] == true && doc["received"].as<uint32_t>() == actualBytes);
      if (ok) {
        complete();
        return;
      }
      retryStream("server did not confirm complete recording");
      return;
    }
    // 断线：录制期间优先落盘；结束后最多重连十次，从头回放完整文件。
    if (!socket.connected()) {
      if (recording || closing) {
        if (!spill)
          retryStream("disconnected during capture");
        return;
      }
      if (int32_t(millis() - retryAt) < 0)
        return;
      // 超过连接尝试上限后进入统一abort路径，防止无网络时永远保持busy。
      if (attempts >= 10) {
        abort();
        return;
      }
      if (!connectStream()) {
        retryAt = millis() + 2000;
        return;
      }
      queueHead = queueTail = queueUsed = 0;
      wireLen = wireOff = wirePcm = 0;
      spill = true;
    }
    if (vadFlags & 1) {
      pollChunked();
      return;
    }
    if (queueUsed || (finishing && sentBytes < totalBytes)) {
      // 按当前TCP可写空间发送，不把write返回值假定为请求长度。
      int room = socket.availableForWrite();
      if (room > 0) {
        static const uint8_t silence[512] = {0};
        uint8_t fromFlash[512];
        const uint8_t *source = queue + queueTail;
        size_t n = queueUsed ? queueUsed : size_t(totalBytes - sentBytes);
        if (n > 512)
          n = 512;
        if (queueUsed && n > AUDIO_QUEUE_SIZE - queueTail)
          n = AUDIO_QUEUE_SIZE - queueTail;
        if (n > size_t(room))
          n = size_t(room);
        if (!queueUsed && sentBytes < storedBytes) {
          if (n > storedBytes - sentBytes)
            n = storedBytes - sentBytes;
          // Flash回放只在写文件关闭后开始，避免把尚未耐久的数据当成完整正文读取。
          if (recording || closing)
            return;
          if (!replay.seek(sentBytes, SeekSet) || replay.read(fromFlash, n) != (int)n) {
            abort();
            return;
          }
          source = fromFlash;
        } else if (!queueUsed)
          source = silence;
        if (n) {
          size_t sent = socket.write(source, n);
          if (sent) {
            if (queueUsed) {
              queueTail = (queueTail + sent) % AUDIO_QUEUE_SIZE;
              queueUsed -= sent;
            }
            sentBytes += sent;
            lastProgress = millis();
          }
        }
      }
      if (uint32_t(millis() - lastProgress) > 15000) {
        retryStream("TCP stalled 15s");
        return;
      }
    }
    // 所有固定长度字节发送完后等服务器确认，尚未释放 busy。
    if (finishing && sentBytes == totalBytes) {
      finishing = false;
      waiting = true;
      responseDue = millis() + 15000;
    }
  }
};
static BridgeWire bridgeWire;
static EspAudioTransport audioTransport;
static BridgeAudio audioBridge(audioTransport);
static bool audioBusy() {
  return audioBridge.busy() || audioTransport.busy();
}
#endif
