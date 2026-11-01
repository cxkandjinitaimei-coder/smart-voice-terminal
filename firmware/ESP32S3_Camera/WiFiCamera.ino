/*
 * 文件用途：ESP32-S3 独立无线相机节点。相机采集 RGB565，经 JPEG 编码后
 * 通过 Wi-Fi 上传 Flask；F407 和 ESP8266 不搬运图像数据。
 *
 * 使用：首次烧录合并 BIN 到 0x0；network_config.h 只提供编译期默认配置，
 * 上电会先读取 NVS，失败时启动 S3-Camera-xxxx 配置热点并在 192.168.4.1 保存。
 * 网页人员页通过租约触发单拍、定时或连续上传，电脑端再做人脸识别和三轴计算。
 * 输入是 HTTP/UDP 任务公告，输出是带设备 ID、任务票据和 JPEG 的 HTTP 请求。
 *
 * 时序阶段：启动读配置 -> STA 连接/AP 回退 -> UDP 发现 -> 领取租约 -> 采集
 * -> JPEG 编码 -> 上传/重试 -> 释放帧缓冲。主循环串行拥有相机，连续模式必须
 * 等当前帧应答后再取下一帧；更新、配置、预览和上传共享同一互斥边界。
 * 资源限制：不缓存过期视频帧；失败只记录状态并重新申请租约，避免 PSRAM 被
 * 无限堆积。完整分区、烧录和配置说明见交付文档，版本只在 FW_VERSION 定义。
 */
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "esp_camera.h"
#include "camera_jpeg.h"
#include "camera_board.h"
#include "network_config.h"
#include "camera_config_defaults.h"
#include "camera_discovery_protocol.h"
#include "esp_system.h"

static const char *FW_VERSION = "CAM-V2026.10.29-04";
WebServer portal(80);
Preferences prefs;
String wifiName, wifiPassword, serverUrl, apiToken, robotId, caCert;
String lastState = "尚未配置", apName;
// 等待间隔会更新 lastState；故障单独保留到下一次上传成功，便于不用串口排查。
String lastError;
int lastHttp = 0;
uint16_t sensorPid = 0;
size_t lastJpegBytes = 0;
uint32_t lastEncodeMs = 0, lastUploadMs = 0;
bool cameraReady = false, rebootPending = false;
bool updateStarted = false, updateComplete = false;
uint32_t nextPoll = 0, lastLease = 0, reconnectAt = 0;
uint32_t uploaded = 0, failed = 0;
WiFiUDP discoveryUdp;
bool discoveryBound=false, serverDiscovered=false;
IPAddress discoveryLocal;
uint32_t nextDiscovery=0;
uint8_t serverFailures=0;
String liveTicket;
uint32_t liveSequence=0, nextFrame=0, liveUntil=0, liveInterval=500, liveUploaded=0;

void stopLive() {liveTicket=""; liveSequence=0;}

