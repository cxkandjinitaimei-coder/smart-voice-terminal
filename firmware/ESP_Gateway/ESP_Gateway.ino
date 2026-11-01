/*
 * ============================================================
 *  ESP8266 桥接网关（无本地传感器，转发 F407 数据 + AI决策回传）
 * ============================================================
 *
 *  角色：ESP8266 只做“桥”——把 F407 上报的数据 POST 给 Flask 中转站，
 *        收到 AI 决策后通过 UART 转发回 F407。
 *
 *  数据流向：
 *      F407 ──UART──► ESP8266 ──HTTP POST──► Flask中转站
 *          ◄────────       ◄──HTTP响应────  (端口5000)
 *
 *  说明：
 *    - 传感器数据 / 用户请求由 F407 通过 UART 发来，ESP 无本地传感器
 *    - 也可以直接在 PC 网页仪表盘（http://PC_IP:5000）发数据测试，
 *      那是另一条通道，不经过 ESP，两条通道互不影响
 *    - 串口监视器输入一行文字回车，立即上报一次（调试用）
 *
 *  中转站返回的统一决策 JSON（信封 + moves，一次决策可含多条不同类型动作）：
 *    {"message":"你好呀","moves":[{"type":"servo","id":1,"angle":90},
 *     {"type":"gesture","name":"nod"},{"type":"expression","face":"happy"}],"raw":"..."}
 *  ESP 解析后【逐条拆成类型化帧】再转发给 F407（F407 只需字符串解析，不需要 JSON 库）：
 *    @MOVE:servo,1,90 \n  @MOVE:gesture,nod\n  @MOVE:expression,happy\n
 *    屏幕文字（text 优先，无则用 message）→ @TEXT:你好呀\n
 *
 *  UART 帧协议（与 F407 通信，行分隔）：
 *    ESP→F407:  @MOVE:<type>,参数...\n   （逐条动作，type 见决策协议v2）
 *    ESP→F407:  @TEXT:<文字>\n            （屏幕显示文字）
 *    F407→ESP:  @SENSOR:温度:28.5,湿度:65\n   （传感器数据）
 *    F407→ESP:  @USER:我热了\n                （用户请求）
 *    F407→ESP:  @ALERT:温度过高\n             （异常/事件，预留：收到立即上报并请求AI决策）
 *
 *  与 F407 接线（NodeMCU 硬件串口，不要再用 SoftSerial D1/D2）：
 *    ESP GPIO1 (TX)  →  F407 PC7 (USART6 RX)   [H1（右侧）排针 第 38 针]
 *    ESP GPIO3 (RX)  ←  F407 PC6 (USART6 TX)   [H1（右侧）排针 第 39 针]
 *    GND ↔ GND，当前波特率 921600（以实际 Serial.begin 定义为准）
 *  ⚠️ 2026-09-18 从 USART3(PB10/PB11) 迁到 USART6(PC6/PC7)：PB10 正是麦克风
 *     I2S2_CK 的脚，麦克风和 ESP 原本物理二选一。详见 ST32 侧 esp_uart.h。
 *  ⚠️ 波特率 2026-09-18 从 9600 升到 115200：一次连发 5 帧约 150 字节，
 *     9600 下光上线就要 156ms，而 F407 画一张表情要 150~300ms ⇒ 必然丢帧。
 *     ★ 必须与 ST32 侧 esp_uart.h 的 ESP_BAUD 保持一致，改一处不改另一处 =
 *       整条链路变乱码。
 *  调试日志走 Serial1(GPIO2/D4 仅TX)；USB 监视器改看 F407 线请设 115200，会与板间通信混在一起。
 *
 *  依赖库：ArduinoJson (v6)；ESP8266WiFi / WiFiUdp 均已内置。
 *
 *  ★ 服务器 IP 自动发现（2026-09-20 新增，见 discoverServer）：
 *    换网/换热点后 PC 的 DHCP 地址会变，写死 IP 必然对不上而且两端都不报错。
 *    现在 ESP 主动 UDP 广播问路，PC 上跑着的 app.py 直接回自己的当前 IP，
 *    SERVER_IPS[] 退化为兜底名单。⇒ **换网不用再改 IP、也不用为重烧 ESP 拔线。**
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include "firmware_version.h"
#include "bridge_motion.h"
#include "bridge_text.h"
static BridgeText screenText;
#include "bridge_timeline.h"
static BridgeTimeline timelineTransport;
#include "bridge_sampled_motion.h"
static BridgeSampledMotion stepPlayer;
static const char *timelineCommandId="";
static void timelinePace() { delay(1); }
#include "bridge_pet.h"
#include "bridge_motion_cache.h"
#include <WiFiUdp.h>             // 服务器 IP 自动发现：UDP 广播问路（见 discoverServer）
#include <ArduinoJson.h>
#include <ArduinoOTA.h>          // 局域网无线烧录（Arduino IDE 选网络端口）
#include <ESP8266httpUpdate.h>   // HTTP OTA：从云端服务器下载固件
#include <LittleFS.h>            // 音频公网背压时暂存 PCM，录音完成后继续上传

/* ===================== 配置区 ===================== */

// 本地配置从 gateway_settings.h 读取；真实凭据放在未跟踪的 gateway_config.h。
#include "gateway_settings.h"

// ---- OTA：无线升级检查与版本比较 ----
#define FW_VERSION        ESP_FW_VERSION // ESP OTA 比较使用的数字版本
#define OTA_CHECK_INTERVAL 60000    // 每 60 秒检查一次是否有新固件
#define OTA_DEVICE_NAME   "esp-bridge"   // ArduinoOTA 在 IDE 里显示的名字
#define DEVICE_ID         "esp-bridge"   // 云端命令队列用的设备 ID

// ---- 与 F407 通信的 UART（硬件 Serial = GPIO1 TX / GPIO3 RX）----
#define F407_BAUD        921600
// 调试日志：Serial1 仅 TX，NodeMCU D4 = GPIO2（可选接 USB-TTL 看日志）
#define DBG Serial1

// ---- 上报间隔（毫秒）----
// 这个值 = 网页点完灯最坏要等多久才被 ESP 取走：5000→1 秒内响应。
// 本地 Flask 扛得住 1s/次；嫌刷屏可改回 1500~2000。
#define SEND_INTERVAL    1000
#define HTTP_TIMEOUT_MS  3000             // 单次 HTTP 等响应超时（原来 15000 太久）
#define UART_SELFTEST    0                // 1=上电不经云端，直接给 F407 发舵机帧做接线自检

