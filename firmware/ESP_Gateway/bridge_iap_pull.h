/*
 * 文件：bridge_iap_pull.h
 *
 * 职责：实现device_pull模式，由ESP主动从公告所属服务器下载F407固件，适用于NAT后的设备。
 * 使用：parseAIResponse取得iap对象后调用iapPullAnnounce，仅登记任务；主循环调用iapPullTick。
 *   tick必须位于audio/camera让出逻辑之后、mcuUpdating早退之前，否则传输态会阻塞自己。
 * 流程：0空闲 → 1核对布局/申请 → 2跨轮等实体按键 → 3拉块/写入/提交 → 4重启CRC复核。
 * 网络：公告来自/api/data，固件来自/api/iap/chunk，阶段与结果发至/api/iap/report。
 *   任务固定pullServerIp，不在同一任务中切换服务器；请求保留原认证与device_id。
 * 缓冲：每HTTP块4096B，先完整验证再调用共用mcuFeed，拆成128B停等写入F407。
 *   HTTP截断重试不改变MCU写入账本，避免同一固件块重复追加；不把TCP关闭当作完整正文。
 * 时序：按键125s兜底；单HTTP块总预算12s/无数据6s/最多3次；块间不得触及F407的20s停滞限。
 *   END/COMMIT/JUMP连续执行，提交后15s内发JUMP；重启复核每2s探测，上限180s。
 * 成功：新应用回复的基址/容量/布局及填充后Slot CRC符合期望，才上报复核结果。
 * 占用：申请即通知AUDIO:BUSY；等待按键期间仍轮询服务，确认后mcuUpdating阻止普通轮询。
 *   busy为主循环协作标志；reset/fail路径统一释放，不能把它当作RTOS锁。
 * 依赖：先包含bridge_iap_esp.h，并提供网络配置、handleF407Receive和服务器路由。
 *   传输阶段部分步骤有界阻塞；代码不提供整个OTA事务的断电自动恢复保证。
 */

#ifndef BRIDGE_IAP_PULL_H
#define BRIDGE_IAP_PULL_H

#include <ESP8266WiFi.h>
#include "bridge_iap_http.h"

// 云端每块 4096B，须与服务端一致；完整下载后再分成 MCU 的 128B 块。
#define IAP_PULL_BLOCK 4096u
#define IAP_PULL_HTTP_IDLE_MS 6000u
#define IAP_PULL_FETCH_MS 12000u
#define IAP_PULL_RETRIES 3u

// 按键申请上限125s；复位后每2s探测一次，校验上限180s。
#define IAP_PULL_APPROVAL_MS 125000u
#define IAP_PULL_VERIFY_MS 180000u
#define IAP_PULL_VERIFY_GAP_MS 2000u

// 0空闲 → 1探测/申请 → 2等按键 → 3传输/提交 → 4重启复核 → 0。
static uint8_t pullState = 0;
static String pullServerIp = "";
static String pullName = "";
// pullTotal/pullCrc对应固件原文，pullSlotCrc对应写入Slot A后的填充镜像，不能混用。
static uint32_t pullTotal = 0;
static uint32_t pullCrc = 0;
// 固件原文 CRC 用于请求；Slot CRC 包含填充区，用于重启后复核。
static uint32_t pullSlotCrc = 0;
static uint32_t pullSeq = 0;
static unsigned long pullT0 = 0, pullNextProbe = 0;
// 记录是否见过WAIT，用来区分尚未确认与已进入确认页后取消/超时返回空闲。
static uint8_t pullSawWait = 0;
// 仅当最后一次主动INFO收到新状态，超时收尾时才有可报告的有效CRC观测。
static bool pullLastFresh = false;
static String pullErr = "";

// 申请中和重启复核也算busy，不只是数据传输阶段，避免ESP自身OTA打断流程。
static bool iapPullBusy() {
  return pullState != 0;
}

// 任务路由固定为pullServerIp，连接超时取剩余预算且封顶1500ms，不切换别的服务器。
static String iapPullConnect(WiFiClient &client, const char *tag, uint32_t budget) {
  if (!pullServerIp.length() || !budget)
    return "";
  if (budget > 1500u)
    budget = 1500u;
  client.setTimeout(budget);
  if (client.connect(pullServerIp.c_str(), SERVER_PORT))
    return pullServerIp;
  client.stop();
  DBG.printf("[IAP-PULL] task server connect failed (%s)\n", tag);
  return "";
}