// 联网后非阻塞问路。与ESP8266使用相同请求/应答，不改变网页5001服务。
// 连接失效连续3次重新发现；切换服务器前丢掉旧连续租约，不把照片发给新服务的旧任务。
void serviceServerDiscovery() {
  if(!CAMERA_LAN_DISCOVERY || serverUrl.startsWith("https://") || updateStarted)return;
  if(WiFi.status()!=WL_CONNECTED) {
    if(discoveryBound)discoveryUdp.stop();
    discoveryBound=false;serverDiscovered=false;nextDiscovery=0;return;
  }
  IPAddress local=WiFi.localIP(),mask=WiFi.subnetMask();
  if(!discoveryBound || local!=discoveryLocal) {
    discoveryUdp.stop();discoveryBound=discoveryUdp.begin(5002);
    discoveryLocal=local;serverDiscovered=false;serverFailures=0;nextDiscovery=0;
  }
  if(!discoveryBound)return;
  if((!serverDiscovered || serverFailures>=3) && (int32_t)(millis()-nextDiscovery)>=0) {
    const char *magic="ARDP_DISCOVER";
    IPAddress broadcast=local;
    for(unsigned i=0;i<4;++i)broadcast[i]=local[i] | uint8_t(~mask[i]);
    discoveryUdp.beginPacket(IPAddress(255,255,255,255),5001);
    discoveryUdp.write(reinterpret_cast<const uint8_t*>(magic),std::strlen(magic));discoveryUdp.endPacket();
    discoveryUdp.beginPacket(broadcast,5001);
    discoveryUdp.write(reinterpret_cast<const uint8_t*>(magic),std::strlen(magic));discoveryUdp.endPacket();
    nextDiscovery=millis()+5000;
  }
  // 每轮最多检查4包，避免广播噪声挤占相机/配置页的主循环。
  for(unsigned count=0;count<4;++count) {
    int size=discoveryUdp.parsePacket();if(size<=0)break;
    IPAddress remote=discoveryUdp.remoteIP();
    char data[80],url[48];int got=discoveryUdp.read(data,sizeof(data));
    uint8_t sender[4],own[4],netmask[4];
    for(unsigned i=0;i<4;++i){sender[i]=remote[i];own[i]=local[i];netmask[i]=mask[i];}
    if(got!=size || !cameraParseDiscovery(data,got,sender,own,netmask,url,sizeof(url)))continue;
    if(serverDiscovered && serverFailures<3 && serverUrl!=url)continue;
    if(serverUrl!=url){stopLive();serverUrl=url;nextPoll=0;}
    if(!serverDiscovered)Serial.printf("[DISC] server=%s (UDP5001)\n",serverUrl.c_str());
    serverDiscovered=true;
  }
}

// 配置入口只允许从本机配置热点进入；LAN 状态页不会泄露 Wi-Fi 密码/API 口令。
bool setupClient() {
  IPAddress remote = portal.client().remoteIP(), local = WiFi.softAPIP();
  return remote[0] == local[0] && remote[1] == local[1] && remote[2] == local[2];
}
String escaped(String value) {
  value.replace("&", "&amp;"); value.replace("<", "&lt;");
  value.replace("\"", "&quot;"); value.replace("'", "&#39;");
  return value;
}
bool validId(const String &id) {
  if (!id.length() || id.length() > 64) return false;
  for (size_t i=0; i<id.length(); ++i)
    if (!isalnum((unsigned char)id[i]) && id[i]!='-' && id[i]!='_') return false;
  return true;
}
void stopCamera() {
  if (cameraReady) { esp_camera_deinit(); cameraReady = false; }
}
bool startCamera() {
  if (cameraReady) return true;
  camera_config_t c = {}; // 先清零，避免 SDK 新增字段带入栈垃圾。
  c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0;
  c.pin_d0=CAM_D0; c.pin_d1=CAM_D1; c.pin_d2=CAM_D2; c.pin_d3=CAM_D3;
  c.pin_d4=CAM_D4; c.pin_d5=CAM_D5; c.pin_d6=CAM_D6; c.pin_d7=CAM_D7;
  c.pin_xclk=CAM_XCLK; c.pin_pclk=CAM_PCLK; c.pin_vsync=CAM_VSYNC; c.pin_href=CAM_HREF;
  c.pin_sccb_sda=CAM_SDA; c.pin_sccb_scl=CAM_SCL; c.pin_pwdn=-1; c.pin_reset=-1;
  // 厂家原图传例程采集 RGB565，再软件编码 JPEG。不能假定传感器支持直接 JPEG。
  c.xclk_freq_hz=15000000; c.pixel_format=PIXFORMAT_RGB565;
  c.frame_size=psramFound()?FRAMESIZE_VGA:FRAMESIZE_QVGA;
  // 单张租约使用单帧缓冲，减少 RGB565 连续 DMA 与 Wi-Fi 竞争带宽。
  c.jpeg_quality=12; c.fb_count=1;
  c.fb_location=psramFound()?CAMERA_FB_IN_PSRAM:CAMERA_FB_IN_DRAM;
  c.grab_mode=CAMERA_GRAB_WHEN_EMPTY;
  esp_err_t err=esp_camera_init(&c);
  if(err!=ESP_OK) {
    lastError=lastState="相机初始化失败 0x"+String(err,HEX);
    Serial.printf("[CAM] init RGB565 failed: 0x%x (%s)\n",err,esp_err_to_name(err));
    return false;
  }
  cameraReady=true;
  sensor_t *sensor=esp_camera_sensor_get();
  sensorPid=sensor?sensor->id.PID:0;
  Serial.printf("[CAM] init OK sensor PID=0x%04x RGB565 %s fb=1 XCLK=15MHz\n",
                sensorPid,psramFound()?"640x480":"320x240");
  // 首次初始化等待自动曝光，丢弃启动帧；后续连续采集不反复初始化。
  delay(250);
  for(int i=0;i<2;++i) { camera_fb_t *fb=esp_camera_fb_get(); if(fb) esp_camera_fb_return(fb); }
  return true;
}

