/*
 * 文件：bridge_iap_esp.h
 *
 * 职责：实现端口8080的F407局域网OTA入口，以及两种OTA路径共用的串口发送/确认工具。
 * 使用：setup调用mcuUpdateBegin()；空闲主循环调用mcuUpdateServer.handleClient()处理HTTP。
 *   UART分发将0x10~0x1F帧交给mcuOnFrame，文本确认由主文件更新mcuApproval。
 * 接口：GET /f407/status查询；POST /f407/request申请；POST /f407/update上传multipart固件。
 *   请求须携带Authorization: Bearer <token>，固件总长和CRC须与实体按键批准的申请一致。
 * 写入流程：物理确认 → INFO核对布局 → META擦Slot B → DATA停等 → END校验 → COMMIT → JUMP。
 *   MCU每块128B，ACK最多尝试4次/每次1200ms；未收到对应ACK不能推进发送序号。
 * 共享：mcuFeed/mcuFlushTail同样供device_pull调用，不能为两条路径各维护一套写入计数。
 * 状态：mcuUpdating为协作占用，mcuCommitted区分已提交镜像；mcuApproval为无申请/等待/已确认。
 *   STATE serial计数用于排除旧应答；got是当前传输计数，不应独立当作最终运行成功。
 * 依赖：主文件提前提供audioBusy/cameraBusy、DBG、API_TOKEN、Serial与handleF407Receive。
 * 边界：只接受A=0x08008000、224KiB、layout=0x00020001的扩容布局；不负责刷Bootloader。
 *   COMMIT后不会再发ABORT回滚；云端拉取的复位CRC复核见bridge_iap_pull.h。
 */

#ifndef BRIDGE_IAP_ESP_H
#define BRIDGE_IAP_ESP_H
#include <ESP8266WebServer.h>

void handleF407Receive();
static ESP8266WebServer mcuUpdateServer(8080);
// 局域网直传与云端拉取共用 MCU 发送状态，主循环据此暂缓普通请求。
static bool mcuUpdating = false, mcuFailed = false, mcuCommitted = false;
// mcuState来自最新STATE；mcuChunk为当前待确认128B块，不能同时服务两路OTA。
static uint8_t mcuState = 255, mcuError = 0, mcuChunk[128];
// mcuAck为最近ACK序号；65535作等待哨兵，mcuSeq仅在对应ACK收到后增加。
static uint16_t mcuAck = 65535, mcuSeq = 0, mcuFill = 0;
static uint32_t mcuStateSerial = 0, mcuGot = 0, mcuTotal = 0, mcuBase = 0, mcuCrc = 0;
static uint32_t mcuCapacity = 98304u, mcuLayout = 0u;
static uint32_t mcuRequestTotal = 0, mcuRequestCrc = 0;

