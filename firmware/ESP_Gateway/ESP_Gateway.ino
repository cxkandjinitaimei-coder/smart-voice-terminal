/*
 * 文件：ESP_Gateway.ino
 *
 * 职责：ESP8266 网关主程序，连接 F407 与 HTTP 中转服务。
 *   接收传感器、姿态、音频及图像；转发网页/AI命令，协调 ESP 自身和 F407 的 OTA。
 * 使用：Arduino IDE 打开本文件，选择 NodeMCU 1.0；先按配置示例创建 gateway_config.h。
 *   setup() 由 Arduino 启动框架调用一次，loop() 随后反复执行，不由其他模块主动调用。
 * 接线：UART0 GPIO1(TX)→F407 PC7(RX)，GPIO3(RX)←PC6(TX)，921600/8N1，3.3V共地。
 *   Serial1 GPIO2(D4)仅输出115200调试日志，UART0不可混入普通日志。
 * 阅读顺序：全局对象 → setup/loop → ESP OTA → Wi-Fi/发现 → 上报 → 命令转换 → 串口分发。
 * 调度：先收 UART，再推进音频、相机与 IAP，空闲时轮询服务；busy 是协作占用标志。
 *   网络连接及部分 IAP 操作仍可能有界阻塞，不代表所有路径均为非阻塞。
 * 依赖：ESP8266 Core、ArduinoJson、LittleFS及本目录实现头；包含顺序与前置声明必须保留。
 *   bridge_*_esp.h/bridge_iap_pull.h使用此处的全局配置和函数，只在该翻译单元包含。
 * 边界：录音触发、VAD、舵机执行和相机采集在 F407；识别与 AI 决策在服务端。
 *   外层工程目录名、未来产品名和AI名字不参与协议或模块路径。
 */

#include <ESP8266WiFi.h>
#include "firmware_version.h"
#include "bridge_motion.h"
#include "bridge_pet.h"
#include "bridge_motion_cache.h"
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESP8266httpUpdate.h>
#include <LittleFS.h>

#include "gateway_settings.h"

// F407 数据及待上报确认；接收函数只更新状态，由主循环发起 HTTP。
String sensorData = "";
String userMsg = "";
String pendingAck = "";
unsigned long lastSend = 0;
unsigned long lastOtaCheck = 0;
bool ackReadyNow = false;

// 当前可达服务地址：发现/连接成功后更新；失效后清空以触发重新发现。
// 音频和相机复用此路由，单个IAP任务另固定公告所属服务器。
String g_server_ip = "";
unsigned long lastDiscover = 0;

// 实现头共享本文件中的对象，保留声明和包含顺序。
#include "bridge_audio_esp.h"
static bool cameraBusy();
#include "bridge_iap_esp.h"
#include "bridge_camera_esp.h"

// 设备拉取 OTA 使用同一服务器选择入口；须先声明再包含实现。
String connectAnyServer(WiFiClient &client, const char *tag);
#include "bridge_iap_pull.h"

void handleF407Receive();
void handleF407Text(const String &line);
static SavedMotionProfile<fs::FS> motionProfileCache;
// 配置同步账本：Ack来自板端；SentRevision/SentAt用于限速与重发。
// 服务器修订变化时应清旧确认，不能只因HTTP发送成功就认定已同步。
static uint32_t motionAck = 0u, motionSentRevision = 0u, motionSentAt = 0u;
static bool motionReplayRequested = true;
void replaySavedMotionProfile();

void forwardMovesToF407(JsonVariant mvJson);
void forwardCommandToF407(JsonVariant cmd);
bool discoverServer();

// 可选接线自检，只发送 PING，不改变舵机位置。
void uartSelfTestToF407() {
  const char *frames = "@PING\n";
  Serial.print(frames);
  Serial.flush();
  DBG.println("[SELFTEST] UART ping only; no startup servo movement");
  DBG.print(frames);
}

// 空闲时重发已校验的舵机配置，等待板端确认；不写死回正角度。
// 调用：启动挂载后及空闲主循环；UART忙时本轮跳过，下轮再试。
// 发送0x22完整快照；收到@LIMITS:OK修订号后停止无谓重复。
void replaySavedMotionProfile() {
  const uint32_t revision = motionProfileCache.revision();
  if (!revision || audioBusy() || iapPullBusy() || mcuUpdating)
    return;
  if (!motionReplayRequested && (motionAck == revision || millis() - motionSentAt < 2000u))
    return;
  if (motionSendHex(Serial, motionProfileCache.wire(), 0x22, 0u)) {
    if (motionSentRevision != revision)
      motionAck = 0u;
    motionSentRevision = revision;
    motionSentAt = millis();
    motionReplayRequested = false;
  }
}