// 阶段/结果上报带认证；hasSlotCrc 单独表示是否存在，合法 CRC 可为任意 uint32。
static void iapPullReport(const char *phase, const char *err, bool hasSlotCrc, uint32_t slotCrc) {
  WiFiClient client;
  String ip = iapPullConnect(client, "iap report", 1500u);
  if (ip.length() == 0) {
    DBG.println("[IAP-PULL] ★ report 发不出去（连不上任何服务器）");
    return;
  }

  String body = String("{\"device_id\":\"") + DEVICE_ID + "\"";
  if (phase && phase[0])
    body += String(",\"phase\":\"") + phase + "\"";
  if (err && err[0])
    body += String(",\"err\":\"") + err + "\"";
  if (hasSlotCrc)
    body += String(",\"slot_crc32\":") + String(slotCrc);
  body += "}";

  client.print(String("POST /api/iap/report?token=") + API_TOKEN + " HTTP/1.1\r\n");
  client.print("Host: " + ip + ":" + String(SERVER_PORT) + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: " + String(body.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  String resp = "";
  unsigned long deadline = millis() + 2000;
  // 读完应答再关连接；等待期间继续接收 F407，最多2s。
  while (client.connected() || client.available()) {
    while (client.available()) {
      int c = client.read();
      if (c < 0)
        break;
      if (resp.length() < 160)
        resp += (char)c;
    }
    if (millis() > deadline)
      break;
    handleF407Receive();
    delay(5);
  }
  client.stop();
  DBG.println(String("[IAP-PULL] report → 云端: ") + body);
  if (resp.length() > 0)
    DBG.println(String("[IAP-PULL] 云端答: ") + resp);
}

// 释放 OTA 占用并清理任务路由，恢复普通轮询和语音 READY。
static void iapPullReset() {
  pullState = 0;
  pullSeq = 0;
  pullSawWait = 0;
  pullLastFresh = false;
  pullName = "";
  pullErr = "";
  pullServerIp = "";
  mcuUpdating = false;
  Serial.print("@AUDIO:READY\n");
}

// 失败：未提交则 ABORT，向服务端报告失败，最后统一复位本地状态。
static void iapPullFail(const String &why) {
  pullErr = why;
  DBG.println(String("[IAP-PULL] ★ 失败: ") + why);
  mcuAbort();
  iapPullReport("failed", pullErr.c_str(), false, 0u);
  iapPullReset();
}

// 下载一块：预算12s、无数据6s、最多3次；完整验证前不改变 MCU 写入计数。
static uint8_t iapPullFetchInto(uint32_t seq) {
  // 静态4KiB块避免大栈占用；这里只能串行处理一个下载任务，不能并发复用缓冲。
  static uint8_t block[IAP_PULL_BLOCK];
  static IapHttpBlock reader;
  // 偏移：任务块号必须与 MCU 已写入字节相符，防止重复或跳块。
  const uint32_t offset = seq * IAP_PULL_BLOCK;
  if (offset >= pullTotal || mcuGot != offset) {
    pullErr = "固件块序号与已写入字节不一致";
    return 2;
  }
  size_t want = pullTotal - offset;
  if (want > IAP_PULL_BLOCK)
    want = IAP_PULL_BLOCK;
  const uint32_t started = millis();
  String lastError = "connect failed";
  size_t lastReceived = 0;
  // 全部尝试共享started起算的12s预算，不能每次重试各等满12s。
  for (unsigned attempt = 0; attempt < IAP_PULL_RETRIES; ++attempt) {
    uint32_t elapsed = uint32_t(millis() - started);
    if (elapsed >= IAP_PULL_FETCH_MS)
      break;
    WiFiClient client;
    String ip = iapPullConnect(client, "iap chunk", IAP_PULL_FETCH_MS - elapsed);
    if (!ip.length()) {
      lastError = "connect failed";
      continue;
    }
    char path[160];
    snprintf(path, sizeof(path), "/api/iap/chunk?token=%s&device_id=%s&seq=%lu", API_TOKEN,
             DEVICE_ID, (unsigned long)seq);
    elapsed = uint32_t(millis() - started);
    if (elapsed >= IAP_PULL_FETCH_MS) {
      client.stop();
      break;
    }
    uint32_t writeBudget = IAP_PULL_FETCH_MS - elapsed;
    client.setTimeout(writeBudget > 500u ? 500u : writeBudget);
    String request = String("GET ") + path + " HTTP/1.1\r\nHost: " + ip + ":" +
                     String(SERVER_PORT) + "\r\nConnection: close\r\n\r\n";
    // 请求头本身短写也视为本次下载失败；尚未向F407交付该块，可安全重试。
    if (client.print(request) != request.length()) {
      lastError = "HTTP request write incomplete";
      client.stop();
      continue;
    }
    // HTTP：累计状态/长度/正文，短响应可以重拉；4xx和永久格式错误直接失败。
    reader.reset(block, want, millis());
    bool retry = true;
    while (!reader.complete()) {
      if (uint32_t(millis() - started) >= IAP_PULL_FETCH_MS) {
        lastError = "network block budget 12s exceeded";
        break;
      }
      handleF407Receive();
      if (mcuFailed) {
        client.stop();
        pullErr = mcuMessage;
        return 2;
      }
      int available = client.available();
      if (available > 0) {
        uint8_t wire[256];
        size_t n = size_t(available);
        if (n > sizeof(wire))
          n = sizeof(wire);
        int got = client.read(wire, n);
        if (got > 0 && !reader.feed(wire, size_t(got), millis())) {
          lastError = reader.reason();
          // 5xx可能暂时恢复，可重试；4xx和永久长度错误不是靠重拉相同内容能解决的。
          if (reader.statusCode() && reader.statusCode() != 200)
            lastError = String("HTTP ") + String(reader.statusCode()) + ": " + lastError;

          retry = reader.statusCode() >= 500 && reader.statusCode() < 600;
          break;
        }
      } else {
        if (!client.connected()) {

          // 断开时再查接收缓冲，避免 ESP yield 期间到达的尾字节被误判为截断。
          if (client.available() > 0)
            continue;
          lastError = "TCP closed before full block";
          break;
        }
        if (reader.idleExpired(millis(), IAP_PULL_HTTP_IDLE_MS)) {
          lastError = "network idle 6s";
          break;
        }
        delay(1);
      }
      delay(0);
    }
    // 保存实际接收量供诊断；client.stop之后只在reader.complete时才交给mcuFeed。
    lastReceived = reader.received();
    client.stop();
    // 交付：完整块才进入 MCU ACK 阶段；网络读取预算不包含 Flash 停等时间。
    if (reader.complete()) {

      if (!mcuFeed(block, want)) {
        pullErr = mcuMessage;
        return 2;
      }
      DBG.printf("[IAP-PULL] block %lu: HTTP %u B OK, MCU total=%lu\n", (unsigned long)seq,
                 (unsigned)want, (unsigned long)mcuGot);
      return 0;
    }
    DBG.printf("[IAP-PULL] block %lu download attempt %u failed (%u/%u B)\n", (unsigned long)seq,
               attempt + 1, (unsigned)lastReceived, (unsigned)want);
    if (!retry)
      break;
    delay(50);
  }
  pullErr = String("固件块 ") + String(seq) + " 下载失败 (" + String((unsigned)lastReceived) + "/" +
            String((unsigned)want) + " B): " + lastError;
  return 2;
}

// 状态1：先验证 F407 基址、容量和布局，再申请物理确认。
static void iapPullProbeTick() {
  uint32_t serial = mcuStateSerial;
  Serial.print("@IAP:INFO\n");
  if (!mcuWait(0, serial, 2500) || mcuBase != 0x08008000 || mcuCapacity != 229376u ||
      mcuLayout != 0x00020001u) {
    iapPullFail(
        "F407 不在 OTA 就绪态（base != 0x08008000）：先确认 bootstrap 已用 SWD 烧过、且当前空闲");
    return;
  }
  mcuFailed = false;
  mcuError = 0;
  mcuApproval = 0;
  mcuRequestTotal = pullTotal;
  mcuRequestCrc = pullCrc;
  char req[48];
  snprintf(req, sizeof(req), "@IAP:REQUEST,%lu,%lu\n", (unsigned long)pullTotal,
           (unsigned long)pullCrc);
  Serial.print(req);
  DBG.println(String("[IAP-PULL] 已申请: ") + pullName + " (" + String(pullTotal) + " B, CRC32 " +
              String(pullCrc) + ") ⇒ 等用户按板子上的 K-UP");
  iapPullReport("waiting_user", "", false, 0u);
  // 转状态2，跨主循环等按键；此阶段继续普通轮询以维持任务认领。
  pullState = 2;
  pullT0 = millis();
  pullSawWait = 0;
}

// 状态2：每轮检查确认、取消和125s超时，不阻塞等待用户。
static void iapPullRequestedTick() {
  if (mcuFailed) {
    iapPullFail(mcuMessage);
    return;
  }
  // 物理按键确认来自串口文本；不以网页点按钮或下载成功替代板端批准。
  if (mcuApproval == 2) {
    DBG.println("[IAP-PULL] 用户已按 K-UP ⇒ 开始传输（此后独占 F407，不再打 /api/data）");
    // 收到物理确认后取得传输占用，后续普通 HTTP 请求暂停。
    mcuUpdating = true;
    pullState = 3;
    return;
  }
  if (mcuApproval == 1) {
    pullSawWait = 1;
  // 已经见过WAIT却不再等待，说明板端取消或过期，应放弃任务而非再次绕过确认。
  } else if (pullSawWait) {

    iapPullFail("用户在 F407 上取消了这次 OTA（或 F407 等按键超时）");
    return;
  }
  if (millis() - pullT0 >= IAP_PULL_APPROVAL_MS) {
    iapPullFail("等 K-UP 超时 125 s（F407 那边 ~120 s 就先放弃了）");
  }
}

// 状态3：有界阻塞搬运；END/COMMIT/JUMP须连续执行，提交后15s内请求跳转。
// 两块之间不能超过 F407 的20s停滞期限，HTTP重试共用12s预算。
static void iapPullTransfer() {
  uint32_t serial = mcuStateSerial;
  Serial.print("@IAP:INFO\n");
  if (!mcuWait(0, serial, 2500) || mcuBase != 0x08008000 || mcuCapacity != 229376u ||
      mcuLayout != 0x00020001u) {
    iapPullFail("按了 K-UP 之后 F407 不再处于就绪态");
    return;
  }
  // META携带原文总长和CRC及标志；校验/擦写动作仍在F407执行。
  uint8_t meta[12] = {0};

  mcuFailed = false;
  mcuCommitted = false;
  // 初始化共用写入账本；必须在 META 和首个数据块之前完成。
  mcuTotal = pullTotal;
  mcuGot = 0;
  mcuFill = 0;
  mcuSeq = 0;
  iapWrite32(meta, pullTotal);
  iapWrite32(meta + 4, pullCrc);
  meta[8] = 1;
  serial = mcuStateSerial;
  mcuFrame(0x13, 0, meta, sizeof(meta));
  if (!mcuWait(2, serial, 12000)) {
    iapPullFail("F407 擦除超时（12 s）");
    return;
  }

  uint32_t blocks = (pullTotal + IAP_PULL_BLOCK - 1) / IAP_PULL_BLOCK;
  DBG.printf("[IAP-PULL] F407 擦除完成，开始拉 %lu 块 × %lu B\n", (unsigned long)blocks,
             (unsigned long)IAP_PULL_BLOCK);
  iapPullReport("uploading", "", false, 0u);

  pullSeq = 0;
  // 每个HTTP块写入并收到MCU确认后才推进pullSeq；失败从统一fail路径结束。
  while (pullSeq < blocks) {
    uint8_t r = iapPullFetchInto(pullSeq);
    if (r == 2) {
      iapPullFail(pullErr);
      return;
    }
    if (r == 1)
      break;
    pullSeq++;
  }
  // 先检查整包长度，再发尾块、END、COMMIT；字节数不符不允许提交。
  if (mcuGot != pullTotal) {
    iapPullFail(mcuGot < pullTotal ? "服务端给的字节比声明的少" : "收到的比声明的多");
    return;
  }

  if (!mcuFlushTail()) {
    iapPullFail(mcuMessage);
    return;
  }

  // 收尾顺序：END等5s → COMMIT等6s → 立即JUMP，随后才报verifying。
  uint8_t cmd = 3;
  serial = mcuStateSerial;
  mcuFrame(0x15, 0, &cmd, 1);
  if (!mcuWait(4, serial, 5000)) {
    iapPullFail("F407 校验（END）失败");
    return;
  }
  cmd = 4;
  serial = mcuStateSerial;
  mcuFrame(0x15, 0, &cmd, 1);
  if (!mcuWait(5, serial, 6000)) {
    iapPullFail("F407 提交（COMMIT）失败");
    return;
  }
  // 镜像已提交，此后只能等待启动校验结果，失败清理不能再当作尚未提交撤回。
  mcuCommitted = true;
  Serial.print("@IAP:JUMP\n");
  DBG.println("[IAP-PULL] 已提交并请求 JUMP；等 F407 重启后复核 slot_crc");
  iapPullReport("verifying", "", false, 0u);
  // 已提交不等同于运行成功，转状态4获取新应用的完整 Slot CRC。
  pullState = 4;
  pullT0 = millis();
  pullNextProbe = millis();
  pullLastFresh = false;
}

// 上报观测 CRC，由服务端比较期望值；超时后的不匹配也不能冒充成功。
static void iapPullFinishReport() {
  DBG.printf("[IAP-PULL] ★ 复核: slot_crc=0x%08lX 期望 0x%08lX\n", (unsigned long)mcuCrc,
             (unsigned long)pullSlotCrc);
  iapPullReport("", "", true, mcuCrc);
  iapPullReset();
}

// 状态4：跨轮询验证新 STATE、布局与 Slot CRC；重启无应答窗口不立即报错。
static void iapPullVerifyTick() {
  if (millis() - pullT0 >= IAP_PULL_VERIFY_MS) {
    // 180s到期时，有新鲜观测就上报真实CRC让服务器判不匹配；没有应答则报告超时。
    if (pullLastFresh) {
      iapPullFinishReport();
    } else {
      iapPullFail("重启后 180 s 内 F407 一直没应答 @IAP:INFO");
    }
    return;
  }
  if ((long)(millis() - pullNextProbe) < 0)
    return;
  pullNextProbe = millis() + IAP_PULL_VERIFY_GAP_MS;

  uint32_t serial = mcuStateSerial;

  // 每次主动发 INFO 再等待新应答，不依赖 F407 重启时主动推送。
  Serial.print("@IAP:INFO\n");
  bool fresh = mcuWait(0, serial, 2500);
  pullLastFresh = fresh;
  if (!fresh)
    return;
  if (mcuBase != 0x08008000 || mcuCapacity != 229376u || mcuLayout != 0x00020001u)
    return;
  // CRC不相等先继续等到截止时间；不能用提交前缓存的CRC提前宣告升级成功。
  if (mcuCrc != pullSlotCrc) {
    DBG.printf("[IAP-PULL] 复核中: slot_crc=0x%08lX，期望 0x%08lX\n", (unsigned long)mcuCrc,
               (unsigned long)pullSlotCrc);
    return;
  }
  iapPullFinishReport();
}

// 必须在主循环 mcuUpdating 早退之前调用，确保自身可继续推进。
static void iapPullTick() {
  switch (pullState) {
  case 1:
    iapPullProbeTick();
    break;
  case 2:
    iapPullRequestedTick();
    break;
  case 3:
    iapPullTransfer();
    break;
  case 4:
    iapPullVerifyTick();
    break;
  default:
    break;
  }
}

// 公告处理只校验和登记，不在 HTTP 响应解析期间下载或擦写。
static void iapPullAnnounce(JsonVariant iap) {
  if (pullState != 0)
    return;
  if (mcuUpdating) {
    DBG.println("[IAP-PULL] 局域网烧录在跑，忽略这次云端任务");
    return;
  }
  if (audioBusy()) {
    DBG.println("[IAP-PULL] 正在录音，忽略这次云端任务（IAP 与流式录音互斥）");
    return;
  }
  // 公告先校验长度、块大小和可用路由，全部通过后才进入探测申请态。
  uint32_t total = iap["bytes"] | (uint32_t)0u;
  uint32_t crc = iap["crc32"] | (uint32_t)0u;
  uint32_t slot = iap["slot_crc32"] | (uint32_t)0u;
  uint32_t blk = iap["block"] | (uint32_t)0u;
  const char *nm = iap["name"] | "";
  if (total < 8 || total > 229376u) {
    DBG.printf("[IAP-PULL] 公告长度不合法(%lu)，忽略\n", (unsigned long)total);
    return;
  }

  if (blk != IAP_PULL_BLOCK) {
    DBG.printf(
        "[IAP-PULL] ★ 块大小不一致: 云端 %lu，本机 %lu ⇒ 拒收（改 IAP_PULL_BLOCK 或服务端 BLOCK_BYTES）\n",
        (unsigned long)blk, (unsigned long)IAP_PULL_BLOCK);
    return;
  }
  // 固定任务所属服务器，避免中途切换路由后读到另一台服务器的任务。
  pullServerIp = g_server_ip;
  if (!pullServerIp.length())
    return;
  pullTotal = total;
  pullCrc = crc;
  pullSlotCrc = slot;
  pullName = String(nm);
  pullSeq = 0;
  pullSawWait = 0;
  pullErr = "";
  pullLastFresh = false;
  mcuFailed = false;
  mcuError = 0;
  mcuApproval = 0;
  mcuRequestTotal = total;
  mcuRequestCrc = crc;
  // 至此只认领任务，尚无META/擦写；下一次tick才执行布局探测与按键申请。
  pullState = 1;
  // 申请期间通知 F407 暂缓自动录音；失败或复核结束后由 reset 释放。
  Serial.print("@AUDIO:BUSY\n");
  DBG.println(String("[IAP-PULL] 云端来了新固件: ") + pullName + " (" + String(total) +
              " B, CRC32 " + String(crc) + ", slot CRC " + String(slot) + ")");
}

#endif