// 每次请求新建有界连接；HTTPS 必须在配置页提供可信根证书，不跳过证书验证。
int postRequest(const String &path, const uint8_t *data, size_t length,
                const String &type, String &response, const String &ticket="", const String &sequence="") {
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  bool tls=serverUrl.startsWith("https://");
  if(tls && caCert.isEmpty()) { lastState="HTTPS 缺少根证书"; return -100; }
  if(tls) { secure.setCACert(caCert.c_str()); secure.setHandshakeTimeout(5); }
  WiFiClient &client=tls?static_cast<WiFiClient&>(secure):plain;
  http.setConnectTimeout(2500); http.setTimeout(5000); http.setReuse(false);
  if(!http.begin(client,serverUrl+path)) {
    if(serverFailures<255)++serverFailures;
    return -101;
  }
  http.addHeader("Authorization","Bearer "+apiToken);
  http.addHeader("Content-Type",type);
  if(ticket.length()) http.addHeader("X-Capture-Ticket",ticket);
  if(sequence.length())http.addHeader("X-Frame-Sequence",sequence);
  int code=http.POST(const_cast<uint8_t*>(data),length);
  if(code>0 && code<500 && code!=401 && code!=403)serverFailures=0;
  else if(serverFailures<255)++serverFailures;
  // 仅解析有界响应，错误反向代理的大页面也不会占满相机内存。
  response="";
  if(code>0 && http.getSize()>=0 && http.getSize()<=8192) response=http.getString();
  http.end();
  return code;
}
int jsonPost(const String &path, const String &body, String &response) {
  return postRequest(path,(const uint8_t*)body.c_str(),body.length(),"application/json",response);
}
void reportFailure(const String &job,const String &ticket,const String &reason) {
  lastError=reason;
  Serial.printf("[CAM] failed: %s\n",reason.c_str());
  DynamicJsonDocument report(1024); report["job_id"]=job; report["ticket"]=ticket; report["error"]=reason;
  String body,ignored; serializeJson(report,body);
  jsonPost("/api/camera/device/report?device_id="+robotId,body,ignored);
}
void pollCamera() {
  String response;
  // 阶段一：先领取一次性租约；未启用、音频/OTA忙、上一张未处理完时均不会拍照。
  int code=jsonPost("/api/camera/device/poll?device_id="+robotId,
    "{\"ready\":true,\"protocol_version\":1,\"camera_type\":\"s3-wifi\",\"continuous_capable\":true}",response);
  lastHttp=code;
  if(code!=200) { stopLive(); lastError=lastState="轮询失败 HTTP "+String(code); ++failed; nextPoll=millis()+3000; return; }
  DynamicJsonDocument doc(4096);
  if(deserializeJson(doc,response)) {stopLive();lastError=lastState="服务器响应格式错误"; return;}
  if(!doc["accepted"].as<bool>()) {
    stopLive();
    lastState=doc["reason"] | "等待拍照请求";
    return;
  }
  String job=doc["job_id"] | "", ticket=doc["ticket"] | "";
  if(doc["mode"] == "continuous") {
    if(ticket.length()<16 || ticket.length()>100) {stopLive();lastError=lastState="连续租约无效";return;}
    if(liveTicket!=ticket){liveTicket=ticket;liveSequence=0;nextFrame=millis();}
    liveInterval=constrain(doc["frame_interval_ms"] | 500,200,1000);
    liveUntil=millis()+10000;lastLease=millis();lastState="连续上传 / 电脑持续识别";
    return;
  }
  stopLive();
  if(job.length()!=32 || ticket.length()<16 || ticket.length()>100) { lastState="拍照租约无效"; return; }
  lastLease=millis();
  if(!startCamera()) { ++failed; reportFailure(job,ticket,lastState); return; }
  // 阶段二：丢掉排队旧帧，再采集 RGB565；转成 JPEG 后立即归还原始帧。
  camera_fb_t *old=esp_camera_fb_get(); if(old) esp_camera_fb_return(old);
  CameraJpeg jpeg;
  uint32_t encodeStart=millis();
  if(!jpeg.capture()) {
    lastState=jpeg.error(); ++failed; reportFailure(job,ticket,lastState); return;
  }
  lastEncodeMs=millis()-encodeStart; lastJpegBytes=jpeg.size();
  Serial.printf("[CAM] RGB565 %ux%u -> JPEG %u bytes, capture+encode=%lu ms\n",
                (unsigned)jpeg.width(),(unsigned)jpeg.height(),(unsigned)jpeg.size(),
                (unsigned long)lastEncodeMs);
  String path="/api/camera/device/upload?device_id="+robotId+"&job_id="+job;
  // 阶段三：整张 JPEG 直接上传；应答丢失时用相同租约/相同字节重试一次（服务端去重）。
  uint32_t uploadStart=millis();
  code=postRequest(path,jpeg.data(),jpeg.size(),"image/jpeg",response,ticket);
  if(code<0 || code>=500) code=postRequest(path,jpeg.data(),jpeg.size(),"image/jpeg",response,ticket);
  lastHttp=code; lastUploadMs=millis()-uploadStart;
  DynamicJsonDocument ack(1024);
  bool ok=(code==200 || code==202) && !deserializeJson(ack,response) && ack["ok"].as<bool>();
  Serial.printf("[CAM] upload HTTP=%d ok=%d time=%lu ms\n",code,ok,(unsigned long)lastUploadMs);
  if(ok) { ++uploaded; lastError=""; lastState="上传成功，等待本地识别"; }
  else { ++failed; lastState="上传失败 HTTP "+String(code); reportFailure(job,ticket,lastState); }
}

