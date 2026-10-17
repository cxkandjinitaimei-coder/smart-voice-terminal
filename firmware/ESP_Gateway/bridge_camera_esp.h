/*
 * 文件：bridge_camera_esp.h
 *
 * 职责：管理ESP端单张拍照事务，向F407发请求、接图落盘并上传服务端。
 * 使用：在mcuFrame及网络配置已可见后包含；主循环调用camera.tick(allowed)。
 *   UART分发将相机二进制帧交给camera.frame，将@CAM文本交给camera.text。
 *   allowed仅控制能否认领新任务；已有任务即使allowed变为false也仍会推进。
 * 流程：空闲轮询任务 → 校验job/ticket及空间 → SNAP → 串口收图 → HTTP上传 → 结果确认。
 * 状态：0空闲、1等F407图像、2连接上传端、3分段发送、4等HTTP响应、5回报错误。
 * 文件：/camera.rgb565保存153600B原始像素，分段读写；不用整帧RAM缓存。
 * 上传：携带Bearer认证、任务ticket及整帧CRC；服务端回202且长度/CRC一致才完成。
 * 重试：上传失败最多三次，每次重新打开文件从头发送；TCP短写保留头部和正文各自偏移。
 * 超时：全事务45s；F407接图无进展6s；发送/响应无进展12s，失败统一进入报告/释放流程。
 * 依赖：bridge_camera.h、LittleFS、HTTPClient、g_server_ip、DEVICE_ID、API_TOKEN和mcuFrame。
 *   实现头只在主文件包含一次；busy在接图结束后仍有效，直到上传确认或失败清理。
 * 边界：这是拍照上传通道，不是本地显示驱动或连续视频流。
 */

#ifndef BRIDGE_CAMERA_ESP_H
#define BRIDGE_CAMERA_ESP_H
#include <ESP8266HTTPClient.h>
#include "bridge_camera.h"