// 物理确认：0无申请、1等待板端按键、2已确认；确认前不擦写镜像。
static uint8_t mcuApproval = 0;
static String mcuMessage = "No update requested";
// 按小端读写32位字段，避免依赖ESP/F407的结构体对齐或直接指针强转。
static uint32_t iapRead32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static void iapWrite32(uint8_t *p, uint32_t x) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = uint8_t(x >> (8 * i));
}
// 封帧：A5 5A + 类型 + 序号LE16 + 长度LE16 + 数据 + XOR。
static void mcuFrame(uint8_t type, uint16_t seq, const uint8_t *p, uint16_t n) {
  if (type == 0x13)
    DBG.println("[IAP] TX META");
  else if (type == 0x15 && n && p[0] == 3)
    DBG.println("[IAP] TX CTRL END");
  else if (type == 0x15 && n && p[0] == 4)
    DBG.println("[IAP] TX CTRL COMMIT");
  else if (type == 0x15 && n && p[0] == 5)
    DBG.println("[IAP] TX CTRL ABORT");
  uint8_t hdr[] = {0xA5, 0x5A, type, uint8_t(seq), uint8_t(seq >> 8), uint8_t(n), uint8_t(n >> 8)};
  uint8_t sum = 0;
  for (unsigned i = 2; i < 7; i++)
    sum ^= hdr[i];
  for (unsigned i = 0; i < n; i++)
    sum ^= p[i];
  Serial.write(hdr, sizeof(hdr));
  if (n)
    Serial.write(p, n);
  Serial.write(sum);
}
// 更新 ACK/错误/STATE；STATE 序列计数用来区分本次应答和旧状态。
static void mcuOnFrame(uint8_t type, uint16_t seq, const uint8_t *p, uint16_t n) {
  if (type == 0x11 && !n)
    mcuAck = seq;
  if (type == 0x12 && n) {
    mcuError = p[0];
    mcuFailed = true;
  }
  // 兼容旧17B状态用于发现设备；新版25B额外给出容量和布局，擦写前仍须严格核对。
  if (type == 0x14 && (n == 17 || n == 25)) {
    mcuState = p[0];
    mcuGot = iapRead32(p + 1);
    mcuBase = iapRead32(p + 9);
    mcuCrc = iapRead32(p + 13);
    mcuCapacity = n == 25 ? iapRead32(p + 17) : 98304u;
    mcuLayout = n == 25 ? iapRead32(p + 21) : 0u;
    mcuStateSerial++;
  }
}
// 等待期间持续接收串口；必须同时收到新 STATE 和目标状态才返回成功。
static bool mcuWait(uint8_t state, uint32_t serial, uint32_t timeout) {
  uint32_t start = millis();
  while (uint32_t(millis() - start) < timeout) {
    handleF407Receive();
    if (mcuFailed)
      return false;
    if (mcuStateSerial != serial && mcuState == state)
      return true;
    delay(1);
  }
  mcuMessage = "F407 response timeout";
  mcuFailed = true;
  return false;
}
// 每个 128B 块停等 ACK，最多四次、每次 1200ms；成功后推进序号。
static bool mcuSendChunk() {
  mcuAck = 65535;
  for (unsigned attempt = 0; attempt < 4; attempt++) {
    mcuFrame(0x10, mcuSeq, mcuChunk, mcuFill);
    uint32_t start = millis();
    while (uint32_t(millis() - start) < 1200) {
      handleF407Receive();
      if (mcuFailed)
        return false;
      // 此时才能清攒块长度并递增序号；超时重发必须保留同一份块数据与序号。
      if (mcuAck == mcuSeq) {
        mcuSeq++;
        mcuFill = 0;
        return true;
      }
      delay(1);
    }
  }
  mcuMessage = "F407 block ACK timeout";
  mcuFailed = true;
  return false;
}

// 两种 OTA 入口共用攒块器；先核对总长度，满 128B 再发送。
static bool mcuFeed(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (mcuGot >= mcuTotal) {
      mcuFailed = true;
      mcuMessage = "Image longer than declared";
      return false;
    }
    mcuChunk[mcuFill++] = p[i];
    mcuGot++;
    if (mcuFill == 128 && !mcuSendChunk())
      return false;
  }
  return true;
}