// 持续 JPEG 序列，不依赖逐张任务/间隔；相机帧只由主循环这一处使用。
void sendLiveFrame() {
  lastLease=millis();
  if(!startCamera()){++failed;stopLive();nextPoll=millis()+3000;return;}
  camera_fb_t *old=esp_camera_fb_get();if(old)esp_camera_fb_return(old);
  CameraJpeg jpeg;uint32_t started=millis();
  if(!jpeg.capture()){lastError=lastState=jpeg.error();++failed;stopLive();return;}
  lastEncodeMs=millis()-started;lastJpegBytes=jpeg.size();
  String response;
  // 帧序号属于当前会话；HTTP失败就丢弃本帧并重新申请，不重试旧视频帧。
  String path="/api/camera/device/live-frame?device_id="+robotId;
  uint32_t uploadStart=millis();
  int code=postRequest(path,jpeg.data(),jpeg.size(),"image/jpeg",response,liveTicket,String(++liveSequence));
  lastUploadMs=millis()-uploadStart;lastHttp=code;
  DynamicJsonDocument ack(1024);
  if(code==202 && !deserializeJson(ack,response) && ack["ok"].as<bool>()) {
    ++liveUploaded;++uploaded;lastError="";lastState="连续上传 / 电脑持续识别";
    liveInterval=constrain(ack["frame_interval_ms"] | 500,200,1000);
  } else {++failed;lastError=lastState="连续上传中断 HTTP "+String(code);stopLive();nextPoll=millis()+1000;}
  nextFrame=millis()+liveInterval; // 耗时叠加，绝不为追帧率突发补发。
}