/* ===================== 全局对象 ===================== */

String sensorData = "";   // F407上报的传感器数据（未收到则为空，后端按"无"处理）
String userMsg    = "";   // 用户消息（F407或串口手动输入）
String pendingAck = "";   // F407 @ACK: 回传，下次/立即上报给云端网页看
unsigned long lastSend = 0;
unsigned long lastOtaCheck = 0;   // 上次检查OTA的时间
bool   ackReadyNow = false;  // 非阻塞：收到@ACK/@ALERT只置位，由主循环尽快上报

String g_server_ip = "";              // ★ 自动发现到的服务器 IP（空 = 还没问到 / 已失效）
unsigned long lastDiscover = 0;       // 上次问路的时间（限速用）

#include "bridge_audio_esp.h"
static bool cameraBusy();
static void serviceBridge();
#include "bridge_iap_esp.h"
#include "bridge_camera_esp.h"
static bool bridgeServicesReady=false;
static uint32_t lastPongAt=0;

// ★ 必须先声明再 include —— bridge_iap_pull.h 里会用到它（挑一个连得上的服务器）。
//   这是本文件自己的规矩：定义放下面、声明集中放这里（见 handleF407Receive）。
String connectAnyServer(WiFiClient &client, const char *tag);
#include "bridge_iap_pull.h"

void handleF407Receive();
void handleF407Text(const String& line);
static SavedMotionProfile<fs::FS> motionProfileCache;
static uint32_t motionAck = 0u, motionSentRevision = 0u, motionSentAt = 0u;
static bool motionReplayRequested=true;
void replaySavedMotionProfile();

void forwardMovesToF407(JsonVariant mvJson);
void forwardCommandToF407(JsonVariant cmd);
bool discoverServer();                // 定义在下面「服务器自动发现」一节

/* ===================== 主流程 ===================== */

void uartSelfTestToF407() {
  // 不经 WiFi/云端：直接验证 NodeMCU GPIO1/3 ↔ F407 PC6/PC7（USART6）
  // （2026-09-18 起 ESP 走 USART6；旧注释写的 PB10/11 是 USART3，早已不用）
  const char* frames = "@PING\n";
  Serial.print(frames);
  Serial.flush();
  DBG.println("[SELFTEST] UART ping only; no startup servo movement");
  DBG.print(frames);
}

void replaySavedMotionProfile() {
  const uint32_t revision=motionProfileCache.revision();
  if(!revision || audioBusy() || iapPullBusy() || mcuUpdating || mcuApproval!=0) return;
  if(!motionReplayRequested &&
     (motionAck==revision || millis()-motionSentAt<2000u)) return;
  if(motionSendHex(Serial,motionProfileCache.wire(),0x22,0u)) {
    if(motionSentRevision!=revision) motionAck=0u;
    motionSentRevision=revision;motionSentAt=millis();motionReplayRequested=false;
  }
}

void setup() {
  // 与 F407：硬件 UART0 = GPIO1(TX)/GPIO3(RX)，921600；调试另走 GPIO2。
  Serial.setRxBufferSize(4096);
  Serial.begin(F407_BAUD);
  Serial.setDebugOutput(false);
  // 调试：GPIO2(D4) 仅 TX；没有 USB-TTL 时也可暂时忽略
  DBG.begin(115200);
  DBG.println();
  DBG.printf("[ESP8266] FW=%s build=%s (F407 UART 921600)\n", FW_VERSION, ESP_BUILD_ID);
  DBG.println("[TIP] 服务器 IP 自动发现：跑着 app.py 的电脑会自动应答，换网不用重烧");
  audioTransport.begin();
  motionProfileCache.begin(LittleFS);
  replaySavedMotionProfile();
  DBG.printf("[HOME] saved profile=%lu, cache=%s; no fixed 90-degree commands\n",
             (unsigned long)motionProfileCache.revision(),motionProfileCache.cached()?"ready":"not ready");

#if UART_SELFTEST
  uartSelfTestToF407();
#endif

  connectWiFi();

  // ★ 联网后先问路，拿到 PC 当前 IP（换热点/换网段也不用再重烧）
  // ⚠️ 要 app.py 正在跑才有人应答；没人应答就先走 SERVER_IPS[] 兜底，
  //    主循环会每隔 DISCOVER_RETRY_MS 再问一次，电脑起来了自然就接上。
  if (WiFi.status() == WL_CONNECTED) {
    discoverServer();
    lastDiscover = millis();
  }

  // ---- ArduinoOTA：局域网无线烧录 ----
  ArduinoOTA.setHostname(OTA_DEVICE_NAME);
  ArduinoOTA.setPassword(API_TOKEN);
  ArduinoOTA.onStart([]() {
    DBG.println("[OTA] 开始无线更新...");
  });
  ArduinoOTA.onEnd([]() {
    DBG.println("\n[OTA] 更新完成");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    DBG.printf("[OTA] 错误 %d\n", err);
  });
  ArduinoOTA.begin();
  mcuUpdateBegin();
  bridgeServicesReady=true;
  DBG.println("[OTA] 无线烧录已就绪 (局域网)");

  // 联网后立刻上报一次，尽快取网页队列里的命令
  lastSend = millis() - SEND_INTERVAL;
}