// 总长度必须完全匹配才发尾块；失败时保留原错误信息。
static bool mcuFlushTail() {
  if (mcuGot != mcuTotal)
    return false;
  if (mcuFill && !mcuSendChunk())
    return false;
  return true;
}
// 所有HTTP操作沿用同一Bearer口令；这不替代实体按键的OTA写入授权。
static bool mcuAuthorized() {
  return mcuUpdateServer.header("Authorization") == String("Bearer ") + API_TOKEN;
}
// 尚未提交时发送 ABORT，再释放申请和占用；已提交镜像不回滚。
static void mcuAbort() {
  uint8_t cmd = 5;
  if (mcuUpdating && !mcuCommitted)
    mcuFrame(0x15, 0, &cmd, 1);
  mcuUpdating = false;
  mcuApproval = 0;
  mcuRequestTotal = 0;
  mcuRequestCrc = 0;
}
// 严格解析无符号十进制并检查乘10溢出，拒绝空值、负数、尾随字符和超范围输入。
static bool mcuNumber(const String &s, uint32_t &out) {
  if (!s.length() || s.length() > 10)
    return false;
  out = 0;
  for (unsigned i = 0; i < s.length(); i++) {
    if (s[i] < '0' || s[i] > '9' || out > (0xFFFFFFFFu - uint32_t(s[i] - '0')) / 10)
      return false;
    out = out * 10 + s[i] - '0';
  }
  return true;
}
// 只给HTTP响应提供文字状态，内部判断仍使用0/1/2以保持协议兼容。
static const char *mcuApprovalName() {
  return mcuApproval == 1 ? "waiting_user" : (mcuApproval == 2 ? "approved" : "none");
}
// 申请：认证、忙状态、长度与 CRC 校验通过后，请 F407 等待实体按键。
static void mcuRequestUpdate() {
  uint32_t total, crc;
  if (!mcuAuthorized()) {
    mcuUpdateServer.send(401, "text/plain", "Unauthorized");
    return;
  }
  if ((cameraBusy() || audioBusy()) || !mcuNumber(mcuUpdateServer.arg("total"), total) ||
      !mcuNumber(mcuUpdateServer.arg("crc"), crc) || total < 8 || total > 229376u) {
    mcuUpdateServer.send(409, "application/json",
                         "{\"ok\":false,\"err\":\"recording busy or invalid image\"}");
    return;
  }
  mcuFailed = false;
  mcuError = 0;
  mcuApproval = 0;
  // 保存申请的长度和CRC，后续上传必须逐一匹配，不能复用一次按键批准不同固件。
  mcuRequestTotal = total;
  mcuRequestCrc = crc;
  char request[48];
  snprintf(request, sizeof(request), "@IAP:REQUEST,%lu,%lu\n", (unsigned long)total,
           (unsigned long)crc);
  Serial.print(request);
  uint32_t start = millis();
  // 这里只短等F407进入确认页，绝不在HTTP请求中等用户按键120秒。
  while (uint32_t(millis() - start) < 1800 && !mcuFailed && mcuApproval == 0) {
    handleF407Receive();
    delay(1);
  }
  if (mcuApproval != 1) {
    mcuMessage = "F407 did not enter user-confirmation state";
    mcuUpdateServer.send(409, "application/json",
                         "{\"ok\":false,\"err\":\"F407 did not accept request\"}");
    return;
  }
  mcuMessage = "Waiting for F407 K-UP confirmation";
  mcuUpdateServer.send(202, "application/json", "{\"ok\":true,\"approval\":\"waiting_user\"}");
}
// 直传流程：确认匹配 → 探测布局 → META擦除 → DATA → END校验 → COMMIT → JUMP。
static void mcuUpload() {
  HTTPUpload &u = mcuUpdateServer.upload();
  // multipart首次回调先拒绝并发/重复START，再重新建立当前传输计数。
  if (u.status == UPLOAD_FILE_START) {

    // HTTP 重复 START 不重置序号和计数，不重复发送 META 擦除下载区。
    if (mcuUpdating) {
      DBG.println("[IAP] duplicate upload START ignored");
      return;
    }
    mcuFailed = false;
    mcuCommitted = false;
    mcuError = 0;
    mcuFill = 0;
    mcuSeq = 0;
    mcuMessage = "Upload started";
    uint32_t expectedCrc;
    // 写入前复核口令、忙状态、物理确认以及已批准的长度/CRC。
    if (!mcuAuthorized() || (cameraBusy() || audioBusy()) || mcuApproval != 2 ||
        !mcuNumber(mcuUpdateServer.arg("total"), mcuTotal) ||
        !mcuNumber(mcuUpdateServer.arg("crc"), expectedCrc) || mcuTotal != mcuRequestTotal ||
        expectedCrc != mcuRequestCrc || mcuTotal < 8 || mcuTotal > 229376u) {
      mcuFailed = true;
      mcuMessage = "Unauthorized, recording busy, or invalid image size/CRC";
      return;
    }
    // 认证、物理确认及申请匹配后才取得写入占用；此时仍未发送擦除META。
    mcuUpdating = true;
    mcuGot = 0;

    // 先确认基址、容量与布局；旧布局在发 META 前拒绝。
    uint32_t serial = mcuStateSerial;
    Serial.print("@IAP:INFO\n");
    if (!mcuWait(0, serial, 2500) || mcuBase != 0x08008000 || mcuCapacity != 229376u ||
        mcuLayout != 0x00020001u) {
      mcuFailed = true;
      mcuMessage = "Install the expanded bootstrap HEX via ROM UART first; F407 must be idle";
      return;
    }
    uint8_t meta[12] = {0};
    iapWrite32(meta, mcuTotal);
    iapWrite32(meta + 4, expectedCrc);
    meta[8] = 1;
    serial = mcuStateSerial;
    mcuFrame(0x13, 0, meta, sizeof(meta));
    // 等F407报告DATA就绪态，表示Slot B擦除完成；不可在Flash擦除期间直接发数据。
    if (!mcuWait(2, serial, 12000))
      return;
    mcuMessage = "Transferring to F407";
  // 上传片段长度不保证是128的倍数，统一交mcuFeed累积并等待ACK。
  } else if (u.status == UPLOAD_FILE_WRITE && mcuUpdating && !mcuFailed) {
    mcuFeed(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    if (mcuUpdating && !mcuFailed) {
      // HTTP正文结束仍要核对声明总长；不足时不发尾块，更不发提交。
      if (!mcuFlushTail())
        mcuFailed = true;
      if (!mcuFailed) {
        // 连续执行 END/COMMIT/JUMP，避免超过 F407 提交后的自动复位期限。
        uint8_t cmd = 3;
        uint32_t serial = mcuStateSerial;
        mcuFrame(0x15, 0, &cmd, 1);
        if (mcuWait(4, serial, 5000)) {
          cmd = 4;
          serial = mcuStateSerial;
          mcuFrame(0x15, 0, &cmd, 1);
          if (mcuWait(5, serial, 6000)) {
            // COMMIT已经成功，后面的清理只释放网关状态，不再撤销已提交镜像。
            mcuCommitted = true;
            mcuMessage = "Image verified and committed; reboot requested";
            Serial.print("@IAP:JUMP\n");
          }
        }
      }
    }
    mcuAbort();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    mcuFailed = true;
    mcuMessage = "HTTP upload interrupted";
    mcuAbort();
  }
}
// 端口 8080：状态查询、申请和 multipart 上传均要求 Bearer 认证。
static void mcuUpdateBegin() {
  mcuUpdateServer.collectHeaders("Authorization");
  mcuUpdateServer.on("/f407/status", HTTP_GET, []() {
    if (!mcuAuthorized()) {
      mcuUpdateServer.send(401, "text/plain", "Unauthorized");
      return;
    }
    uint32_t serial = mcuStateSerial;
    mcuFailed = false;
    mcuError = 0;
    Serial.print("@IAP:INFO\n");
    bool fresh = mcuWait(0, serial, 1800);
    String body = "{\"online\":" + String(fresh ? "true" : "false") +
                  ",\"base\":" + String(mcuBase) + ",\"slot_crc\":" + String(mcuCrc) +
                  ",\"capacity\":" + String(mcuCapacity) + ",\"layout_id\":" + String(mcuLayout) +
                  ",\"state\":" + String(mcuState) + ",\"approval\":\"" +
                  String(mcuApprovalName()) + "\"}";
    mcuUpdateServer.send(200, "application/json", body);
  });
  mcuUpdateServer.on("/f407/request", HTTP_POST, mcuRequestUpdate);
  mcuUpdateServer.on(
      "/f407/update", HTTP_POST,
      []() {
        if (!mcuAuthorized()) {
          mcuUpdateServer.send(401, "text/plain", "Unauthorized");
          return;
        }
        String body = "{\"ok\":" + String(mcuCommitted ? "true" : "false") + ",\"message\":\"" +
                      mcuMessage + "\",\"error\":" + String(mcuError) +
                      ",\"received\":" + String(mcuGot) + "}";
        mcuUpdateServer.send(mcuCommitted ? 200 : 409, "application/json", body);
      // 处理正文的回调与生成最终HTTP结果的回调分离，最终ok以提交结果为准。
      },
      mcuUpload);
  mcuUpdateServer.begin();
  DBG.println("[IAP] authenticated F407 updater on port 8080");
}
#endif