// 板间串口接线：UART0 GPIO1=TX/GPIO3=RX → F407 PC7=RX/PC6=TX，波特率921600。
// Serial1 GPIO2（D4）仅发送调试日志，115200 baud。
void setup() {
  // 先扩接收缓冲，再开启板间串口；日志不得写入 UART0。
  Serial.setRxBufferSize(4096);
  Serial.begin(F407_BAUD);
  Serial.setDebugOutput(false);

  DBG.begin(115200);
  DBG.println();
  DBG.printf("[ESP8266] FW=%s build=%s (F407 UART 921600)\n", FW_VERSION, ESP_BUILD_ID);
  DBG.println("[TIP] 服务器 IP 自动发现：跑着 app.py 的电脑会自动应答，换网不用重烧");
  // 挂载音频缓存与舵机配置文件；挂载失败由传输层拒绝新录音。
  audioTransport.begin();
  motionProfileCache.begin(LittleFS);
  replaySavedMotionProfile();
  DBG.printf("[HOME] saved profile=%lu, cache=%s; no fixed 90-degree commands\n",
             (unsigned long)motionProfileCache.revision(),
             motionProfileCache.cached() ? "ready" : "not ready");

#if UART_SELFTEST
  uartSelfTestToF407();
#endif

  // 先联网再尝试UDP发现；暂时没有服务应答时，主循环会按重试间隔再找。
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    discoverServer();
    lastDiscover = millis();
  }

  // ESP 自身的 ArduinoOTA，与 F407 固件转发是两条更新路径。
  ArduinoOTA.setHostname(OTA_DEVICE_NAME);
  ArduinoOTA.setPassword(API_TOKEN);
  ArduinoOTA.onStart([]() { DBG.println("[OTA] 开始无线更新..."); });
  ArduinoOTA.onEnd([]() { DBG.println("\n[OTA] 更新完成"); });
  ArduinoOTA.onError([](ota_error_t err) { DBG.printf("[OTA] 错误 %d\n", err); });
  ArduinoOTA.begin();
  mcuUpdateBegin();
  DBG.println("[OTA] 无线烧录已就绪 (局域网)");

  // 让首次loop立即上报，避免上电后再等待一个完整轮询周期。
  lastSend = millis() - SEND_INTERVAL;
}

// 协作式调度：先接收串口，再推进音频、拍照、OTA及普通轮询。
// 忙标志约束任务顺序，不是 RTOS 互斥锁；网络调用仍有有界阻塞。
void loop() {
  handleF407Receive();
  audioBridge.tick(millis());
  audioTransport.poll();
  if (audioTransport.failed())
    audioBridge.fail(audioTransport.failureReason());

  // 音频仍在录制或上传时，暂缓拍照、普通 HTTP 轮询与 OTA。
  if (audioBusy()) {
    delay(0);
    return;
  }
  // 仅在 OTA 空闲且没有等待物理确认时接收新拍照任务。
  camera.tick(!iapPullBusy() && !mcuUpdating && mcuApproval == 0);
  // 拍照事务从串口接图到服务器确认期间保持占用。
  if (cameraBusy()) {
    delay(0);
    return;
  }
  replaySavedMotionProfile();

  // 定期重发空闲通知，允许 F407 在任一端重启后恢复语音触发。
  static unsigned long lastAudioReady = 0;
  if (!iapPullBusy() && !mcuUpdating && mcuApproval == 0 && millis() - lastAudioReady >= 3000) {
    lastAudioReady = millis();
    Serial.print("@AUDIO:READY\n");
  }
  mcuUpdateServer.handleClient();

  // 须先推进 OTA 状态机，再检查 mcuUpdating；反过来会让任务停住。
  iapPullTick();
  if (mcuUpdating) {
    delay(0);
    return;
  }

  static unsigned long lastLinkPing = 0;
  // 板间PING用于探测F407链路，不代表服务器在线，也不等同于传感器有数据。
  if (millis() - lastLinkPing >= 2000) {
    lastLinkPing = millis();
    Serial.print("@PING\n");
  }

  // 掉线重连只在当前没有音频/相机/写入占用时执行，避免主动挤占收数据时段。
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (WiFi.status() == WL_CONNECTED && g_server_ip.length() == 0 &&
      millis() - lastDiscover >= DISCOVER_RETRY_MS) {
    lastDiscover = millis();
    discoverServer();
  }

  // ArduinoOTA需要主循环持续服务；高优先级传输占用期间会被推迟。
  ArduinoOTA.handle();

  handleF407Receive();
  if (audioBusy())
    return;

  // 确认或告警立即上报，其余按 SEND_INTERVAL 轮询；不从接收函数递归上报。
  if (ackReadyNow || millis() - lastSend >= SEND_INTERVAL) {
    ackReadyNow = false;
    lastSend = millis();
    sendToServer();
  }

  // 等待 F407 按键确认时也不更新 ESP 自身，避免中断申请。
  if (!audioBusy() && !iapPullBusy())
    checkForUpdate();
}