// 单张拍照：LittleFS 接图 → HTTP 上传 → 服务端完整性确认；不提供视频流。
class EspCamera : public CameraSink {
  File image;
  WiFiClient tcp;
  CameraReceiver receiver;
  // job绑定服务器任务，ticket授权这次照片上传；请求间不能串用或混用旧票据。
  String job, ticket, error, response, header;
  uint16_t sid = 0;
  uint32_t started = 0, progress = 0, lastPoll = 0, crc32 = 0;
  uint32_t sent = 0;
  // fill/off为当前512B图像缓冲的有效长度/已发送偏移；headerOff单独跟踪请求头。
  size_t fill = 0, off = 0, headerOff = 0;
  uint8_t chunk[512];
  // phase：0空闲、1串口接图、2连接、3上传、4等响应、5报错。
  uint8_t phase = 0, attempt = 0;
  static const char *path() {
    return "/camera.rgb565";
  }
  // 构造设备接口URL；沿用当前可达服务路由与稳定设备ID。
  String base(const char *route) {
    return String("http://") + g_server_ip + ":" + String(SERVER_PORT) + route +
           "?device_id=" + DEVICE_ID;
  }
  String auth() {
    return String("Bearer ") + API_TOKEN;
  }
  // 最终收尾：通知 F407，关闭文件/连接，删除缓存并释放任务占用。
  void release(bool ok) {
    Serial.printf("@CAM:%s,%u\n", ok ? "DONE" : "FAIL", sid);
    image.close();
    tcp.stop();
    receiver.reset();
    phase = 0;
    job = "";
    ticket = "";
    LittleFS.remove(path());
  }
  // 先关闭IO并记录原因，转phase=5；报告和最终清理交给下一轮tick统一执行。
  void failed(const String &why) {
    error = why;
    image.close();
    tcp.stop();
    phase = 5;
  }
  // 每次重连均从文件起点上传；ticket 和 CRC 随请求头发送。
  bool uploadConnect() {
    tcp.setTimeout(1200);
    if (WiFi.status() != WL_CONNECTED || !tcp.connect(g_server_ip.c_str(), SERVER_PORT))
      return false;
    image = LittleFS.open(path(), "r");
    if (!image || image.size() != 153600)
      return false;
    char hex[9];
    snprintf(hex, sizeof(hex), "%08lX", (unsigned long)crc32);
    header = String("POST /api/camera/device/upload?device_id=") + DEVICE_ID + "&job_id=" + job +
             " HTTP/1.0\r\nHost: " + g_server_ip + "\r\nAuthorization: " + auth() +
             "\r\nX-Capture-Ticket: " + ticket +
             "\r\nContent-Type: application/x-rgb565\r\nX-Image-CRC32: " + hex +
             "\r\nContent-Length: 153600\r\nConnection: close\r\n\r\n";
    headerOff = 0;
    sent = 0;
    fill = off = 0;
    response = "";
    progress = millis();
    return true;
  }
  // 失败最多尝试三次；期间仍保留拍照任务和 Flash 文件。
  void retry() {
    image.close();
    tcp.stop();
    if (++attempt >= 3)
      failed("HTTP upload failed after 3 attempts");
    else
      phase = 2;
  }
  // 小型轮询/报错接口，设置1200ms超时；它仍是阻塞HTTP调用，不用于大图正文。
  bool postJSON(const char *route, const String &body, String &out) {
    WiFiClient c;
    HTTPClient http;
    http.setTimeout(1200);
    http.setReuse(false);
    if (!http.begin(c, base(route)))
      return false;
    http.addHeader("Authorization", auth());
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    out = code > 0 ? http.getString() : "";
    http.end();
    return code >= 200 && code < 300;
  }

public:
  EspCamera() : receiver(*this) {}
  // 任何非0阶段均占用事务，接图结束进入上传时不能立刻放开音频/OTA。
  bool busy() const {
    return phase != 0;
  }
  // START 在 DATA 前可能因丢 ACK 重传；此时重新打开空缓存即可。
  bool beginImage() override {

    image.close();
    image = LittleFS.open(path(), "w");
    return bool(image);
  }
  bool appendImage(const uint8_t *p, size_t n) override {
    return image && image.write(p, n) == n;
  }
  // CameraReceiver已完成整帧CRC检查；关闭写文件后转到网络连接阶段。
  void endImage(uint32_t c) override {
    image.close();
    crc32 = c;
    phase = 2;
    progress = millis();
  }
  // 相机ACK使用0x33，载荷为会话LE16和flag；永久错误进入报告态。
  void cameraAck(uint16_t id, uint16_t seq, uint8_t flag) override {
    uint8_t p[] = {uint8_t(id), uint8_t(id >> 8), flag};
    mcuFrame(0x33, seq, p, 3);
    if (flag == 2)
      failed("MCU camera frame invalid or Flash write failed");
  }
  // 收到完整相机帧后立即处理；报告态不再接收数据，避免失败后继续追加文件。
  void frame(uint8_t t, uint16_t seq, const uint8_t *p, uint16_t n) {
    if (phase && phase != 5) {
      receiver.frame(t, seq, p, n);
      progress = millis();
    }
  }
  // 只消费@CAM命名空间；ERROR还须匹配当前sid，迟到的旧会话错误不影响新任务。
  bool text(const String &line) {
    if (!line.startsWith("@CAM:"))
      return false;
    if (line.startsWith(String("@CAM:ERROR,") + sid + ","))
      failed(line.substring((String("@CAM:ERROR,") + sid + ",").length()));
    return true;
  }
  // 每轮推进状态；只有空闲且 allowed 时才轮询新任务。
  void tick(bool allowed) {
    const uint32_t now = millis();
    // 认领：校验任务 ID 与票据，检查空间，建立会话后发送 SNAP。
    if (!phase) {
      if (!allowed || WiFi.status() != WL_CONNECTED || !g_server_ip.length() ||
          now - lastPoll < 1500)
        return;
      lastPoll = now;
      String out;
      if (!postJSON("/api/camera/device/poll", "{\"ready\":true,\"protocol_version\":1}", out))
        return;
      StaticJsonDocument<1024> doc;
      if (deserializeJson(doc, out) || !doc["accepted"].as<bool>())
        return;
      // 仅接受已认领且结构合法的任务；job应为32字符，ticket不能为空。
      job = doc["job_id"].as<String>();
      ticket = doc["ticket"].as<String>();
      if (job.length() != 32 || !ticket.length())
        return;
      FSInfo fs;
      error = "";
      attempt = 0;
      started = progress = now;
      // 会话号回绕时跳过0，因为0在接收器中表示未启用。
      if (++sid == 0)
        ++sid;
      receiver.arm(sid);
      phase = 1;
      if (!LittleFS.info(fs) || fs.totalBytes - fs.usedBytes < 180000) {
        failed("LittleFS needs 180000 free bytes");
        return;
      }
      Serial.printf("@CAM:SNAP,%u\n", sid);
      DBG.println("[CAM] one-shot requested");
      return;
    }
    // 超时：整个事务 45s，串口接图无进展 6s；错误转报错态统一释放。
    if (phase != 5 && uint32_t(now - started) > 45000)
      failed("Camera transaction timeout 45s");
    if (phase == 1 && uint32_t(now - progress) > 6000)
      failed("F407 camera init/VSYNC/response timeout");
    if (phase == 2) {
      if (!uploadConnect()) {
        retry();
        return;
      }
      phase = 3;
      return;
    }
    // 发送：先请求头后图像；每次按 TCP 可写空间推进，短写保留偏移。
    if (phase == 3) {
      if (!tcp.connected()) {
        retry();
        return;
      }
      int room = tcp.availableForWrite();
      if (room > 0) {
        size_t wrote = 0;
        // 请求头全部写完后才读像素；每次只推进实际成功写入的偏移。
        if (headerOff < header.length()) {
          size_t n = header.length() - headerOff;
          if (n > size_t(room))
            n = room;
          wrote = tcp.write((const uint8_t *)header.c_str() + headerOff, n);
          headerOff += wrote;
        } else {
          // 当前缓冲发完再读下一块，未发送的短写尾部不能被新文件内容覆盖。
          if (off == fill && sent < 153600) {
            fill = image.read(chunk, sizeof(chunk));
            off = 0;
            if (!fill) {
              failed("Flash read failed");
              return;
            }
          }
          size_t n = fill - off;
          if (n > size_t(room))
            n = room;
          if (n) {
            wrote = tcp.write(chunk + off, n);
            off += wrote;
            sent += wrote;
          }
          // 正文全部交TCP后关闭文件，转等响应；尚未对F407发送DONE。
          if (sent == 153600) {
            image.close();
            phase = 4;
          }
        }
        if (wrote)
          progress = now;
      }
      if (uint32_t(now - progress) > 12000)
        retry();
      return;
    }
    // 确认：每轮最多收 2048B；HTTP 202、字节数和 CRC 都通过才成功。
    if (phase == 4) {
      unsigned budget = 2048;
      while (budget-- && tcp.available()) {
        response += char(tcp.read());
        progress = now;
        if (response.length() > 4096) {
          retry();
          return;
        }
      }
      int split = response.indexOf("\r\n\r\n");
      if (split >= 0) {
        StaticJsonDocument<1024> doc;
        if (!deserializeJson(doc, response.substring(split + 4))) {
          if (response.startsWith("HTTP/1.0 202") || response.startsWith("HTTP/1.1 202")) {
            if (doc["ok"].as<bool>() && doc["received"].as<uint32_t>() == 153600 &&
                doc["crc32"].as<uint32_t>() == crc32) {
              DBG.println("[CAM] server verified complete picture");
              release(true);
              return;
            }
          }
          failed(doc["error"] | "Server rejected camera upload");
          return;
        }
      }
      if ((!tcp.connected() && !tcp.available()) || uint32_t(now - progress) > 12000)
        retry();
      return;
    }
    // 失败：回报 job_id/ticket/error，然后统一关闭并恢复空闲。
    if (phase == 5) {
      // 错误回报携带原job/ticket并截断原因长度；无论报告成功与否最后都释放事务。
      StaticJsonDocument<512> doc;
      doc["job_id"] = job;
      doc["ticket"] = ticket;
      doc["error"] = error.substring(0, 160);
      String body, out;
      serializeJson(doc, body);
      postJSON("/api/camera/device/report", body, out);
      DBG.println("[CAM] failed: " + error);
      release(false);
    }
  }
};
static EspCamera camera;
static bool cameraBusy() {
  return camera.busy();
}
#endif