void loop() {
  // 重启校验也占用OTA资源；即使网页关闭，180秒上限仍能释放，避免永远停录。
  serviceBridge();
  /* 正文独立于动作发送，等板端接收确认后才领取下一项；忙时不烧重试次数。 */
  timelineTransport.tick(Serial,millis(),audioBusy() || cameraBusy() || mcuUpdating || iapPullBusy() || mcuApproval!=0,timelinePace);
  screenText.tick(Serial,millis(),mcuUpdating || iapPullBusy() || mcuApproval!=0);
  audioBridge.tick(millis());
  audioTransport.poll();
  if (audioTransport.failed()) audioBridge.fail(audioTransport.failureReason());
  // 阶段：录音拥有当前网络和 UART 预算；本轮不做阻塞式 HTTP 轮询或 OTA。
  if (audioBusy()) { delay(0); return; }
  camera.tick(!iapPullBusy() && !mcuUpdating && mcuApproval==0);
  if(cameraBusy()) { delay(0); return; }
  replaySavedMotionProfile();
  /* 阶段：任一板重启后重新广播空闲状态，让 F407 在一段录音完成后释放
   * 自动语音触发；这里不能误报为仍在录音。 */
  static unsigned long lastAudioReady = 0;
  if (!iapPullBusy() && !mcuUpdating && mcuApproval==0 && millis() - lastAudioReady >= 3000) {
    lastAudioReady = millis();
    Serial.print("@AUDIO:READY\n");
  }
  // ★ 云端下发（IAP device_pull）的状态机必须在这里 tick：
  //   ① 必须在 `if (mcuUpdating) return;` **之前** —— 它在传输期间会自己把
  //      mcuUpdating 置真，放到那行之后就再也轮不到它了；
  //   ② 放在上面 `if (audioBusy()) return;` **之后**，顺便白拿一层"录音时不许 OTA"。
  iapPullTick();
  if (mcuUpdating) { delay(0); return; }
  // 阶段一：维护 Wi-Fi 连接；离线时保留 UART 收发，不能让网络重连阻塞主循环。
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    delay(0); return; // 无网络时仍由前面的 serviceBridge 维护 F407 心跳。
  }

  // 1.2) 服务器 IP 没问到（或刚被作废）→ 隔 DISCOVER_RETRY_MS 再问一次
  //      这段最多阻塞 DISCOVER_WAIT_MS，问路期间照常收 F407（见 discoverServer）
  if (WiFi.status() == WL_CONNECTED && g_server_ip.length() == 0 &&
      millis() - lastDiscover >= DISCOVER_RETRY_MS) {
    lastDiscover = millis();
    discoverServer();
  }

  // 1.5) ArduinoOTA 必须频繁调用，才能响应 IDE 的无线烧录请求
  ArduinoOTA.handle();

  // 阶段二：接收 F407 数据（传感器/ACK/用户消息）；硬件 Serial GPIO1/3 只收字节。
  handleF407Receive();
  if (audioBusy()) return;

  // 阶段三：F407 来了 @ACK/@ALERT 就立即上报；没有事件时按周期领取网页队列。
  //    否则每 SEND_INTERVAL(1s) 定时去取网页命令。HTTP 全程阻塞等待，
  //    但间隔已降到 1s 且只在主循环这一步调用，不在收数据函数里递归发。
  if (millis()-lastSend>=200u &&
      (ackReadyNow || stepPlayer.active() || millis() - lastSend >= SEND_INTERVAL)) {
    ackReadyNow = false;
    lastSend = millis();
    sendToServer();
  }

  // 阶段四：定时检查 ESP 自身 HTTP OTA；F407 device_pull 期间必须让出资源。
  // ★ 云端下发（IAP）在跑时不查：状态 2 正在等用户按 K-UP，此刻若重启 ESP 去刷自己，
  //   会把 F407 晾在等确认页上（它 ~120 s 后自己放弃，但用户看到的是一次莫名的失败）。
  if (!audioBusy() && !iapPullBusy() && !mcuUpdating && mcuApproval==0) checkForUpdate();
}

/* 这段也在普通 HTTP 等待中运行。服务端未应答时，不能让 8080 和心跳饿死。
 * OTA 回调内会继续收 UART，但不递归处理 HTTP；二进制帧始终整帧发送。 */
static void serviceBridge() {
  static bool entered=false;
  static uint32_t lastPing=0, lastDiag=0;
  if(entered) return;
  entered=true;
  handleF407Receive();
  stepPlayer.tick(Serial,millis(),lastPongAt && millis()-lastPongAt<8000u);
  mcuUpdateTick();
  if(!mcuUpdating && millis()-lastPing>=2000u) {
    lastPing=millis();Serial.print("@PING\n");
  }
  if(bridgeServicesReady) {
    mcuUpdateServer.handleClient();
    if(!mcuUpdating && !iapPullBusy() && !audioBusy()) ArduinoOTA.handle();
  }
  if(millis()-lastDiag>=10000u) {
    lastDiag=millis();
    DBG.printf("[HEALTH] wifi=%d ip=%s heap=%u uart_pong=%s ota=%u\n",
      (int)WiFi.status(),WiFi.localIP().toString().c_str(),ESP.getFreeHeap(),
      lastPongAt && millis()-lastPongAt<8000u?"OK":"MISSING",mcuUpdating?1:0);
  }
  entered=false;
}

/* ===================== HTTP OTA：查询、下载、校验、重启 ===================== */

void checkForUpdate() {
  if (millis() - lastOtaCheck < OTA_CHECK_INTERVAL) return;
  lastOtaCheck = millis();
  if (WiFi.status() != WL_CONNECTED) return;

  // OTA 也走**和上报同一套**候选逻辑（自动发现的地址最前，SERVER_IPS[] 依次兜底）。
  // ★ 老注释写的是「以前写死 SERVER_IPS[0]，换网后 HTTP OTA 会跟着一起静默失效」，
  //   但那个 bug **并没有真修** —— 它只是把写死的 IP 换成了另一个写死的 IP
  //   （只退到 SERVER_IPS[0]、不走候选表）。上云后 SERVER_IPS[0] 成了公网地址，
  //   这个洞会从"查不到"变成"永远只去公网查" ⇒ 局域网联调时更隐蔽。⇒ 真正走候选表。
  WiFiClient client;
  String otaIP = connectAnyServer(client, "ota-info");
  if (otaIP.length() == 0) return;

  // 阶段：向服务端读取 ESP 固件信息；token 只放在请求参数中，不写入日志。
  String path = String("/api/ota/info?token=") + API_TOKEN;
  client.print(String("GET ") + path + " HTTP/1.1\r\n");
  client.print("Host: " + otaIP + ":" + String(SERVER_PORT) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  // 读响应（等首字节 + 收完）
  String response = "";
  unsigned long hardTimeout = millis() + 8000;
  while (client.available() == 0) {
    if (millis() > hardTimeout) { client.stop(); return; }
    serviceBridge();
    if (audioBusy() || mcuUpdating || iapPullBusy() || mcuApproval!=0) { client.stop(); return; }
    delay(10);
  }
  while (client.available() || client.connected()) {
    while (client.available()) {
      response += (char)client.read();
      if (response.length() > 2048) break;
    }
    if (millis() > hardTimeout) break;
    serviceBridge();
    if (audioBusy() || mcuUpdating || iapPullBusy() || mcuApproval!=0) { client.stop(); return; }
    delay(5);
  }
  client.stop();

  int headerEnd = response.indexOf("\r\n\r\n");
  if (headerEnd < 0) return;
  String respBody = response.substring(headerEnd + 4);
  respBody.trim();

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, respBody);
  if (err) return;

  String latest = doc["version"] | "";
  String url    = doc["url"] | "";
  DBG.printf("[OTA] 当前=%s 云端=%s\n", FW_VERSION, latest.c_str());
  unsigned la=0,lb=0,lc=0,ca=0,cb=0,cc=0;
  char extra;
  bool newer=sscanf(latest.c_str(),"%u.%u.%u%c",&la,&lb,&lc,&extra)==3 &&
    sscanf(FW_VERSION,"%u.%u.%u",&ca,&cb,&cc)==3 &&
    (la>ca || (la==ca && (lb>cb || (lb==cb && lc>cc))));
  if (newer && url.length() > 0) {
    DBG.println("[OTA] 发现新固件，开始下载更新...");
    // 下载并写入 flash，成功后 ESP 会自动重启
    t_httpUpdate_return ret = ESPhttpUpdate.update(client, url);
    switch (ret) {
      case HTTP_UPDATE_OK:         DBG.println("[OTA] 更新成功，重启中"); break;
      case HTTP_UPDATE_FAILED:     DBG.printf("[OTA] 更新失败: %d\n", ESPhttpUpdate.getLastError()); break;
      case HTTP_UPDATE_NO_UPDATES: DBG.println("[OTA] 版本相同，无需更新"); break;
    }
  }
}

