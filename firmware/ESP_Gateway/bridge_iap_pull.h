#ifndef BRIDGE_IAP_PULL_H
#define BRIDGE_IAP_PULL_H

/* =====================================================================
 * 云端下发（OTA transport = device_pull）—— ESP 侧状态机
 *
 * ## 为什么需要它
 * ESP 在 NAT 后、**没有公网 IP** ⇒ 云端**连不进** `ESP:8080/f407/*`，所以
 * `bridge_iap_esp.h` 那套「局域网直连」（transport = `lan_direct`）**上云后
 * 物理上做不到**。云端那条路是**设备自己来拉**：
 *   · "有活吗" 搭在**每秒一次**的 `POST /api/data` 响应里（`iap` 字段，
 *     服务端 `flask_relay/iap_pull.announce()`）⇒ 反应 ≤1 s，不新增轮询、不新增端口；
 *   · 字节用 `GET /api/iap/chunk?seq=N` 逐块取回。
 * 服务端那半在 `flask_relay/iap_pull.py`（有 PC 侧自测 `iap_pull_selftest.py`）。
 *
 * ## ★ 本文件**不重写**帧协议
 * `0x10/0x11/0x12/0x13/0x14/0x15` 与 `@IAP:REQUEST/INFO/JUMP` 全部**逐字复用**
 * `bridge_iap_esp.h` 里**已经验收过**的那套（局域网交付包正在跑）。这里只换掉
 * "字节从哪来"：从 HTTP 拉，而不是从 multipart 的 `u.buf`。
 * ⇒ 02OTA 那侧的 `IAP_OnFrame()` 一行都不用改。
 *
 * ## 状态机（每圈由 `loop()` 调 `iapPullTick()`）
 * | 态 | 做什么 | 阻塞量 |
 * |---|---|---|
 * | 0 IDLE      | 等公告（`/api/data` 的 `iap` 字段） | 0 |
 * | 1 PROBE     | `@IAP:INFO` 验 `base=0x08008000` → `@IAP:REQUEST` → 2 | ≤2.5 s |
 * | 2 REQUESTED | ★ **跨圈**等用户按 K-UP（可能几十秒，**绝不能死等**） | 0 |
 * | 3 XFER      | INFO → META → 等擦除 → 逐块拉 → END → COMMIT → JUMP | ≤~60 s |
 * | 4 FINISH    | ★ **跨圈**：每 2 s 探一次，等 `mcuCrc == 期望 slot CRC` | ≤180 s |
 *
 * ## ★★ 三条"绝不许违反"的界（都是 02OTA 那边的常量）
 * · `IAP_JUMP_TIMEOUT_MS = 15 s`（`iap_app.h:71`）：`@IAP:COMMIT` 之后 **15 s 内
 *   必须发 `@IAP:JUMP`**，02 那边到点**自己复位**。⇒ END/COMMIT/JUMP 必须
 *   **一口气发完**，中间不许插任何长阻塞（照 `mcuUpload` 原样：5 s + 6 s，合计 11 s）。
 * · `AILINK_OTA_STALL_MS = 20 s`（`ailink.c:496`）：F407 侧"**20 s 收不到新数据
 *   就判停**"。⇒ 两块之间的耗时（HTTP + 停等 ACK）必须远小于 20 s。4096 B/块下
 *   正常每块 ~0.1 s；就算 F407 三条重传全打满也只有 4×1.2 = 4.8 s，安全。
 * · `AILINK_OTA_WAIT_MS = 125 s`（`ailink.c:495`）：F407 侧等按键最多 ~120 s
 *   ⇒ 本文件 `IAP_PULL_APPROVAL_MS` 取 125 s 作兜底，并在收到 `@IAP:NO` 时立刻收手。
 *
 * ## ★ 进度**不由设备上报**（省掉 24 条跨网 TCP 连接）
 * 服务端 `iap_pull.read_chunk()` **自己按"设备拉到第几块"推进 `got_blocks`**
 * ⇒ 设备**不必每块再 POST 一次 report**。本文件只在**换阶段**（`waiting_user` /
 * `uploading` / `verifying`）和**最终结果**（`slot_crc32` / `err`）时各发一次，
 * 全程 3~4 条。★ 这不是省小钱：werkzeug 每个响应都 `Connection: close`（无保活），
 * 每块一次 report 就是**多 24 条跨网 TCP 连接**。
 *
 * ## 互斥（照 `合作表 §4.6`）
 * · 与**流式录音**互斥：`iapPullAnnounce()` 拒绝在 `audioBusy()` 时起任务。
 * · 与**局域网烧录**互斥：复用同一个 `mcuUpdating`。
 * · 与**设备自己的 HTTP 轮询**互斥：状态 3/4 期间 `mcuUpdating=true` ⇒
 *   `loop()` 在 `if (mcuUpdating) return;` 处早退，不再打 `/api/data`。
 *   ⚠️ 所以**认领不过期靠的是 `read_chunk()` 每块刷新 `_claimed_mono`**
 *   （服务端 `CLAIM_STALE_SEC=300 s`）；状态 4 最长 180 s < 300 s，仍然安全。
 */