// 检查 ESP 自身固件；仅接受严格更高的三段数字版本。
void checkForUpdate() {
  if (millis() - lastOtaCheck < OTA_CHECK_INTERVAL)
    return;
  lastOtaCheck = millis();
  if (WiFi.status() != WL_CONNECTED)
    return;

  WiFiClient client;
  String otaIP = connectAnyServer(client, "ota-info");
  if (otaIP.length() == 0)
    return;

  // ESP自身更新先查询版本/URL，查询时仍服务板间UART。
  String path = String("/api/ota/info?token=") + API_TOKEN;
  client.print(String("GET ") + path + " HTTP/1.1\r\n");
  client.print("Host: " + otaIP + ":" + String(SERVER_PORT) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  String response = "";
  unsigned long hardTimeout = millis() + 8000;
  // 等待首字节与接收正文共用总截止时间；delay让Wi-Fi协议栈有机会运行。
  while (client.available() == 0) {
    if (millis() > hardTimeout) {
      client.stop();
      return;
    }
    handleF407Receive();
    if (audioBusy()) {
      client.stop();
      return;
    }
    delay(10);
  }
  while (client.available() || client.connected()) {
    while (client.available()) {
      response += (char)client.read();
      if (response.length() > 2048)
        break;
    }
    if (millis() > hardTimeout)
      break;
    handleF407Receive();
    if (audioBusy()) {
      client.stop();
      return;
    }
    delay(5);
  }
  client.stop();

  // 先拆HTTP头与正文，再解析JSON；没有正文或JSON错误时不生成板端动作。
  int headerEnd = response.indexOf("\r\n\r\n");
  if (headerEnd < 0)
    return;
  String respBody = response.substring(headerEnd + 4);
  respBody.trim();

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, respBody);
  if (err)
    return;

  String latest = doc["version"] | "";
  String url = doc["url"] | "";
  DBG.printf("[OTA] 当前=%s 云端=%s\n", FW_VERSION, latest.c_str());
  unsigned la = 0, lb = 0, lc = 0, ca = 0, cb = 0, cc = 0;
  char extra;
  // 比较语义版本；公开版 1.0.0 不会被原 1.1.13 自动视为升级。
  bool newer = sscanf(latest.c_str(), "%u.%u.%u%c", &la, &lb, &lc, &extra) == 3 &&
               sscanf(FW_VERSION, "%u.%u.%u", &ca, &cb, &cc) == 3 &&
               (la > ca || (la == ca && (lb > cb || (lb == cb && lc > cc))));
  if (newer && url.length() > 0) {
    DBG.println("[OTA] 发现新固件，开始下载更新...");

    // 此处写入的是ESP自身Flash，成功由库复位；不是向F407的Slot B发送数据。
    t_httpUpdate_return ret = ESPhttpUpdate.update(client, url);
    switch (ret) {
    case HTTP_UPDATE_OK:
      DBG.println("[OTA] 更新成功，重启中");
      break;
    case HTTP_UPDATE_FAILED:
      DBG.printf("[OTA] 更新失败: %d\n", ESPhttpUpdate.getLastError());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      DBG.println("[OTA] 版本相同，无需更新");
      break;
    }
  }
}

// 保持原有重连策略；连接过程会占用主循环，长期稳定性仍需实板验证。
void connectWiFi() {
  DBG.print("[WiFi] 连接 ");
  DBG.print(WIFI_SSID);
  DBG.print(" ...");
  // 以STA模式加入既有Wi-Fi；网关本身不创建供F407连接的无线热点。
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 30) {
    delay(500);
    DBG.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    DBG.print(" 成功! IP = ");
    DBG.println(WiFi.localIP());
  } else {
    DBG.println(" 失败! 将重试");
  }
}