/* ===================== Wi-Fi：连接、重试和断线恢复 ===================== */

void connectWiFi() {
  /* 只启动/重试关联，不在这里死等 15 秒。离线时主循环仍能探测 F407。
   * WiFi 自身会继续关联；间隔 30 秒才重新 begin，避免每圈重置握手。 */
  static bool started=false;
  static uint32_t attemptedAt=0;
  if(started && millis()-attemptedAt<30000u) return;
  started=true;attemptedAt=millis();
  DBG.print("[WiFi] 连接 ");
  DBG.print(WIFI_SSID);
  DBG.print(" ...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  DBG.println(" 异步连接中；[HEALTH] 输出关联状态与 IP");
}

/* ===================== 服务器自动发现（UDP 广播问路）=====================
 * 为什么要有这段：换网/换热点后 PC 的 DHCP 地址会变，写死 IP 必然对不上，
 * 而"对不上"的表现是**静默回落到公网**（两端都不报错，极难查）。
 * 这里让 ESP 主动广播问一句，跑着 app.py 的那台 PC 会把自己的当前 IP 报回来。
 *   PC 侧实现在 flask_relay/app.py::beacon_loop()
 *   （端口 BEACON_PORT / 口令 BEACON_MAGIC 必须和上面那两个宏一致）
 * 问不到不算错：退到 SERVER_IPS[] 兜底，主循环每 DISCOVER_RETRY_MS 再问一次。
 */
bool discoverServer() {
  WiFiUDP udp;
  if (!udp.begin(DISCOVER_PORT)) {
    DBG.println("[DISC] udp.begin 失败，跳过问路");
    return false;
  }

  // 有限广播(255.255.255.255) 与子网定向广播各发一份 ——
  // 有的热点只放行其中一种，两份都发最稳。
  udp.beginPacket(IPAddress(255, 255, 255, 255), DISCOVER_PORT);
  udp.write(DISCOVER_MAGIC);
  udp.endPacket();

  IPAddress bcast = WiFi.broadcastIP();
  if (bcast != IPAddress(255, 255, 255, 255)) {
    udp.beginPacket(bcast, DISCOVER_PORT);
    udp.write(DISCOVER_MAGIC);
    udp.endPacket();
  }
  DBG.println("[DISC] 已广播问路，等应答...");

  unsigned long deadline = millis() + DISCOVER_WAIT_MS;
  bool ok = false;

  while (millis() < deadline) {
    if (udp.parsePacket() > 0) {
      char buf[64];
      int n = udp.read(buf, sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = 0;
        String reply = String(buf);
        reply.trim();
        // 约定格式：ARDP_IP:<IP>:<端口>
        // （自己那份广播若被 AP 回环，这里是 "ARDP_DISCOVER"，不匹配就静默跳过）
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
    // ★ 等应答期间照常收 F407 —— 不这么写，阻塞这 1.5 秒会把 F407 发来的行憋丢
    //   （与 sendToServer() 的等待循环同一个道理）
    serviceBridge();
    delay(10);
  }

  if (!ok) DBG.println("[DISC] 没人应答（app.py 没跑？），先用 SERVER_IPS[] 兜底");
  udp.stop();
  return ok;
}

/* ===================== 上报与响应处理 ===================== */

/* 挑一个连得上的服务器：**自动发现的地址永远排第一**，SERVER_IPS[] 依次兜底。
   成功 ⇒ 返回连上的那个 IP（空串 = 一个都连不上，调用方自己处置）。
   ★ 抽出来是为了让 `checkForUpdate()` 和 `sendToServer()` 走**同一套候选逻辑** ——
     以前前者只退到 SERVER_IPS[0]、后者才走全表；`[0]` 一改，前者就只是把
     "静默查不到"换个 IP 而已（见 checkForUpdate 里那段老注释，那个洞一直没修）。
   ⚠️ 副作用（是**刻意**的）：把连上的那个记回 g_server_ip —— 一路复用同一个中转站，
     音频流也用它。连不上时**作废** g_server_ip，让主循环重新问路。 */
String connectAnyServer(WiFiClient &client, const char *tag) {
  client.setTimeout(600);
  const int MAX_CAND = sizeof(SERVER_IPS) / sizeof(SERVER_IPS[0]) + 1;
  String candidates[MAX_CAND];
  int ipCount = 0;
  if (g_server_ip.length() > 0) candidates[ipCount++] = g_server_ip;
  for (int i = 0; i < (int)(sizeof(SERVER_IPS) / sizeof(SERVER_IPS[0])); i++) {
    candidates[ipCount++] = String(SERVER_IPS[i]);
  }

  for (int i = 0; i < ipCount; i++) {
    serviceBridge();
    if(mcuUpdating || iapPullBusy() || mcuApproval!=0) return "";
    DBG.println(String("[HTTP] 尝试连接 ") + candidates[i] + ":" + String(SERVER_PORT) + " [" + tag + "]");
    if (client.connect(candidates[i].c_str(), SERVER_PORT)) {
      g_server_ip = candidates[i];   // 音频流也复用这个已验证可达的中转地址。
      DBG.println("[HTTP] 已连接 " + candidates[i]);
      return candidates[i];
    }
    client.stop();   // 失败后清理，避免影响下一次连接
    delay(0);
  }

  DBG.println(String("[HTTP] 所有服务器地址连接失败 [") + tag + "]");
  // 自动发现的地址也连不上 ⇒ 作废它，让主循环 5 秒后重新问路。
  //（PC 换了 IP / 电脑关机了 / app.py 停了 都会走到这；不作废就永远卡在旧 IP）
  if (g_server_ip.length() > 0) {
    DBG.println("[DISC] 作废已发现的 " + g_server_ip + "，稍后重新问路");
    g_server_ip = "";
  }
  return "";
}

static uint32_t controlAck = 0u;
static uint32_t controlSentAt = 0u;
static uint32_t controlSentRevision = 0u;
static uint16_t motionSequence = 0u;
static String motionState = "";
static BridgePose boardPose;
static uint32_t idleAck=0u, idleSent=0u, idleSentAt=0u;

void sendToServer() {
  // ---- 构造请求体（传感器数据来自F407，ESP本身无传感器）----
  // sensor 为空时也上报“无数据”，让云端和 AI 明确知道本轮没有传感器数据，而不是漏传。
  DynamicJsonDocument bodyDoc(2048);
  bodyDoc["token"]     = API_TOKEN;
  bodyDoc["device_id"] = DEVICE_ID;
  bodyDoc["control_ack"] = controlAck;
  bodyDoc["motion_ack"] = motionAck;
  bodyDoc["motion_state"] = motionState;
  // 继续轮询上报进度；上一段结束前，服务端暂不领取下一段时间轴。
  bodyDoc["motion_busy"] = timelineTransport.executing() || stepPlayer.active();
  JsonObject steps=bodyDoc.createNestedObject("step_playback");
  steps["protocol"]=1;steps["id"]=stepPlayer.id();steps["phase"]=stepPlayer.phase();
  steps["accepted"]=stepPlayer.accepted();steps["sent"]=stepPlayer.sent();steps["active"]=stepPlayer.active();
  JsonObject board=steps.createNestedObject("board");
  board["pos"]=stepPlayer.boardPos();board["total"]=stepPlayer.total();board["phase"]=stepPlayer.boardPhase();
  JsonObject delivery=bodyDoc.createNestedObject("motion_delivery");
  delivery["id"]=timelineTransport.id();delivery["phase"]=timelineTransport.phase();
  delivery["sequence"]=timelineTransport.sequence();delivery["attempts"]=timelineTransport.attempts();
  delivery["board_state"]=timelineTransport.board();
  JsonObject screen=bodyDoc.createNestedObject("text_transport");
  screen["phase"]=screenText.phase();screen["sequence"]=screenText.sequence();
  screen["attempts"]=screenText.attempts();
  bodyDoc["idle_ack"] = idleAck;
  if(boardPose.valid && millis()-boardPose.seen<=1500u) {
    JsonObject pose=bodyDoc.createNestedObject("board_pose");
    pose["uptime_ms"]=boardPose.uptime;pose["mask"]=boardPose.mask;
    pose["age_ms"]=(uint32_t)(millis()-boardPose.seen);
    pose["moving"]=boardPose.moving!=0u;pose["idle"]=boardPose.idle;
    pose["idle_revision"]=boardPose.revision;
    JsonArray angles=pose.createNestedArray("angles");
    for(unsigned i=0;i<3;i++)angles.add(boardPose.angle[i]);
  }
  bodyDoc["motion_cache"] = motionProfileCache.cached();
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
  bodyDoc["sensor"]    = (sensorData.length() > 0) ? sensorData : String("无数据");
  bodyDoc["user"]      = userMsg;
  if (pendingAck.length() > 0) {
    bodyDoc["ack"] = pendingAck;
    pendingAck = "";
  }
  // 只有"用户主动请求"(user非空)才让云端调AI决策；周期上报不调AI（省费用）
  bodyDoc["need_ai"] = (userMsg.length() > 0);
  String body;
  serializeJson(bodyDoc, body);
  userMsg = "";   // 清掉本次请求，避免30s周期上报重复触发AI决策

  DBG.println("\n[HTTP] 本机IP: " + WiFi.localIP().toString());
  DBG.printf("[HTTP] request body bytes=%u\n",(unsigned)body.length());

  // ---- 发送 HTTP POST（先试自动发现的 IP，再退到 SERVER_IPS[] 兜底）----
  // ★ 候选逻辑抽进了 connectAnyServer()，好让 checkForUpdate() 走**同一套**。
  WiFiClient client;
  String serverIP = connectAnyServer(client, "data");
  if (serverIP.length() == 0) return;

  client.print(String("POST ") + API_PATH + " HTTP/1.1\r\n");
  client.print("Host: " + serverIP + ":" + String(SERVER_PORT) + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: " + String(body.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  // ---- 读取响应（最多 4KB，HTTP_TIMEOUT_MS 硬超时）----
  // 关键点：ESP8266 必须定时 delay()，否则 WiFi 协议栈来不及处理收包，
  // 紧循环会一直拿不到数据；连接关闭后还要把缓冲收干净再结束。
  String response = "";
  unsigned long hardTimeout = millis() + HTTP_TIMEOUT_MS;

  // 等第一个字节
  while (client.available() == 0) {
    if (millis() > hardTimeout) {
      DBG.println("[HTTP] 等待响应超时");
      client.stop();
      return;
    }
    serviceBridge();   // 等响应期间仍响应局域网更新探测。
    if (audioBusy() || mcuUpdating || iapPullBusy() || mcuApproval!=0) { client.stop(); return; }
    delay(10);
  }

  // 收完剩余数据（连接关闭 且 缓冲清空 才算结束）
  while (client.available() || client.connected()) {
    while (client.available()) {
      response += (char)client.read();
      if (response.length() > 4096) break;
    }
    if (millis() > hardTimeout) break;
    serviceBridge();
    if (audioBusy() || mcuUpdating || iapPullBusy() || mcuApproval!=0) { client.stop(); return; }
    delay(5);
  }
  client.stop();

  // ---- 提取状态行 和 body（跳过HTTP头部）----
  int headerEnd = response.indexOf("\r\n\r\n");
  String headers  = (headerEnd >= 0) ? response.substring(0, headerEnd) : response;
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

/* ===================== 解析AI决策 / 网页命令并转发F407 ===================== */

void forwardMovesToF407(JsonVariant mvJson) {
  if (mvJson.isNull()) return;
  String frames = "";
  for (JsonObject m : mvJson.as<JsonArray>()) {
    String type = m["type"] | "";
    if (type.length() == 0) continue;
    String line = "@MOVE:" + type;
    if (type == "servo") {
      if(m["id"].isNull() || m["angle"].isNull()) continue;
      line += "," + String((int)m["id"]) + "," + String((int)m["angle"]);
      if (m.containsKey("speed")) line += "," + String((int)m["speed"]);
    } else if(type=="home") {
      line += "," + (m["id"].isNull() ? String("all") : String((int)m["id"]));
      if(m.containsKey("speed")) line+=","+String((int)m["speed"]);
    } else if (type == "gesture") {
      line += "," + String((const char*)(m["name"] | ""));
    } else if (type == "expression") {
      line += "," + String((const char*)(m["face"] | ""));
    } else {
      for (JsonPair kv : m) {
        if (String(kv.key().c_str()) == "type") continue;
        JsonVariant v = kv.value();
        if (!v.isNull() && !v.is<JsonObject>() && !v.is<JsonArray>()) {
          line += "," + v.as<String>();
        }
      }
    }
    frames += line + "\n";
  }
  if (frames.length() > 0) {
    Serial.print(frames);                 // 真正发给 F407（GPIO1 TX）
    DBG.println("[UART] -> F407 moves:");
    DBG.print(frames);
  }
}

/* 网页协议 @...# → STM32 行协议 @...\n（可多帧粘连） */
void forwardAtHashRawToF407(const String& rawIn) {
  String raw = rawIn;
  raw.trim();
  if (raw.length() == 0) return;
  // 单条库时间轴从 decision_to_raw 生成时可能没有 '#' 分隔符。
  // 这里仍必须进入二进制动作转换路径，不能误走短文本行路径。
  if (raw.startsWith("@TIMELINE:") && raw.indexOf('#') < 0) raw += "#";

  // 换行格式也必须经过文字确认通道，不能绕回无确认的Serial.print。
  if (raw.indexOf('#') < 0) {
    int start=0;
    while(start<(int)raw.length()) {
      int end=raw.indexOf('\n',start);if(end<0)end=raw.length();
      String line=raw.substring(start,end);line.trim();
      if(line.startsWith("@TEXT:"))screenText.submit(line.c_str()+6,millis());
      else if(line.length()) {Serial.print(line);Serial.print("\n");}
      start=end+1;
    }
    return;
  }

  String frames = "";
  int from = 0;
  while (from < (int)raw.length()) {
    int at = raw.indexOf('@', from);
    if (at < 0) break;
    int hash = raw.indexOf('#', at + 1);
    if (hash < 0) {
      // 缺 #：把剩余当一行发出，避免丢命令
      String rest = raw.substring(at);
      rest.trim();
      if(rest.startsWith("@TEXT:"))screenText.submit(rest.c_str()+6,millis());
      else if (rest.length()) frames += rest + "\n";
      break;
    }
    String one = raw.substring(at, hash);  // 不含 #
    one.trim();
    if(one.startsWith("@TEXT:")) {
      screenText.submit(one.c_str()+6,millis());
    } else if (one == "@TIMELINE:STOP") {
      // 阶段：保持与动作前文字帧的顺序，先完成前面的文字发送，再提交二进制任务。
      if (frames.length()) { Serial.print(frames); frames=""; }
      timelineTransport.cancel(millis());
      uint8_t stop[8]={0xA5,0x5A,0x21,0,0,0,0,0x21};
      Serial.write(stop,sizeof(stop));
    } else if (one.startsWith("@TIMELINE:")) {
      if (frames.length()) { Serial.print(frames); frames=""; }
      if (!motionSequence) motionSequence=(uint16_t)(micros()|1u);
      motionSequence++; if(!motionSequence) motionSequence++;
      if(!timelineTransport.submit(one.c_str()+10,motionSequence,timelineCommandId,millis()))
        DBG.println("[TIMELINE] invalid encoding; nothing sent");
    } else if (one.length()) frames += one + "\n";
    from = hash + 1;
  }
  if (frames.length() > 0) {
    Serial.print(frames);                 // 发给 F407
    DBG.println("[UART] -> F407 raw(@#=>\\n):");
    DBG.print(frames);
  }
}

void forwardCommandToF407(JsonVariant cmd) {
  if (cmd.isNull() || !cmd.is<JsonObject>()) return;
  DBG.println("[CMD] 网页动作命令 -> F407");

  // 本阶段隔离：只转发 raw 里的舵机帧；网页侧已过滤为 @MOVE:servo...
  const char* raw = cmd["raw"] | "";
  if (String(raw).length() > 0) {
    // 日志走 D4/GPIO2；UART0 只发协议帧，避免日志挤满 F407 接收缓冲
    DBG.println("[WEB==>] 收到网页命令 raw: " + String(raw));
    timelineCommandId=cmd["motion_id"] | "";
    forwardAtHashRawToF407(String(raw));
    timelineCommandId="";
    return;
  }

  // 兼容阶段：带类型的即时命令和 AI 动作共用同一条校验与下发路径。
  if (cmd.containsKey("moves")) {
    forwardMovesToF407(cmd["moves"]);
  }
  const char *screen=cmd["text"] | "";
  if(*screen)screenText.submit(screen,millis());
}

void parseAIResponse(const String& respBody) {
  // ---- 云端下发（OTA）的公告先单独捞一遍：只把 `iap` 这个键收进来 ----
  // 为什么不并进下面那个 respDoc(2048)：
  //   ① 不动**已经验收过**的命令通道 —— respDoc 一旦 NoMemory，连 `command` 一起丢；
  //   ② 用 Filter 过滤后，`iap` 占的内存与 AI 回复正文**互不影响**
  //      （ArduinoJson 7.4.3，Filter.hpp 已确认在库里）。
  // ⚠️ 这里只**记下任务 + 置个状态位**，绝不做阻塞的事 —— 我们还在 HTTP 响应处理里。
  {
    StaticJsonDocument<64> iapOnly;
    iapOnly["iap"] = true;                       // 只要 iap 这棵子树，别的键全部丢掉
    StaticJsonDocument<384> iapDoc;              // 够放 {job_id,name,bytes,crc32,slot_crc32,block}
    if (!deserializeJson(iapDoc, respBody, DeserializationOption::Filter(iapOnly))
        && !iapDoc["iap"].isNull()) {
      iapPullAnnounce(iapDoc["iap"]);
    }
  }

  // 解析中转站返回的统一决策 JSON（信封 + moves[]）+ 可选 command
  DynamicJsonDocument respDoc(8192);
  DeserializationError err = deserializeJson(respDoc, respBody);
  if (err || respDoc.overflowed()) {
    DBG.println("[AI] JSON解析失败: " + String(err.c_str()) + " overflow=" + String(respDoc.overflowed()));
    DBG.println("[AI] 原始内容: " + respBody);
    return;
  }

  // 先执行 command（尤其是待机让路 @TIMELINE:STOP），再进入 step_playback。
  // 否则同一响应里先启动 @STEPQ、再收到 STOP，会把刚启动的录制动作停掉。
  if (respDoc.containsKey("command")) {
    forwardCommandToF407(respDoc["command"]);
  }

  JsonObject playback=respDoc["step_playback"];
  if(!playback.isNull()) {
    const char* id=playback["id"] | "";
    if(playback["stop"] | false) stepPlayer.cancel(Serial,id);
    else if(playback["total"].is<unsigned>() && playback["total"].as<unsigned>()<=302 &&
            playback["start"].is<unsigned>() && playback["start"].as<unsigned>()<=302) {
      JsonArray batch=playback["steps"].as<JsonArray>();
      SampledStep part[12];bool valid=batch.size()<=12;
      for(unsigned i=0;valid && i<batch.size();i++) {
        JsonObject s=batch[i];valid=s["at_ms"].is<unsigned>() && s["at_ms"].as<unsigned>()<=30000;part[i].at=s["at_ms"] | 0;
        for(unsigned a=0;a<3;a++) {
          valid=valid && s["angles"][a].is<unsigned>() && s["speeds"][a].is<unsigned>() &&
            s["angles"][a].as<unsigned>()<=180 && s["speeds"][a].as<unsigned>()<=360;
          part[i].angle[a]=s["angles"][a] | 65535;part[i].speed[a]=s["speeds"][a] | 65535;
        }
      }
      if(valid && !timelineTransport.executing()) {
        bool first=strcmp(id,stepPlayer.id())!=0;
        if(stepPlayer.receive(Serial,id,playback["total"],playback["start"],part,batch.size(),millis()) && first) {
          const char *face=playback["expression"] | "";
          bool faceOK=*face && strlen(face)<=20;
          for(const char *p=face;*p;p++)if(!((*p>='a'&&*p<='z')||*p=='_'))faceOK=false;
          if(faceOK)Serial.printf("@MOVE:expression,%s\n",face);
        }
      }
    }
  }

  JsonObject controls = respDoc["controls"];
  JsonObject limits = respDoc["motion_profile"];
  uint32_t limitsRevision = limits["revision"] | 0u;
  if(limitsRevision && !audioBusy() && !iapPullBusy() && !mcuUpdating &&
      (motionSentRevision!=limitsRevision || millis()-motionSentAt>=5000u)) {
    const char *wire=limits["wire"] | "";
    const bool changed=motionProfileCache.revision()!=limitsRevision;
    uint32_t checked=0u;
    if(*wire && motionProfileValid(wire,checked) && checked==limitsRevision && motionProfileCache.accept(wire)) {
      if(changed) motionReplayRequested=true;
      replaySavedMotionProfile();
    }
  }
  uint32_t revision = controls["revision"] | 0u;
  if (revision && !audioBusy() && !iapPullBusy() && !mcuUpdating &&
      (controlSentRevision != revision || millis() - controlSentAt >= 5000u)) {
    char frame[100];
    snprintf(frame, sizeof(frame), "@CONFIG:%lu,%u,%u,%u,%u,%u,%u\n",
             (unsigned long)revision, (controls["enabled"] | true) ? 1u : 0u,
             (unsigned)(controls["minimum"] | 900u), (unsigned)(controls["margin"] | 800u),
             (unsigned)(controls["onset_ms"] | 160u), (unsigned)(controls["silence_ms"] | 800u),
             (unsigned)(controls["servo_speed"] | 90u));
    Serial.print(frame);
    if (controlSentRevision != revision) controlAck = 0u;
    controlSentRevision = revision; controlSentAt = millis();
  }

  JsonObject idle=respDoc["idle_profile"];
  uint32_t idleRevision=idle["revision"] | 0u;
  if(idleRevision && !audioBusy() && !iapPullBusy() && !mcuUpdating &&
     (idleSent!=idleRevision || (idleAck!=idleRevision && millis()-idleSentAt>=5000u))) {
    const char *wire=idle["wire"] | "";
    if(motionSendHex(Serial,wire,0x23u,++motionSequence)) {
      if(idleSent!=idleRevision)idleAck=0u;
      idleSent=idleRevision;idleSentAt=millis();
    }
  }

  const char* message = respDoc["message"] | "";
  const char* text    = respDoc["text"]    | "";
  const char* raw     = respDoc["raw"]     | "";

  DBG.println("--------------------------------------------------");
  DBG.println("[AI] 消息  : " + String(message));
  DBG.println("[AI] 屏幕文字(text): " + String(text));
  if (String(raw).length() > 0) {
    DBG.println("[AI] 原始  : " + String(raw));
  }

  // ---- AI 决策转发给 F407（类型化帧） ----
  // 云端返回 ai:false（周期上报未调AI）时不转发 AI 文字/moves，但仍可取 command
  bool needForward = true;
  if (respDoc.containsKey("ai") && respDoc["ai"] == false) needForward = false;

  const char *deviceFrames=respDoc["device_frames"] | "";
  if (needForward && *deviceFrames) {
    forwardAtHashRawToF407(String(deviceFrames));
  } else if (needForward && !respDoc.containsKey("timeline")) {
    String frames = "";
    JsonVariant mvJson = respDoc["moves"];
    if (!mvJson.isNull()) {
      forwardMovesToF407(mvJson);
    } else if (respDoc.containsKey("action")) {
      String line = "@MOVE:" + String((const char*)(respDoc["action"] | ""));
      if (respDoc.containsKey("value")) line += "," + String((int)(respDoc["value"] | 0));
      frames += line + "\n";
    }

    String screen = (String(text).length() > 0) ? String(text) : String(message);
    // ai:false 的 message「已记录」不要刷屏；上面 needForward 已挡
    if (screen.length() > 0 && !(respDoc.containsKey("ai") && respDoc["ai"] == false))
      screenText.submit(screen.c_str(),millis());
    if (frames.length() > 0) {
      Serial.print(frames);               // 发给 F407
      DBG.println("[UART] -> F407 决策帧:");
      DBG.print(frames);
    }
  } else {
    DBG.println("[UART] 未调用AI，跳过决策转发");
  }

  // ---- 网页遥控命令已在本函数开头先处理（STOP 必须先于 step_playback）----
}

/* ===================== 接收 F407 数据 ===================== */

void handleF407Text(const String& line) {
    /* USB 维护时先断开 PC6/PC7，串口监视器用 921600 发送 @ESP:DIAG。
     * 平常不向 MCU 串口打印调试信息，防止日志污染协议。不会输出密码。 */
    if(line=="@ESP:DIAG") {
      Serial.printf("@ESP:STATUS,version=%s,build=%s,wifi=%d,ip=%s,heap=%u,pong=%s,server=%s,ota=8080\r\n",
        FW_VERSION,ESP_BUILD_ID,(int)WiFi.status(),WiFi.localIP().toString().c_str(),ESP.getFreeHeap(),
        lastPongAt && millis()-lastPongAt<8000u?"OK":"MISSING",g_server_ip.c_str());
      return;
    }
    if(camera.text(line))return;
    if(line.startsWith("@POSE:")) {
      boardPose.accept(line.c_str(),millis());
      return;
    }
    if(line.startsWith("@IDLE:OK,")) {
      idleAck=(uint32_t)strtoul(line.c_str()+9,nullptr,10);
      return;
    }
    if(line.startsWith("@LIMITS:OK,")) {
      motionAck=(uint32_t)strtoul(line.c_str()+11,nullptr,10);
    } else if(line.startsWith("@STEPQ:")) {
      /* F407 本地队列进度：accepted / running / done / stopped / error。 */
      stepPlayer.onBoardLine(line.c_str()+7);
      ackReadyNow=true;
    } else if(line.startsWith("@TIMELINE:")) {
      motionState=line.substring(10);
      timelineTransport.ack(motionState.c_str(), millis());
      pendingAck="TIMELINE:"+motionState;
      ackReadyNow=true;
    } else if (line == "@CONFIG:NEED") {
      controlAck = controlSentRevision = 0u;
      idleAck=idleSent=0u;boardPose.valid=false;
      motionAck=motionSentRevision=0u;
      motionReplayRequested=true;
    } else if (line.startsWith("@CONFIG:OK,")) {
      controlAck = (uint32_t)strtoul(line.c_str() + 11, nullptr, 10);
    } else if (line.startsWith("@IAP:WAIT,")) {
      mcuApproval=1;
      mcuMessage="Waiting for F407 K-UP confirmation";
      DBG.println("[IAP] F407 asks for physical K-UP confirmation");
    } else if (line.startsWith("@IAP:OK,")) {
      mcuApproval=2;
      mcuMessage="F407 user confirmation received";
      DBG.println("[IAP] F407 user confirmation received");
    } else if (line.startsWith("@IAP:NO,")) {
      mcuApproval=0;
      mcuMessage="F407 OTA confirmation expired or was cancelled";
      DBG.println("[IAP] F407 OTA confirmation expired/cancelled");
    } else if (line.startsWith("@IAP:ERR,")) {
      mcuApproval=0;
      mcuMessage="F407 rejected OTA request";
      DBG.println("[IAP] F407 rejected OTA request");
    } else
    if (line.startsWith("@SENSOR:")) {
      sensorData = line.substring(8);
      DBG.println("[F407] 传感器数据: " + sensorData);
    } else if (line.startsWith("@USER:")) {
      userMsg = line.substring(6);
      DBG.println("[F407] 用户消息: " + userMsg);
    } else if (line.startsWith("@ALERT:")) {
      // 异常/事件帧（预留）：F407 传感器检测到异常 → 立即上报云端请求 AI 决策
      userMsg = line.substring(7);
      DBG.println("[F407] 异常事件: " + userMsg);
      ackReadyNow = true;   // 非阻塞：主循环马上上报，不再在本函数里卡 HTTP
    } else if (line == "@PONG") {
      lastPongAt=millis();
      DBG.println("[LINK] F407 @PONG: UART round trip OK");
    } else if (line.startsWith("@ACK:")) {
      pendingAck = line.substring(5);
      DBG.println("[F407] ACK: " + pendingAck);
      ackReadyNow = true;   // 非阻塞：主循环马上上报，不再在本函数里卡 HTTP
    } else {
      DBG.printf("[F407] ignored non-protocol line (%u bytes)\n",(unsigned)line.length());
    }
}

void handleF407Receive() {
  if (Serial.hasOverrun() || Serial.hasRxError()) {
    bridgeWire.reset();
    audioTransport.uartOverrun();
    audioBridge.badFrame();
    DBG.println("[LINK] UART RX error/overflow: retry expected audio frame");
  }
  while (Serial.available()) {
    int result=bridgeWire.feed((uint8_t)Serial.read(),millis());
    if (result==1) { String line(bridgeWire.text); line.trim(); handleF407Text(line); }
    else if (result==2) {
      if (bridgeWire.type==1) {
        DBG.printf("[AUDIO] START frame from F407: seq=%u len=%u\n",
                   (unsigned)bridgeWire.seq, (unsigned)bridgeWire.len);
      }
      if(bridgeWire.type==0x25u)
        screenText.ack(bridgeWire.seq,bridgeWire.payload,bridgeWire.len);
      else if (bridgeWire.type>=0x30 && bridgeWire.type<=0x32)
        camera.frame(bridgeWire.type,bridgeWire.seq,bridgeWire.payload,bridgeWire.len);
      else if (bridgeWire.type>=0x10 && bridgeWire.type<=0x1F)
        mcuOnFrame(bridgeWire.type,bridgeWire.seq,bridgeWire.payload,bridgeWire.len);
      else if ((cameraBusy() || mcuUpdating || iapPullBusy() || mcuApproval!=0) && bridgeWire.type==1) { uint8_t no=2; mcuFrame(4,0,&no,1); }
      else audioBridge.frame(bridgeWire.type,bridgeWire.seq,
                              bridgeWire.payload,bridgeWire.len,millis());
    }
    else if (result<0) audioBridge.badFrame();
  }
}