#include <ESP8266WiFi.h>
#include "bridge_iap_http.h"

/* ---- 与服务端 flask_relay/iap_pull.BLOCK_BYTES **必须一致** ----
 * 不一致会怎样：块数算错 ⇒ 拉到服务端的范围外拿到 409 ⇒ 在 `mcuGot != pullTotal`
 * 处以"字节数对不上"失败（**不会静默烧错**）。现在服务端会把这个值随公告发下来，
 * `iapPullAnnounce()` 当场比对 ⇒ 不一致直接拒，报错更准。 */
#define IAP_PULL_BLOCK         4096u
#define IAP_PULL_HTTP_IDLE_MS  6000u   /* no TCP bytes, not time waiting for MCU ACK */
#define IAP_PULL_FETCH_MS     12000u   /* bounded across all retries, below MCU stall */
#define IAP_PULL_RETRIES          3u

#define IAP_PULL_APPROVAL_MS   125000u  /* 等 K-UP 的上限；F407 自己 ~120 s 就放弃 */
#define IAP_PULL_VERIFY_MS     180000u  /* JUMP 后复核 slot_crc 的上限（照 perform_lan_ota） */
#define IAP_PULL_VERIFY_GAP_MS   2000u  /* 复核两次探的间隔（照 perform_lan_ota 的 2 s） */

/* ---- 状态 ---- */
static uint8_t  pullState = 0;          /* 0 IDLE / 1 PROBE / 2 REQUESTED / 3 XFER / 4 FINISH */
static String   pullServerIp = ""; /* task stays on the server that announced it */
static String   pullName  = "";         /* 固件名，只给日志看 */
static uint32_t pullTotal = 0;          /* 整包字节数（原文） */
static uint32_t pullCrc   = 0;          /* 原文 CRC32 → @IAP:REQUEST / META */
static uint32_t pullSlotCrc = 0;        /* ★ 期望的 Slot A CRC（padded）→ PASS 判据 */
static uint32_t pullSeq   = 0;          /* 已拉到的块号 */
static unsigned long pullT0 = 0, pullNextProbe = 0;
static uint8_t  pullSawWait = 0;        /* 见过 @IAP:WAIT 吗（用来区分"取消"和"还没开始"） */
static bool     pullLastFresh = false;  /* 最后一次 @IAP:INFO 有没有人应 */
static String   pullErr = "";

static bool iapPullBusy() { return pullState != 0; }

static String iapPullConnect(WiFiClient &client, const char *tag, uint32_t budget) {
  if (!pullServerIp.length() || !budget) return "";
  if (budget>1500u) budget=1500u;
  client.setTimeout(budget);
  if (client.connect(pullServerIp.c_str(),SERVER_PORT)) return pullServerIp;
  client.stop();
  DBG.printf("[IAP-PULL] task server connect failed (%s)\n",tag);
  return "";
}

/* ---------------------------------------------------------------------
 * 报进度 / 报结果
 *   phase: "" 表示只报结果（不带 phase，由服务端按 slot_crc32 自己判 done/failed）
 *   err  : 非空 ⇒ 服务端把任务判 FAIL
 *   hasSlotCrc=1 时才带 `slot_crc32`。CRC 是无符号 32 位，不能拿负数作哨兵：
 *   最高位为 1 的合法 CRC 若被转成 int32，会被误当成“没有 CRC”。
 * --------------------------------------------------------------------- */