// UDP 自动发现中转服务；广播格式是兼容协议，不随项目改名。
bool discoverServer() {
  WiFiUDP udp;
  if (!udp.begin(DISCOVER_PORT)) {
    DBG.println("[DISC] udp.begin 失败，跳过问路");
    return false;
  }

  udp.beginPacket(IPAddress(255, 255, 255, 255), DISCOVER_PORT);
  udp.write(DISCOVER_MAGIC);
  udp.endPacket();

  // 同时发送全网广播和子网广播，以适应不同热点的广播转发方式。
  IPAddress bcast = WiFi.broadcastIP();
  if (bcast != IPAddress(255, 255, 255, 255)) {
    udp.beginPacket(bcast, DISCOVER_PORT);
    udp.write(DISCOVER_MAGIC);
    udp.endPacket();
  }
  DBG.println("[DISC] 已广播问路，等应答...");

  unsigned long deadline = millis() + DISCOVER_WAIT_MS;
  bool ok = false;

  // 等待发现应答时继续接收 F407，最长 DISCOVER_WAIT_MS。
  while (millis() < deadline) {
    if (udp.parsePacket() > 0) {
      char buf[64];
      int n = udp.read(buf, sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = 0;
        String reply = String(buf);
        reply.trim();

        // 只接受约定发现前缀并验证IPv4；服务端监听端口仍需与本地配置一致。
        if (reply.startsWith("ARDP_IP:")) {
          String rest = reply.substring(8);
          int colon = rest.lastIndexOf(':');
          String ipStr = (colon > 0) ? rest.substring(0, colon) : rest;
          ipStr.trim();
          IPAddress resolved;
          if (ipStr.length() > 0 && resolved.fromString(ipStr)) {
            g_server_ip = ipStr;
            DBG.println("[DISC] 问到服务器: " + g_server_ip);
            ok = true;
            break;
          }
          DBG.println("[DISC] 应答格式不对，忽略: " + reply);
        }
      }
    }

    handleF407Receive();
    delay(10);
  }

  if (!ok)
    DBG.println("[DISC] 没人应答（app.py 没跑？），先用 SERVER_IPS[] 兜底");
  udp.stop();
  return ok;
}

// 优先已发现的地址，再尝试配置候选；成功地址供音频和拍照复用。
String connectAnyServer(WiFiClient &client, const char *tag) {
  // 候选空间预留一个发现地址及配置名单；按顺序尝试，不同时保持多路连接。
  const int MAX_CAND = sizeof(SERVER_IPS) / sizeof(SERVER_IPS[0]) + 1;
  String candidates[MAX_CAND];
  int ipCount = 0;
  if (g_server_ip.length() > 0)
    candidates[ipCount++] = g_server_ip;
  for (int i = 0; i < (int)(sizeof(SERVER_IPS) / sizeof(SERVER_IPS[0])); i++) {
    candidates[ipCount++] = String(SERVER_IPS[i]);
  }

  for (int i = 0; i < ipCount; i++) {
    DBG.println(String("[HTTP] 尝试连接 ") + candidates[i] + ":" + String(SERVER_PORT) + " [" +
                tag + "]");
    if (client.connect(candidates[i].c_str(), SERVER_PORT)) {
      g_server_ip = candidates[i];
      DBG.println("[HTTP] 已连接 " + candidates[i]);
      return candidates[i];
    }
    client.stop();
    delay(200);
  }

  DBG.println(String("[HTTP] 所有服务器地址连接失败 [") + tag + "]");

  // 全部连接失败后清除路由，让主循环重新发现服务。
  if (g_server_ip.length() > 0) {
    DBG.println("[DISC] 作废已发现的 " + g_server_ip + "，稍后重新问路");
    g_server_ip = "";
  }
  return "";
}

// controls/idle/motion分别维护确认版本，避免其中一类确认误覆盖其他配置。
static uint32_t controlAck = 0u;
static uint32_t controlSentAt = 0u;
static uint32_t controlSentRevision = 0u;
static uint16_t motionSequence = 0u;
static String motionState = "";
static BridgePose boardPose;
static uint32_t idleAck = 0u, idleSent = 0u, idleSentAt = 0u;