void setupPortal() {
  portal.on("/status",HTTP_GET,[](){
    DynamicJsonDocument d(2048); d["version"]=FW_VERSION; d["wifi"]=WiFi.status()==WL_CONNECTED;
    d["ip"]=WiFi.localIP().toString(); d["camera"]=cameraReady; d["state"]=lastState;
    d["uploaded"]=uploaded; d["failed"]=failed; d["psram"]=psramFound();
    // 配置地址不含账号、口令或查询串；只显示访问目标，绝不回传 token/密码。
    d["server"]=serverUrl; d["pixel_format"]="RGB565"; d["sensor_pid"]=sensorPid;
    d["server_source"]=serverDiscovered?"udp":"configured";
    d["lan_discovery"]=bool(CAMERA_LAN_DISCOVERY) && !serverUrl.startsWith("https://");
    d["server_failures"]=serverFailures;
    d["last_error"]=lastError; d["last_http"]=lastHttp; d["jpeg_bytes"]=lastJpegBytes;
    d["capture_encode_ms"]=lastEncodeMs; d["upload_ms"]=lastUploadMs;
    d["continuous"]=liveTicket.length()>0;d["live_uploaded"]=liveUploaded;
    String out;serializeJson(d,out);portal.send(200,"application/json",out);
  });
  portal.on("/",HTTP_GET,[](){
    if(!setupClient()){portal.send(200,"text/plain; charset=utf-8","相机状态请访问 /status；电脑网页人员相机页可开启连续上传识别。修改配置请连接相机热点。");return;}
    String page="<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>S3 相机配置</title><style>body{font:16px sans-serif;max-width:620px;margin:30px auto;padding:15px;background:#152424;color:#eef}input,textarea,button{display:block;width:95%;padding:12px;margin:8px 0 18px;border-radius:8px}a{color:#8fd}</style><h1>S3 Wi-Fi 相机</h1><p>";
    page+=FW_VERSION;
    page+="</p><h2>本地连续预览</h2><p>仅查看相机画面；人脸识别在电脑网页开启“连续上传识别”。关闭此页只停止本地预览。</p><button onclick=\"preview=!preview;this.textContent=preview?'停止本地预览':'开始本地预览'\">开始本地预览</button><img id='preview' style='width:100%;background:#000'><p id='preview-error'></p><script>let preview=false;async function tick(){if(preview){try{const r=await fetch('/preview.jpg?t='+Date.now());if(!r.ok)throw Error(await r.text());const u=URL.createObjectURL(await r.blob()),im=document.getElementById('preview');if(im.dataset.u)URL.revokeObjectURL(im.dataset.u);im.src=u;im.dataset.u=u;document.getElementById('preview-error').textContent='';}catch(e){document.getElementById('preview-error').textContent=e.message;}}setTimeout(tick,350);}tick();</script><p>";
    page+="</p><a href='/status'>查看状态</a><form method='post' action='/save'>Wi-Fi 名称<input name='ssid' maxlength='32' required value='"+escaped(wifiName)+"'>Wi-Fi 密码（留空保留）<input type='password' name='password' maxlength='63'>服务器地址（电脑局域网 IP，不能填127.0.0.1）<input name='server' required placeholder='http://192.168.1.100:5000' value='"+escaped(serverUrl)+"'>网页登录口令/API Token（留空保留）<input name='token' type='password' maxlength='512'>机器人设备 ID<input name='device' value='"+escaped(robotId)+"' required>HTTPS 根证书 PEM（使用 HTTP 时留空）<textarea name='ca' rows='3' maxlength='6000'></textarea><button>保存并重启</button></form><hr><p>相机应用升级：只选本包 S3 app.bin，禁止选择 F407/ESP8266/合并 BIN。</p><form method='post' action='/update' enctype='multipart/form-data'><input type='file' name='firmware' accept='.bin' required><button>升级 S3 应用</button></form>";
    portal.sendHeader("Cache-Control","no-store");portal.send(200,"text/html; charset=utf-8",page);
  });
  portal.on("/preview.jpg",HTTP_GET,[](){
    if(!setupClient()){portal.send(403,"text/plain","Connect setup AP first");return;}
    if(updateStarted){portal.send(409,"text/plain","Updating");return;}
    // 与上传共用同一主循环；单个预览请求结束后释放JPEG，不额外常驻一份大缓冲。
    lastLease=millis();
    if(!startCamera()){portal.send(503,"text/plain; charset=utf-8",lastError);return;}
    CameraJpeg jpeg;
    if(!jpeg.capture()){lastError=jpeg.error();portal.send(503,"text/plain; charset=utf-8",lastError);return;}
    portal.sendHeader("Cache-Control","no-store");
    portal.setContentLength(jpeg.size());portal.send(200,"image/jpeg","");
    WiFiClient client=portal.client();client.setTimeout(2000);
    size_t sent=0;uint32_t deadline=millis()+3000;
    while(sent<jpeg.size() && client.connected() && (int32_t)(millis()-deadline)<0){
      size_t count=client.write(jpeg.data()+sent,min((size_t)1460,jpeg.size()-sent));
      if(!count)break;sent+=count;delay(1);
    }
    if(sent!=jpeg.size())client.stop();
  });
  portal.on("/save",HTTP_POST,[](){
    if(!setupClient()){portal.send(403,"text/plain","Connect setup AP first");return;}
    String ssid=portal.arg("ssid"),url=portal.arg("server"),id=portal.arg("device");url.trim();
    while(url.endsWith("/"))url.remove(url.length()-1);
    if(ssid.isEmpty() || ssid.length()>32 || portal.arg("password").length()>63 || !validId(id) ||
       url.length()>200 || (!url.startsWith("http://") && !url.startsWith("https://")) ||
       url.indexOf('?',0)>=0 || url.indexOf('#',0)>=0 || url.indexOf('@',0)>=0 ||
       url.indexOf("127.0.0.1")>=0 || url.indexOf("localhost")>=0 || portal.arg("token").length()>512 ||
       portal.arg("ca").length()>6000) {portal.send(400,"text/plain; charset=utf-8","配置无效，请核对服务器地址、设备ID及长度");return;}
    String token=portal.arg("token").length()?portal.arg("token"):apiToken;
    String cert=portal.arg("ca").length()?portal.arg("ca"):caCert;
    if(token.isEmpty() || (url.startsWith("https://") && cert.isEmpty())) {portal.send(400,"text/plain; charset=utf-8","缺少口令或 HTTPS 根证书");return;}
    prefs.putString("ssid",ssid);prefs.putString("server",url);prefs.putString("device",id);
    if(portal.arg("password").length())prefs.putString("password",portal.arg("password"));
    prefs.putString("token",token);prefs.putString("ca",cert);
    portal.send(200,"text/plain; charset=utf-8","保存成功，正在重启。网页人员页选择 ESP32-S3 Wi-Fi 后先测试拍一张。");rebootPending=true;
  });
  portal.on("/update",HTTP_POST,[](){
    if(!setupClient()){portal.send(403,"text/plain","Connect setup AP first");return;}
    bool ok=updateStarted && updateComplete && !Update.hasError();
    portal.send(ok?200:400,"text/plain",ok?"OK, rebooting":"Update failed: complete app BIN required");
    rebootPending=ok;
    updateStarted=false;updateComplete=false;
  },[](){
    if(!setupClient())return;
    HTTPUpload &u=portal.upload();
    if(u.status==UPLOAD_FILE_START){
      stopLive();stopCamera();updateComplete=false;updateStarted=Update.begin(UPDATE_SIZE_UNKNOWN);
    }
    else if(u.status==UPLOAD_FILE_WRITE){
      if(updateStarted && !Update.hasError() && Update.write(u.buf,u.currentSize)!=u.currentSize)Update.abort();
    }
    else if(u.status==UPLOAD_FILE_END){
      updateComplete=updateStarted && u.totalSize>0 && Update.end(true);
    }
    else if(u.status==UPLOAD_FILE_ABORTED){Update.abort();updateStarted=false;updateComplete=false;}
  });
  portal.begin();
}
void setup() {
  Serial.begin(115200);
  Serial.printf("\n[BOOT] %s reset_reason=%d; entered application\n",FW_VERSION,(int)esp_reset_reason());
  bool nvsOk=prefs.begin("wifi-camera",false);
  Serial.printf("[BOOT] config storage=%s; loading defaults/overrides\n",nvsOk?"OK":"FAILED");
  // 新私用配置批次只应用一次，修正旧地址；后续用户在配网页的修改仍优先。
  // 写入失败时本次先用已编译配置，下次再重试；公开空模板不覆盖原配置。
  CameraDefaultsResult applied = nvsOk ? cameraApplyBundledConfig(prefs, CAMERA_CONFIG_REVISION,
      CAMERA_WIFI_SSID, CAMERA_WIFI_PASSWORD, CAMERA_SERVER_URL, CAMERA_API_TOKEN, CAMERA_ROBOT_ID)
      : CAMERA_DEFAULTS_NONE;
  bool useSaved = nvsOk && applied == CAMERA_DEFAULTS_NONE;
  if (applied != CAMERA_DEFAULTS_NONE)
    Serial.printf("[BOOT] bundled network config %s; credentials hidden\n",
        applied == CAMERA_DEFAULTS_APPLIED ? "applied once" : "active, persistence retry pending");
  wifiName=useSaved?prefs.getString("ssid",CAMERA_WIFI_SSID):String(CAMERA_WIFI_SSID);
  wifiPassword=useSaved?prefs.getString("password",CAMERA_WIFI_PASSWORD):String(CAMERA_WIFI_PASSWORD);
  serverUrl=useSaved?prefs.getString("server",CAMERA_SERVER_URL):String(CAMERA_SERVER_URL);
  apiToken=useSaved?prefs.getString("token",CAMERA_API_TOKEN):String(CAMERA_API_TOKEN);
  robotId=useSaved?prefs.getString("device",CAMERA_ROBOT_ID):String(CAMERA_ROBOT_ID);
  caCert=nvsOk?prefs.getString("ca",""):String("");
  Serial.printf("[BOOT] network configured=%s, server configured=%s\n",
                wifiName.length()?"yes":"no",serverUrl.length()?"yes":"no");
  WiFi.mode(WIFI_AP_STA);WiFi.setSleep(false);WiFi.setAutoReconnect(true);
  apName="S3-Camera-"+String((uint32_t)ESP.getEfuseMac()&0xffff,HEX);
  bool apOk=WiFi.softAP(apName.c_str(),"CHANGE_ME_AP_PASSWORD");
  Serial.printf("[WIFI] fallback AP=%s name=%s\n",apOk?"OK":"FAILED",apName.c_str());
  if(wifiName.length())WiFi.begin(wifiName.c_str(),wifiPassword.c_str());
  configTime(0,0,"pool.ntp.org","time.windows.com");setupPortal();
  Serial.printf("%s | setup AP %s / 192.168.4.1\n",FW_VERSION,apName.c_str());
}
void loop() {
  portal.handleClient();
  if(rebootPending){delay(200);ESP.restart();}
  serviceServerDiscovery();
  uint32_t now=millis();
  // 只在联网状态改变时报告；看到 STA connected 和 IP 才证明已连上目标热点。
  static int reportedWifi=-1;
  int wifiStatus=(int)WiFi.status();
  if(wifiStatus!=reportedWifi){
    reportedWifi=wifiStatus;
    if(wifiStatus==WL_CONNECTED)Serial.printf("[WIFI] STA connected, IP=%s\n",WiFi.localIP().toString().c_str());
    else Serial.printf("[WIFI] STA status=%d, waiting/reconnecting\n",wifiStatus);
  }
  if(WiFi.status()!=WL_CONNECTED){
    stopLive();
    lastState="等待 Wi-Fi；可连接相机热点修改配置";
    if(wifiName.length() && now-reconnectAt>=15000){reconnectAt=now;WiFi.reconnect();}
  } else if(!updateStarted && serverUrl.length() && apiToken.length() && (int32_t)(now-nextPoll)>=0){
    nextPoll=now+(liveTicket.length()?2000:1000);pollCamera();
  }
  if(liveTicket.length() && (int32_t)(millis()-liveUntil)>=0)stopLive();
  if(!updateStarted && liveTicket.length() && (int32_t)(millis()-nextFrame)>=0)sendLiveFrame();
  if(cameraReady && millis()-lastLease>15000)stopCamera(); // 无采集租约后停驱动；非物理断电。
  delay(2);
}