static void iapPullReport(const char *phase, const char *err, bool hasSlotCrc, uint32_t slotCrc) {
  WiFiClient client;
  String ip = iapPullConnect(client, "iap report", 1500u);
  if (ip.length() == 0) { DBG.println("[IAP-PULL] ★ report 发不出去（连不上任何服务器）"); return; }

  String body = String("{\"device_id\":\"") + DEVICE_ID + "\"";
  if (phase && phase[0]) body += String(",\"phase\":\"") + phase + "\"";
  if (err   && err[0])   body += String(",\"err\":\"") + err + "\"";
  if (hasSlotCrc)        body += String(",\"slot_crc32\":") + String(slotCrc);
  body += "}";

  /* report 也是 ESP 的无 session 接口；与 chunk 一样必须带 token。
     少了它时服务端回 401，网页永远停在上一阶段，且 ESP 原先只打印响应
     不会中断传输，现场看起来就像“确认后没有下发”。 */
  client.print(String("POST /api/iap/report?token=") + API_TOKEN + " HTTP/1.1\r\n");
  client.print("Host: " + ip + ":" + String(SERVER_PORT) + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: " + String(body.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  /* 把响应读完再关：不等它读干净就 stop()，服务端那边会看到 broken pipe。
     等待期间照常收 F407（别把 @IAP: 的帧丢了）。 */
  String resp = "";
  unsigned long deadline = millis() + 2000;
  while (client.connected() || client.available()) {
    while (client.available()) {
      int c = client.read();
      if (c < 0) break;
      if (resp.length() < 160) resp += (char)c;
    }
    if (millis() > deadline) break;
    handleF407Receive();
    delay(5);
  }
  client.stop();
  DBG.println(String("[IAP-PULL] report → 云端: ") + body);
  if (resp.length() > 0) DBG.println(String("[IAP-PULL] 云端答: ") + resp);
}

static void iapPullReset() {
  pullState = 0; pullSeq = 0; pullSawWait = 0;
  pullLastFresh = false;
  pullName = ""; pullErr = ""; pullServerIp = "";
  mcuUpdating = false;              /* 放开互斥，让 loop() 恢复正常轮询 */
  Serial.print("@AUDIO:READY\n");
}

/* 失败收尾：CTRL ABORT（没提交过才真发）→ 报给云端 → 回 IDLE。
   ★ 走这条路**绝不会**留下半个镜像：F407 侧 `IAP_C_ABORT` 会把这次擦写作废。
   ⚠️ 形参是 `const String&`（不是 `const char*`）：两个调用点传的
      `mcuMessage` 是 String（bridge_iap_esp.h 里的那个），字面量也能隐式转过来。 */
static void iapPullFail(const String &why) {
  pullErr = why;
  DBG.println(String("[IAP-PULL] ★ 失败: ") + why);
  mcuAbort();
  iapPullReport("failed", pullErr.c_str(), false, 0u);
  iapPullReset();
}

/* ---------------------------------------------------------------------
 * 下载完整的 seq 块后再交给 128 B MCU 写入器；短 TCP 可安全重拉同一块。
 * 返回 0=写入完成，2=失败（原因在 pullErr）；块数来自批准的固件大小。
 * --------------------------------------------------------------------- */
static uint8_t iapPullFetchInto(uint32_t seq) {
  static uint8_t block[IAP_PULL_BLOCK];
  static IapHttpBlock reader;
  const uint32_t offset=seq*IAP_PULL_BLOCK;
  if (offset>=pullTotal || mcuGot!=offset) {
    pullErr="固件块序号与已写入字节不一致"; return 2;
  }
  size_t want=pullTotal-offset;
  if (want>IAP_PULL_BLOCK) want=IAP_PULL_BLOCK;
  const uint32_t started=millis();
  String lastError="connect failed";
  size_t lastReceived=0;
  for (unsigned attempt=0; attempt<IAP_PULL_RETRIES; ++attempt) {
    uint32_t elapsed=uint32_t(millis()-started);
    if (elapsed>=IAP_PULL_FETCH_MS) break;
    WiFiClient client;
    String ip=iapPullConnect(client,"iap chunk",IAP_PULL_FETCH_MS-elapsed);
    if (!ip.length()) { lastError="connect failed"; continue; }
    char path[160];
    snprintf(path,sizeof(path),"/api/iap/chunk?token=%s&device_id=%s&seq=%lu",
             API_TOKEN,DEVICE_ID,(unsigned long)seq);
    elapsed=uint32_t(millis()-started);
    if (elapsed>=IAP_PULL_FETCH_MS) { client.stop(); break; }
    uint32_t writeBudget=IAP_PULL_FETCH_MS-elapsed;
    client.setTimeout(writeBudget>500u ? 500u : writeBudget);
    String request=String("GET ")+path+" HTTP/1.1\r\nHost: "+ip+":"+
                   String(SERVER_PORT)+"\r\nConnection: close\r\n\r\n";
    if (client.print(request)!=request.length()) {
      lastError="HTTP request write incomplete"; client.stop(); continue;
    }
    reader.reset(block,want,millis());
    bool retry=true;
    while (!reader.complete()) {
      if (uint32_t(millis()-started)>=IAP_PULL_FETCH_MS) {
        lastError="network block budget 12s exceeded"; break;
      }
      handleF407Receive();
      if (mcuFailed) { client.stop(); pullErr=mcuMessage; return 2; }
      int available=client.available();
      if (available>0) {
        uint8_t wire[256];
        size_t n=size_t(available);
        if (n>sizeof(wire)) n=sizeof(wire);
        int got=client.read(wire,n);
        if (got>0 && !reader.feed(wire,size_t(got),millis())) {
          lastError=reader.reason();
          if (reader.statusCode() && reader.statusCode()!=200)
            lastError=String("HTTP ")+String(reader.statusCode())+": "+lastError;
          /* Permanent HTTP/length errors must never be programmed/retried.
           * A server 5xx may be transient; 401/409 are real refusals. */
          retry=reader.statusCode()>=500 && reader.statusCode()<600;
          break;
        }
      } else {
        if (!client.connected()) {
          /* ESP8266 core 3.1.2 available() snapshots the RX size BEFORE
           * optimistic_yield(). Final data + FIN can arrive in that yield:
           * the old snapshot is 0 and connected() is false (CLOSE_WAIT),
           * while the final body bytes are already buffered. Drain them
           * before treating a peer close as truncation. Never pad a body. */
          if (client.available()>0) continue;
          lastError="TCP closed before full block"; break;
        }
        if (reader.idleExpired(millis(),IAP_PULL_HTTP_IDLE_MS)) {
          lastError="network idle 6s"; break;
        }
        delay(1);
      }
      delay(0);
    }
    lastReceived=reader.received();
    client.stop();
    if (reader.complete()) {
      /* No network timer runs during 32 UART/Flash ACKs. Partial downloads
       * have not changed mcuGot/mcuFill/mcuSeq, so retries cannot duplicate. */
      if (!mcuFeed(block,want)) { pullErr=mcuMessage; return 2; }
      DBG.printf("[IAP-PULL] block %lu: HTTP %u B OK, MCU total=%lu\n",
                 (unsigned long)seq,(unsigned)want,(unsigned long)mcuGot);
      return 0;
    }
    DBG.printf("[IAP-PULL] block %lu download attempt %u failed (%u/%u B)\n",
               (unsigned long)seq,attempt+1,(unsigned)lastReceived,(unsigned)want);
    if (!retry) break;
    delay(50);
  }
  pullErr=String("固件块 ")+String(seq)+" 下载失败 ("+String((unsigned)lastReceived)+
          "/"+String((unsigned)want)+" B): "+lastError;
  return 2;
}

/* ---------------------------------------------------------------------
 * 状态 1：先探 F407 在不在 OTA 就绪态，再发申请
 * ★ 这条前置检查是照 f407_ota_relay.perform_lan_ota():95-97 抄的 ——
 *   "没确认 F407 就位就弹 K-UP 页"会让用户白按一次。
 * --------------------------------------------------------------------- */
static void iapPullProbeTick() {
  uint32_t serial = mcuStateSerial;
  Serial.print("@IAP:INFO\n");
  if (!mcuWait(0, serial, 2500) || mcuBase != 0x08008000 || mcuCapacity != 229376u || mcuLayout != 0x00020001u) {
    iapPullFail("F407 不在 OTA 就绪态（base != 0x08008000）：先确认 bootstrap 已用 SWD 烧过、且当前空闲");
    return;
  }
  mcuFailed = false; mcuError = 0; mcuApproval = 0;
  mcuRequestTotal = pullTotal; mcuRequestCrc = pullCrc;
  char req[48];
  snprintf(req, sizeof(req), "@IAP:REQUEST,%lu,%lu\n",
           (unsigned long)pullTotal, (unsigned long)pullCrc);
  Serial.print(req);
  DBG.println(String("[IAP-PULL] 已申请: ") + pullName + " (" + String(pullTotal) +
              " B, CRC32 " + String(pullCrc) + ") ⇒ 等用户按板子上的 K-UP");
  iapPullReport("waiting_user", "", false, 0u);
  pullState = 2;
  pullT0 = millis();
  pullSawWait = 0;
}

/* ---------------------------------------------------------------------
 * 状态 2：跨圈等用户按 K-UP
 * ★ 这是本文件与 8080 那条路**最大的一处不同**：8080 那条在 HTTP handler 里
 *   `while(...<1800ms)` 死等（bridge_iap_esp.h:96），而云端这条是**用户按板子**，
 *   可能几十秒 ⇒ **绝不能死等**（会把 /api/data 和流式录音全卡死）。
 *   这里每圈看一眼 mcuApproval 就走，零阻塞。
 *   顺带：这一态里 mcuUpdating 是**假**的，所以设备照常每秒打 /api/data ——
 *   既让服务端 `announce()` 不断刷新认领（不会 300 s 过期），也让网页看得见进度。
 * --------------------------------------------------------------------- */
static void iapPullRequestedTick() {
  if (mcuFailed) { iapPullFail(mcuMessage); return; }
  if (mcuApproval == 2) {
    DBG.println("[IAP-PULL] 用户已按 K-UP ⇒ 开始传输（此后独占 F407，不再打 /api/data）");
    mcuUpdating = true;            /* ★ 从这里开始 loop() 会早退，什么都不许来抢 F407 */
    pullState = 3;
    return;
  }
  if (mcuApproval == 1) {
    pullSawWait = 1;
  } else if (pullSawWait) {
    /* 见过 WAIT 又回到 0 ⇒ 用户在板上取消了，或 F407 到点自己放弃（~120 s） */
    iapPullFail("用户在 F407 上取消了这次 OTA（或 F407 等按键超时）");
    return;
  }
  if (millis() - pullT0 >= IAP_PULL_APPROVAL_MS) {
    iapPullFail("等 K-UP 超时 125 s（F407 那边 ~120 s 就先放弃了）");
  }
}

/* ---------------------------------------------------------------------
 * 状态 3：一口气搬完（**阻塞，但每一段都有界**）
 * ★ 全程不能回头：END/COMMIT/JUMP 三步必须连着发（IAP_JUMP_TIMEOUT_MS 只有 15 s）。
 * --------------------------------------------------------------------- */
static void iapPullTransfer() {
  uint32_t serial = mcuStateSerial;
  Serial.print("@IAP:INFO\n");
  if (!mcuWait(0, serial, 2500) || mcuBase != 0x08008000 || mcuCapacity != 229376u || mcuLayout != 0x00020001u) {
    iapPullFail("按了 K-UP 之后 F407 不再处于就绪态");
    return;
  }
  uint8_t meta[12] = {0};
  /* 云端主动拉取不经过 8080 的 mcuUpload()，所以它必须在这里自己建立
     mcuFeed() 的传输账本。漏掉 mcuTotal 时它保持 0，第一字节就触发
     “Image longer than declared”，表现为按 K-UP 后完全没有固件进度。 */
  mcuFailed = false; mcuCommitted = false;
  mcuTotal = pullTotal; mcuGot = 0;
  mcuFill = 0; mcuSeq = 0;
  iapWrite32(meta, pullTotal);
  iapWrite32(meta + 4, pullCrc);
  meta[8] = 1;
  serial = mcuStateSerial;
  mcuFrame(0x13, 0, meta, sizeof(meta));
  if (!mcuWait(2, serial, 12000)) { iapPullFail("F407 擦除超时（12 s）"); return; }

  uint32_t blocks = (pullTotal + IAP_PULL_BLOCK - 1) / IAP_PULL_BLOCK;
  DBG.printf("[IAP-PULL] F407 擦除完成，开始拉 %lu 块 × %lu B\n",
             (unsigned long)blocks, (unsigned long)IAP_PULL_BLOCK);
  iapPullReport("uploading", "", false, 0u);

  pullSeq = 0;
  while (pullSeq < blocks) {
    uint8_t r = iapPullFetchInto(pullSeq);
    if (r == 2) { iapPullFail(pullErr); return; }
    if (r == 1) break;                       /* 服务端说没了（正常到头的信号） */
    pullSeq++;
  }
  if (mcuGot != pullTotal) {
    iapPullFail(mcuGot < pullTotal ? "服务端给的字节比声明的少" : "收到的比声明的多");
    return;
  }
  /* ⚠️ 上面这个 `mcuGot != pullTotal` 的检查**必须在 mcuFlushTail() 之前** ——
     mcuFlushTail() 自己也查一遍（保留原短路语义），撞上时它返回 false 但
     **不动 mcuMessage**，那下面的报错就成了上一句的旧文。 */
  if (!mcuFlushTail()) { iapPullFail(mcuMessage); return; }

  uint8_t cmd = 3; serial = mcuStateSerial;
  mcuFrame(0x15, 0, &cmd, 1);
  if (!mcuWait(4, serial, 5000)) { iapPullFail("F407 校验（END）失败"); return; }
  cmd = 4; serial = mcuStateSerial;
  mcuFrame(0x15, 0, &cmd, 1);
  if (!mcuWait(5, serial, 6000)) { iapPullFail("F407 提交（COMMIT）失败"); return; }
  mcuCommitted = true;
  Serial.print("@IAP:JUMP\n");
  DBG.println("[IAP-PULL] 已提交并请求 JUMP；等 F407 重启后复核 slot_crc");
  iapPullReport("verifying", "", false, 0u);
  pullState = 4;
  pullT0 = millis();
  pullNextProbe = millis();                  /* 立刻探第一次 */
  pullLastFresh = false;
}

/* 复核通过（或超时后带着观测值去让服务端给结论）⇒ 报 slot_crc32 并收工 */
static void iapPullFinishReport() {
  DBG.printf("[IAP-PULL] ★ 复核: slot_crc=0x%08lX 期望 0x%08lX\n",
             (unsigned long)mcuCrc, (unsigned long)pullSlotCrc);
  iapPullReport("", "", true, mcuCrc);        /* 服务端拿它和镜像的 padded CRC 比 */
  iapPullReset();
}

/* ---------------------------------------------------------------------
 * 状态 4：跨圈复核（照 perform_lan_ota:132-158 的 2 s 间隔 / 180 s 上限）
 * ★ 为什么不能立刻判：JUMP 之后 F407 要**重启到新应用**，这中间 @IAP:INFO
 *   没人应答（或还答的是老状态）。PASS 判据 = `base==0x08008000 && slot_crc==期望`。
 * ★ 为什么不在状态 3 里轮询等完：最长 180 s，会把这 180 s 全占死。
 * --------------------------------------------------------------------- */
static void iapPullVerifyTick() {
  if (millis() - pullT0 >= IAP_PULL_VERIFY_MS) {
    if (pullLastFresh) {
      iapPullFinishReport();    /* 有观测值 ⇒ 报上去，让服务端给出精确的"不一致"结论 */
    } else {
      iapPullFail("重启后 180 s 内 F407 一直没应答 @IAP:INFO");
    }
    return;
  }
  if ((long)(millis() - pullNextProbe) < 0) return;   /* 还没到下一次探的时间 */
  pullNextProbe = millis() + IAP_PULL_VERIFY_GAP_MS;

  uint32_t serial = mcuStateSerial;
  /* F407 重启后的 App 不会主动推 STATE；此前这里只有 mcuWait()，等同于
     “没发问题就等答案”，所以网页永久卡在重启校验。每 2 秒主动发一次 INFO，
     收到新 App 的 Slot A CRC 后才能回报 PASS/FAIL。 */
  Serial.print("@IAP:INFO\n");
  bool fresh = mcuWait(0, serial, 2500);
  pullLastFresh = fresh;
  if (!fresh) return;                        /* 重启窗口里没人应 —— 正常 */
  if (mcuBase != 0x08008000 || mcuCapacity != 229376u || mcuLayout != 0x00020001u) return;         /* 还没进应用 */
  if (mcuCrc != pullSlotCrc) {
    DBG.printf("[IAP-PULL] 复核中: slot_crc=0x%08lX，期望 0x%08lX\n",
               (unsigned long)mcuCrc, (unsigned long)pullSlotCrc);
    return;                                  /* 再等（可能还在校验/搬运） */
  }
  iapPullFinishReport();                     /* ★ 一致 ⇒ PASS */
}

/* ---------------------------------------------------------------------
 * 每圈由 loop() 调一次
 * ★ ★ 放置位置有讲究：必须在 `if (mcuUpdating) return;` **之前** ——
 *   本状态机在传输期间会自己把 mcuUpdating 置真，放到那行之后就再也轮不到它了。
 *   放在 `if (audioBusy()) return;` **之后**，顺便白拿一层"录音时不许 OTA"。
 * --------------------------------------------------------------------- */
static void iapPullTick() {
  switch (pullState) {
    case 1: iapPullProbeTick();     break;
    case 2: iapPullRequestedTick(); break;
    case 3: iapPullTransfer();      break;
    case 4: iapPullVerifyTick();    break;
    default: break;                       /* 0 = IDLE：等公告 */
  }
}

/* ---------------------------------------------------------------------
 * 从 /api/data 的响应里认到 `iap` 公告时调（见 parseAIResponse）
 * ★ 这里**绝不做阻塞的事** —— 它是在**处理 HTTP 响应的过程中**被调的
 *   （sendToServer → parseAIResponse）。真正干活交给 iapPullTick()。
 * --------------------------------------------------------------------- */
static void iapPullAnnounce(JsonVariant iap) {
  if (pullState != 0) return;                  /* 已经有任务在跑（含"正等按键"）*/
  if (mcuUpdating) { DBG.println("[IAP-PULL] 局域网烧录在跑，忽略这次云端任务"); return; }
  if (audioBusy()) {
    DBG.println("[IAP-PULL] 正在录音，忽略这次云端任务（IAP 与流式录音互斥）");
    return;
  }
  uint32_t total = iap["bytes"]     | (uint32_t)0u;
  uint32_t crc   = iap["crc32"]     | (uint32_t)0u;
  uint32_t slot  = iap["slot_crc32"]| (uint32_t)0u;
  uint32_t blk   = iap["block"]     | (uint32_t)0u;
  const char *nm = iap["name"]      | "";
  if (total < 8 || total > 229376u) {
    DBG.printf("[IAP-PULL] 公告长度不合法(%lu)，忽略\n", (unsigned long)total);
    return;
  }
  /* ★ 块大小不一致会以"字节数对不上"失败（不会静默烧错），但那样报错不准。
     服务端把 BLOCK_BYTES 一起发下来了，这里当场比对，报错能一句话说清。 */
  if (blk != IAP_PULL_BLOCK) {
    DBG.printf("[IAP-PULL] ★ 块大小不一致: 云端 %lu，本机 %lu ⇒ 拒收（改 IAP_PULL_BLOCK 或服务端 BLOCK_BYTES）\n",
               (unsigned long)blk, (unsigned long)IAP_PULL_BLOCK);
    return;
  }
  pullServerIp = g_server_ip;
  if (!pullServerIp.length()) return;
  pullTotal = total; pullCrc = crc; pullSlotCrc = slot;
  pullName = String(nm);
  pullSeq = 0; pullSawWait = 0; pullErr = "";
  pullLastFresh = false;
  mcuFailed = false; mcuError = 0; mcuApproval = 0;
  mcuRequestTotal = total; mcuRequestCrc = crc;
  pullState = 1;
  Serial.print("@AUDIO:BUSY\n"); /* reserve the UART while waiting for K-UP */
  DBG.println(String("[IAP-PULL] 云端来了新固件: ") + pullName + " (" +
              String(total) + " B, CRC32 " + String(crc) + ", slot CRC " +
              String(slot) + ")");
}

#endif