// 上报传感器、配置确认和板端姿态，同时取得待执行命令。
void sendToServer() {

  DynamicJsonDocument bodyDoc(1536);
  bodyDoc["token"] = API_TOKEN;
  bodyDoc["device_id"] = DEVICE_ID;
  bodyDoc["control_ack"] = controlAck;
  bodyDoc["motion_ack"] = motionAck;
  bodyDoc["motion_state"] = motionState;
  bodyDoc["idle_ack"] = idleAck;
  // 只上报1500ms内新鲜的板端姿态；未更新时省略，避免网页误把旧位置当实时位置。
  if (boardPose.valid && millis() - boardPose.seen <= 1500u) {
    JsonObject pose = bodyDoc.createNestedObject("board_pose");
    pose["uptime_ms"] = boardPose.uptime;
    pose["mask"] = boardPose.mask;
    pose["age_ms"] = (uint32_t)(millis() - boardPose.seen);
    pose["moving"] = boardPose.moving != 0u;
    pose["idle"] = boardPose.idle;
    pose["idle_revision"] = boardPose.revision;
    JsonArray angles = pose.createNestedArray("angles");
    for (unsigned i = 0; i < 3; i++)
      angles.add(boardPose.angle[i]);
  }
  bodyDoc["motion_cache"] = motionProfileCache.cached();
  // 这些是网关传输统计，不是麦克风初始化结果；采样/初始化由F407上报。
  JsonObject audio = bodyDoc.createNestedObject("audio_transport");
  audio["version"] = FW_VERSION;
  audio["phase"] = audioTransport.phase();
  audio["accepted"] = audioTransport.accepted();
  audio["stored"] = audioTransport.stored();
  audio["sent"] = audioTransport.sent();
  audio["total"] = audioTransport.total();
  audio["error"] = audioTransport.failureReason();
  audio["uart_overruns"] = audioTransport.overrunCount();
  audio["retries"] = audioTransport.retries();
  bodyDoc["sensor"] = (sensorData.length() > 0) ? sensorData : String("无数据");
  bodyDoc["user"] = userMsg;
  // 待确认信息装入本次请求后清空；该通道沿用原有单槽语义，并非持久消息队列。
  if (pendingAck.length() > 0) {
    bodyDoc["ack"] = pendingAck;
    pendingAck = "";
  }

  // 仅有用户消息时申请 AI 决策，周期状态上报不触发对话。
  bodyDoc["need_ai"] = (userMsg.length() > 0);
  String body;
  serializeJson(bodyDoc, body);
  // 本次用户消息已进入请求体，清理以免每次周期轮询重复请求AI。
  userMsg = "";

  DBG.println("\n[HTTP] 本机IP: " + WiFi.localIP().toString());
  DBG.printf("[HTTP] request body bytes=%u\n", (unsigned)body.length());

  WiFiClient client;
  String serverIP = connectAnyServer(client, "data");
  if (serverIP.length() == 0)
    return;

  client.print(String("POST ") + API_PATH + " HTTP/1.1\r\n");
  client.print("Host: " + serverIP + ":" + String(SERVER_PORT) + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: " + String(body.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  String response = "";
  // 等待响应设总超时，并在等待循环持续处理串口；新录音开始即让出。
  unsigned long hardTimeout = millis() + HTTP_TIMEOUT_MS;

  // 等待首字节与接收正文共用总截止时间；delay让Wi-Fi协议栈有机会运行。
  while (client.available() == 0) {
    if (millis() > hardTimeout) {
      DBG.println("[HTTP] 等待响应超时");
      client.stop();
      return;
    }
    handleF407Receive();
    if (audioBusy()) {
      client.stop();
      return;
    }
    delay(10);
  }

  while (client.available() || client.connected()) {
    while (client.available()) {
      response += (char)client.read();
      if (response.length() > 4096)
        break;
    }
    if (millis() > hardTimeout)
      break;
    handleF407Receive();
    if (audioBusy()) {
      client.stop();
      return;
    }
    delay(5);
  }
  client.stop();

  // 先拆HTTP头与正文，再解析JSON；没有正文或JSON错误时不生成板端动作。
  int headerEnd = response.indexOf("\r\n\r\n");
  String headers = (headerEnd >= 0) ? response.substring(0, headerEnd) : response;
  String respBody = (headerEnd >= 0) ? response.substring(headerEnd + 4) : response;
  respBody.trim();

  int nl = headers.indexOf('\n');
  if (nl >= 0) {
    String statusLine = headers.substring(0, nl);
    DBG.println("[HTTP] 状态: " + statusLine);
  }
  DBG.println("[HTTP] 响应: " + respBody);

  if (respBody.length() == 0) {
    DBG.println("[HTTP] 响应为空");
    return;
  }

  parseAIResponse(respBody);
}

// 将结构化动作转为 F407 文本帧，保留服务器给出的速度。
void forwardMovesToF407(JsonVariant mvJson) {
  if (mvJson.isNull())
    return;
  String frames = "";
  for (JsonObject m : mvJson.as<JsonArray>()) {
    String type = m["type"] | "";
    if (type.length() == 0)
      continue;
    String line = "@MOVE:" + type;
    // 舵机与归中动作显式保留speed字段；没有指定speed时交给板端/配置默认策略。
    if (type == "servo") {
      if (m["id"].isNull() || m["angle"].isNull())
        continue;
      line += "," + String((int)m["id"]) + "," + String((int)m["angle"]);
      if (m.containsKey("speed"))
        line += "," + String((int)m["speed"]);
    } else if (type == "home") {
      line += "," + (m["id"].isNull() ? String("all") : String((int)m["id"]));
      if (m.containsKey("speed"))
        line += "," + String((int)m["speed"]);
    } else if (type == "gesture") {
      line += "," + String((const char *)(m["name"] | ""));
    } else if (type == "expression") {
      line += "," + String((const char *)(m["face"] | ""));
    } else {
      for (JsonPair kv : m) {
        if (String(kv.key().c_str()) == "type")
          continue;
        JsonVariant v = kv.value();
        if (!v.isNull() && !v.is<JsonObject>() && !v.is<JsonArray>()) {
          line += "," + v.as<String>();
        }
      }
    }
    frames += line + "\n";
  }
  if (frames.length() > 0) {
    Serial.print(frames);
    DBG.println("[UART] -> F407 moves:");
    DBG.print(frames);
  }
}

// 拆分 @...# 文本帧；时间序列转换为二进制，避免超过行缓冲长度。
void forwardAtHashRawToF407(const String &rawIn) {
  String raw = rawIn;
  raw.trim();
  if (raw.length() == 0)
    return;

  // 单个时间序列也走二进制路径；缺少分隔符不改变其语义。
  if (raw.startsWith("@TIMELINE:") && raw.indexOf('#') < 0)
    raw += "#";

  if (raw.indexOf('#') < 0) {
    if (!raw.endsWith("\n"))
      raw += "\n";
    Serial.print(raw);
    DBG.print("[UART] -> F407 raw(nl): ");
    DBG.print(raw);
    return;
  }

  String frames = "";
  int from = 0;
  while (from < (int)raw.length()) {
    int at = raw.indexOf('@', from);
    if (at < 0)
      break;
    int hash = raw.indexOf('#', at + 1);
    if (hash < 0) {

      String rest = raw.substring(at);
      rest.trim();
      if (rest.length())
        frames += rest + "\n";
      break;
    }
    String one = raw.substring(at, hash);
    one.trim();
    // 先发出此前累积的文本，再发二进制任务，保持原命令顺序。
    if (one == "@TIMELINE:STOP") {

      if (frames.length()) {
        Serial.print(frames);
        frames = "";
      }
      uint8_t stop[8] = {0xA5, 0x5A, 0x21, 0, 0, 0, 0, 0x21};
      Serial.write(stop, sizeof(stop));
    } else if (one.startsWith("@TIMELINE:")) {
      if (frames.length()) {
        Serial.print(frames);
        frames = "";
      }
      // 为时间序列生成非零帧序号；STOP与普通动作仍保持服务端给定的先后顺序。
      if (!motionSequence)
        motionSequence = (uint16_t)(micros() | 1u);
      motionSequence++;
      // 为时间序列生成非零帧序号；STOP与普通动作仍保持服务端给定的先后顺序。
      if (!motionSequence)
        motionSequence++;
      if (!motionSendHex(Serial, one.c_str() + 10, 0x20, motionSequence))
        DBG.println("[TIMELINE] invalid encoding; nothing sent");
    } else if (one.length())
      frames += one + "\n";
    from = hash + 1;
  }
  if (frames.length() > 0) {
    Serial.print(frames);
    DBG.println("[UART] -> F407 raw(@#=>\\n):");
    DBG.print(frames);
  }
}

// 网页命令优先使用 raw；没有 raw 时兼容 moves 和文字字段。
void forwardCommandToF407(JsonVariant cmd) {
  if (cmd.isNull() || !cmd.is<JsonObject>())
    return;
  DBG.println("[CMD] 网页动作命令 -> F407");

  const char *raw = cmd["raw"] | "";
  if (String(raw).length() > 0) {

    DBG.println("[WEB==>] 收到网页命令 raw: " + String(raw));
    forwardAtHashRawToF407(String(raw));
    return;
  }

  if (cmd.containsKey("moves")) {
    forwardMovesToF407(cmd["moves"]);
  }
  const char *screen = cmd["text"] | "";
  if (*screen) {
    Serial.print("@TEXT:");
    Serial.println(screen);
  }
}

// 先独立解析 OTA 公告，仅登记任务；耗时操作留给 iapPullTick。
void parseAIResponse(const String &respBody) {

  {
    // 过滤只提取iap子树，防止对话正文占满这份小文档；这里只登记，不开始下载。
    StaticJsonDocument<64> iapOnly;
    iapOnly["iap"] = true;
    StaticJsonDocument<384> iapDoc;
    if (!deserializeJson(iapDoc, respBody, DeserializationOption::Filter(iapOnly)) &&
        !iapDoc["iap"].isNull()) {
      iapPullAnnounce(iapDoc["iap"]);
    }
  }

  // 再解析普通指令和配置；两次解析的内存预算互相独立。
  DynamicJsonDocument respDoc(8192);
  DeserializationError err = deserializeJson(respDoc, respBody);
  if (err) {
    DBG.println("[AI] JSON解析失败: " + String(err.c_str()));
    DBG.println("[AI] 原始内容: " + respBody);
    return;
  }

  // 配置按 revision 重发与确认，收到 NEED 后重新同步，不凭网页缓存判成功。
  JsonObject controls = respDoc["controls"];
  JsonObject limits = respDoc["motion_profile"];
  // motion_profile通过CRC/revision匹配后才缓存，配置重发不会额外生成固定90度动作。
  uint32_t limitsRevision = limits["revision"] | 0u;
  if (limitsRevision && !audioBusy() && !iapPullBusy() && !mcuUpdating &&
      (motionSentRevision != limitsRevision || millis() - motionSentAt >= 5000u)) {
    const char *wire = limits["wire"] | "";
    const bool changed = motionProfileCache.revision() != limitsRevision;
    uint32_t checked = 0u;
    if (*wire && motionProfileValid(wire, checked) && checked == limitsRevision &&
        motionProfileCache.accept(wire)) {
      if (changed)
        motionReplayRequested = true;
      replaySavedMotionProfile();
    }
  }
  // controls传递VAD阈值、起音/静音时长及默认速度，板端确认号才表示应用成功。
  uint32_t revision = controls["revision"] | 0u;
  if (revision && !audioBusy() && !iapPullBusy() && !mcuUpdating &&
      (controlSentRevision != revision || millis() - controlSentAt >= 5000u)) {
    char frame[100];
    snprintf(frame, sizeof(frame), "@CONFIG:%lu,%u,%u,%u,%u,%u,%u\n", (unsigned long)revision,
             (controls["enabled"] | true) ? 1u : 0u, (unsigned)(controls["minimum"] | 900u),
             (unsigned)(controls["margin"] | 800u), (unsigned)(controls["onset_ms"] | 160u),
             (unsigned)(controls["silence_ms"] | 800u), (unsigned)(controls["servo_speed"] | 90u));
    Serial.print(frame);
    if (controlSentRevision != revision)
      controlAck = 0u;
    controlSentRevision = revision;
    controlSentAt = millis();
  }

  // 待机动作快照走0x23二进制；只在修订变化或尚未确认且到重试时间时发送。
  JsonObject idle = respDoc["idle_profile"];
  uint32_t idleRevision = idle["revision"] | 0u;
  if (idleRevision && !audioBusy() && !iapPullBusy() && !mcuUpdating &&
      (idleSent != idleRevision || (idleAck != idleRevision && millis() - idleSentAt >= 5000u))) {
    const char *wire = idle["wire"] | "";
    if (motionSendHex(Serial, wire, 0x23u, ++motionSequence)) {
      if (idleSent != idleRevision)
        idleAck = 0u;
      idleSent = idleRevision;
      idleSentAt = millis();
    }
  }

  const char *message = respDoc["message"] | "";
  const char *text = respDoc["text"] | "";
  const char *raw = respDoc["raw"] | "";

  DBG.println("--------------------------------------------------");
  DBG.println("[AI] 消息  : " + String(message));
  DBG.println("[AI] 屏幕文字(text): " + String(text));
  if (String(raw).length() > 0) {
    DBG.println("[AI] 原始  : " + String(raw));
  }

  // 周期上报 ai=false 不刷新 AI 对话；网页 command 仍可执行。
  bool needForward = true;
  if (respDoc.containsKey("ai") && respDoc["ai"] == false)
    needForward = false;

  // 优先采用服务端已编码的动作帧；存在timeline时避免再将moves重复执行一遍。
  const char *deviceFrames = respDoc["device_frames"] | "";
  if (needForward && *deviceFrames) {
    forwardAtHashRawToF407(String(deviceFrames));
  } else if (needForward && !respDoc.containsKey("timeline")) {
    String frames = "";
    JsonVariant mvJson = respDoc["moves"];
    if (!mvJson.isNull()) {
      forwardMovesToF407(mvJson);
    } else if (respDoc.containsKey("action")) {
      String line = "@MOVE:" + String((const char *)(respDoc["action"] | ""));
      if (respDoc.containsKey("value"))
        line += "," + String((int)(respDoc["value"] | 0));
      frames += line + "\n";
    }

    String screen = (String(text).length() > 0) ? String(text) : String(message);

    if (screen.length() > 0 && !(respDoc.containsKey("ai") && respDoc["ai"] == false)) {
      frames += "@TEXT:" + screen + "\n";
    }
    if (frames.length() > 0) {
      Serial.print(frames);
      DBG.println("[UART] -> F407 决策帧:");
      DBG.print(frames);
    }
  } else {
    DBG.println("[UART] 未调用AI，跳过决策转发");
  }

  if (respDoc.containsKey("command")) {
    forwardCommandToF407(respDoc["command"]);
  }
}

// 路由文本上报、物理确认和配置 ACK；只置状态，不在此发 HTTP。
void handleF407Text(const String &line) {
  if (camera.text(line))
    return;
  // 姿态消息优先独立处理，不将其当传感器字符串或普通ACK拼接上云。
  if (line.startsWith("@POSE:")) {
    boardPose.accept(line.c_str(), millis());
    return;
  }
  if (line.startsWith("@IDLE:OK,")) {
    idleAck = (uint32_t)strtoul(line.c_str() + 9, nullptr, 10);
    return;
  }
  if (line.startsWith("@LIMITS:OK,")) {
    motionAck = (uint32_t)strtoul(line.c_str() + 11, nullptr, 10);
  } else if (line.startsWith("@TIMELINE:")) {
    motionState = line.substring(10);
    pendingAck = "TIMELINE:" + motionState;
    ackReadyNow = true;
    // F407 重启后使旧确认失效，重新发送配置及离线舵机参数。
  } else if (line == "@CONFIG:NEED") {
    controlAck = controlSentRevision = 0u;
    idleAck = idleSent = 0u;
    boardPose.valid = false;
    motionAck = motionSentRevision = 0u;
    motionReplayRequested = true;
  } else if (line.startsWith("@CONFIG:OK,")) {
    controlAck = (uint32_t)strtoul(line.c_str() + 11, nullptr, 10);
  } else if (line.startsWith("@IAP:WAIT,")) {
    mcuApproval = 1;
    mcuMessage = "Waiting for F407 K-UP confirmation";
    DBG.println("[IAP] F407 asks for physical K-UP confirmation");
  } else if (line.startsWith("@IAP:OK,")) {
    // 这里只记录物理按键确认；实际擦除仍由后续上传流程验证同一份长度/CRC后触发。
    mcuApproval = 2;
    mcuMessage = "F407 user confirmation received";
    DBG.println("[IAP] F407 user confirmation received");
  } else if (line.startsWith("@IAP:NO,")) {
    mcuApproval = 0;
    mcuMessage = "F407 OTA confirmation expired or was cancelled";
    DBG.println("[IAP] F407 OTA confirmation expired/cancelled");
  } else if (line.startsWith("@IAP:ERR,")) {
    mcuApproval = 0;
    mcuMessage = "F407 rejected OTA request";
    DBG.println("[IAP] F407 rejected OTA request");
  } else if (line.startsWith("@SENSOR:")) {
    sensorData = line.substring(8);
    DBG.println("[F407] 传感器数据: " + sensorData);
  } else if (line.startsWith("@USER:")) {
    userMsg = line.substring(6);
    DBG.println("[F407] 用户消息: " + userMsg);
  } else if (line.startsWith("@ALERT:")) {

    userMsg = line.substring(7);
    DBG.println("[F407] 异常事件: " + userMsg);
    ackReadyNow = true;
  } else if (line == "@PONG") {
    DBG.println("[LINK] F407 @PONG: UART round trip OK");
  } else if (line.startsWith("@ACK:")) {
    pendingAck = line.substring(5);
    DBG.println("[F407] ACK: " + pendingAck);
    ackReadyNow = true;
  } else {
    DBG.printf("[F407] ignored non-protocol line (%u bytes)\n", (unsigned)line.length());
  }
}

// 串口错误时重置拆帧器，请求音频重传；完整帧再按类型分发。
void handleF407Receive() {
  if (Serial.hasOverrun() || Serial.hasRxError()) {
    bridgeWire.reset();
    audioTransport.uartOverrun();
    audioBridge.badFrame();
    DBG.println("[LINK] UART RX error/overflow: retry expected audio frame");
  }
  while (Serial.available()) {
    // 逐字节推进状态；读取到完整帧后立即消费其缓冲，下一次feed可能覆盖内容。
    int result = bridgeWire.feed((uint8_t)Serial.read(), millis());
    if (result == 1) {
      String line(bridgeWire.text);
      line.trim();
      handleF407Text(line);
    } else if (result == 2) {
      if (bridgeWire.type == 1) {
        DBG.printf("[AUDIO] START frame from F407: seq=%u len=%u\n", (unsigned)bridgeWire.seq,
                   (unsigned)bridgeWire.len);
      }
      // 相机 0x30~0x32，IAP 0x10~0x1F，其余交给音频协议。
      if (bridgeWire.type >= 0x30 && bridgeWire.type <= 0x32)
        camera.frame(bridgeWire.type, bridgeWire.seq, bridgeWire.payload, bridgeWire.len);
      else if (bridgeWire.type >= 0x10 && bridgeWire.type <= 0x1F)
        mcuOnFrame(bridgeWire.type, bridgeWire.seq, bridgeWire.payload, bridgeWire.len);
      else if ((cameraBusy() || mcuUpdating || iapPullBusy() || mcuApproval != 0) &&
               bridgeWire.type == 1) {
        uint8_t no = 2;
        mcuFrame(4, 0, &no, 1);
      } else
        audioBridge.frame(bridgeWire.type, bridgeWire.seq, bridgeWire.payload, bridgeWire.len,
                          millis());
    } else if (result < 0)
      audioBridge.badFrame();
  }
}
