#ifndef BRIDGE_IAP_ESP_H
#define BRIDGE_IAP_ESP_H
#include <ESP8266WebServer.h>

/* F407局域网OTA网关：request等待实体确认，update传镜像，status取真实CRC。
 * 安装后F407自行复位，ESP必须保持服务；不能在COMMIT后立即放开录音/拍照。
 * 重启等待与刷写错误分开处理：探测未应答可以重试，不能污染后续查询。
 */

void handleF407Receive();
static ESP8266WebServer mcuUpdateServer(8080);
static bool mcuUpdating=false, mcuFailed=false, mcuCommitted=false;
static uint8_t mcuState=255, mcuError=0, mcuChunk[128];
static uint16_t mcuAck=65535, mcuSeq=0, mcuFill=0;
static uint32_t mcuStateSerial=0, mcuGot=0, mcuTotal=0, mcuBase=0, mcuCrc=0;
static uint32_t mcuCapacity=98304u, mcuLayout=0u;
static uint32_t mcuRequestTotal=0, mcuRequestCrc=0;
/* 0=none, 1=F407 screen is waiting for K-UP, 2=physical confirmation received */
static uint8_t mcuApproval=0;
static String mcuMessage="No update requested";
/* 局域网调用者仍负责最终PASS；ESP只保留业务互斥直到拿到匹配证据。
 * 5秒应答宽限覆盖一次HTTP丢包；180秒上限防止网页关闭后永久锁住录音。 */
static bool mcuVerifying=false, mcuVerifyMatched=false, mcuUploadRefused=false;
static uint32_t mcuVerifyStarted=0, mcuVerifyMatchedAt=0, mcuExpectedSlotCrc=0;
static const uint32_t MCU_VERIFY_LIMIT_MS=180000u, MCU_VERIFY_GRACE_MS=5000u;
static uint32_t iapRead32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
static void iapWrite32(uint8_t *p,uint32_t x) {
  for(unsigned i=0;i<4;i++) p[i]=uint8_t(x>>(8*i));
}
static void mcuFrame(uint8_t type,uint16_t seq,const uint8_t *p,uint16_t n) {
  if(type==0x13) DBG.println("[IAP] TX META");
  else if(type==0x15 && n && p[0]==3) DBG.println("[IAP] TX CTRL END");
  else if(type==0x15 && n && p[0]==4) DBG.println("[IAP] TX CTRL COMMIT");
  else if(type==0x15 && n && p[0]==5) DBG.println("[IAP] TX CTRL ABORT");
  uint8_t hdr[]={0xA5,0x5A,type,uint8_t(seq),uint8_t(seq>>8),uint8_t(n),uint8_t(n>>8)};
  uint8_t sum=0; for(unsigned i=2;i<7;i++) sum^=hdr[i];
  for(unsigned i=0;i<n;i++) sum^=p[i];
  Serial.write(hdr,sizeof(hdr)); if(n) Serial.write(p,n); Serial.write(sum);
}
static void mcuOnFrame(uint8_t type,uint16_t seq,const uint8_t *p,uint16_t n) {
  if(type==0x11 && !n) mcuAck=seq;
  if(type==0x12 && n) { mcuError=p[0]; mcuFailed=true; }
  if(type==0x14 && (n==17 || n==25)) {
    mcuState=p[0]; mcuGot=iapRead32(p+1); mcuBase=iapRead32(p+9);
    mcuCrc=iapRead32(p+13);
    mcuCapacity=n==25 ? iapRead32(p+17) : 98304u;
    mcuLayout=n==25 ? iapRead32(p+21) : 0u; mcuStateSerial++;
  }
}
static bool mcuWait(uint8_t state,uint32_t serial,uint32_t timeout) {
  uint32_t start=millis();
  while(uint32_t(millis()-start)<timeout) {
    handleF407Receive();
    if(mcuFailed) return false;
    if(mcuStateSerial!=serial && mcuState==state) return true;
    delay(1);
  }
  mcuMessage="F407 response timeout"; mcuFailed=true; return false;
}
/* INFO是观测请求。重启期间静默是预期状态，不把超时写入刷写失败标志。
 * ★ 判"板子在答"只看**有没有新的 INFO 回帧**，不再额外要求 stage==IDLE：
 *   刚跳转/刚上电那几秒，板上可能还停在上一轮的 DONE，或者先忙一段外设初始化，
 *   那不代表镜像没落地。stage 照旧上报，只是不再拿它当"活着"的条件
 *   （以前 online=false 会连带把网页的 PASS 一起憋死）。 */
static bool mcuProbeInfo(uint32_t timeout) {
  const uint32_t serial=mcuStateSerial, start=millis();
  Serial.print("@IAP:INFO\n");
  while(uint32_t(millis()-start)<timeout) {
    handleF407Receive();
    if(mcuStateSerial!=serial) return true;
    delay(1);
  }
  return false;
}
/* MCU回报整个224KiB A槽；镜像尾部是擦除态FF，不能拿原BIN CRC直接比较。
 * 从已校验的原文CRC继续计算，无需分配补齐镜像或额外帧缓冲。 */
static uint32_t mcuPaddedCrc(uint32_t rawCrc,uint32_t length) {
  uint32_t crc=rawCrc^0xFFFFFFFFu;
  for(uint32_t offset=length;offset<229376u;offset++) {
    crc^=0xFFu;
    for(unsigned bit=0;bit<8;bit++) crc=(crc>>1)^((crc&1u)?0xEDB88320u:0u);
    if((offset&1023u)==0u) delay(0);
  }
  return crc^0xFFFFFFFFu;
}
static void mcuUpdateTick() {
  if(!mcuVerifying) return;
  const uint32_t now=millis();
  if((mcuVerifyMatched && uint32_t(now-mcuVerifyMatchedAt)>=MCU_VERIFY_GRACE_MS) ||
     uint32_t(now-mcuVerifyStarted)>=MCU_VERIFY_LIMIT_MS) {
    mcuMessage=mcuVerifyMatched ? "Slot A verified; OTA reservation released" :
      "Reboot verification timed out; image was committed, check Slot A status";
    mcuVerifying=false; mcuUpdating=false;
    /* 恢复自动语音门禁。失败超时只释放占用，绝不把未验证的镜像判为成功。 */
    Serial.print("@AUDIO:READY\n");
  }
}
static bool mcuAuthorized();
static const char *mcuApprovalName();
static void mcuStatusRequest() {
  if(!mcuAuthorized()) { mcuUpdateServer.send(401,"text/plain","Unauthorized"); return; }
  mcuUpdateTick();
  const bool fresh=mcuProbeInfo(1800u);
  if(mcuVerifying && !mcuVerifyMatched && fresh && mcuBase==0x08008000u &&
     mcuCapacity==229376u && mcuLayout==0x00020001u && mcuCrc==mcuExpectedSlotCrc) {
    mcuVerifyMatched=true; mcuVerifyMatchedAt=millis();
    mcuMessage="New application replied; full Slot A CRC matches";
  }
  String body="{\"online\":"+String(fresh?"true":"false")+",\"base\":"+String(mcuBase)+
    ",\"slot_crc\":"+String(mcuCrc)+",\"expected_crc\":"+String(mcuExpectedSlotCrc)+
    ",\"capacity\":"+String(mcuCapacity)+",\"layout_id\":"+String(mcuLayout)+",\"state\":"+String(mcuState)+
    ",\"approval\":\""+String(mcuApprovalName())+"\",\"verifying\":"+String(mcuVerifying?"true":"false")+
    ",\"verify_crc_match\":"+String(mcuVerifyMatched?"true":"false")+"}";
  mcuUpdateServer.send(200,"application/json",body);
}
static bool mcuSendChunk() {
  mcuAck=65535;
  for(unsigned attempt=0;attempt<4;attempt++) {
    mcuFrame(0x10,mcuSeq,mcuChunk,mcuFill);
    uint32_t start=millis();
    while(uint32_t(millis()-start)<1200) {
      handleF407Receive();
      if(mcuFailed) return false;
      if(mcuAck==mcuSeq) { mcuSeq++; mcuFill=0; return true; }
      delay(1);
    }
  }
  mcuMessage="F407 block ACK timeout"; mcuFailed=true; return false;
}
/* 把一段字节喂进 128 B 攒块器（满一块就停等 ACK 发出去）。
   ★ 这段是从 mcuUpload() 里**逐字抽出**的：8080 那条（局域网直连）与云端那条
     （bridge_iap_pull.h 的"设备主动拉"）**共用同一份**，别再各写一份。
   返回 false ⇒ 已失败（mcuFailed 一定也是 true），原因在 mcuMessage。 */
static bool mcuFeed(const uint8_t *p,size_t n) {
  for(size_t i=0;i<n;i++) {
    if(mcuGot>=mcuTotal) { mcuFailed=true; mcuMessage="Image longer than declared"; return false; }
    mcuChunk[mcuFill++]=p[i]; mcuGot++;
    if(mcuFill==128 && !mcuSendChunk()) return false;
  }
  return true;
}
/* 发最后不足 128 B 的尾巴。
   ★ 前置条件 mcuGot==mcuTotal 是**刻意保留**的：原代码写的是
     `if(mcuGot!=mcuTotal || (mcuFill && !mcuSendChunk())) mcuFailed=true;` ——
     字节数不对时**短路、根本不发尾巴**（发了就是拿不完整的数据去糊 F407 的 flash）。
     ⚠️ 所以调用方在调它**之前**必须先自己确认 mcuGot==mcuTotal；
        返回 false 而 mcuMessage 还是旧值 ⇒ 就是撞上了这个前置条件。 */
static bool mcuFlushTail() {
  if(mcuGot!=mcuTotal) return false;
  if(mcuFill && !mcuSendChunk()) return false;
  return true;
}
static bool mcuAuthorized() {
  return mcuUpdateServer.header("Authorization")==String("Bearer ")+API_TOKEN;
}
static void mcuAbort() {
  uint8_t cmd=5;
  if(mcuUpdating && !mcuCommitted) mcuFrame(0x15,0,&cmd,1);
  mcuUpdating=false;
  mcuApproval=0; mcuRequestTotal=0; mcuRequestCrc=0;
}
static bool mcuNumber(const String &s,uint32_t &out) {
  if(!s.length() || s.length()>10) return false;
  out=0;
  for(unsigned i=0;i<s.length();i++) {
    if(s[i]<'0'||s[i]>'9'||out>(0xFFFFFFFFu-uint32_t(s[i]-'0'))/10) return false;
    out=out*10+s[i]-'0';
  }
  return true;
}
static const char *mcuApprovalName() {
  return mcuApproval==1 ? "waiting_user" : (mcuApproval==2 ? "approved" : "none");
}
static void mcuRequestUpdate() {
  uint32_t total, crc;
  if(!mcuAuthorized()) { mcuUpdateServer.send(401,"text/plain","Unauthorized"); return; }
  if(mcuVerifying || mcuUpdating) {
    mcuUpdateServer.send(409,"application/json","{\"ok\":false,\"err\":\"OTA transfer or reboot verification is still active\"}"); return;
  }
  if((cameraBusy() || audioBusy()) || !mcuNumber(mcuUpdateServer.arg("total"),total) ||
     !mcuNumber(mcuUpdateServer.arg("crc"),crc) || total<8 || total>229376u) {
    mcuUpdateServer.send(409,"application/json","{\"ok\":false,\"err\":\"recording busy or invalid image\"}"); return;
  }
  mcuFailed=false; mcuError=0; mcuApproval=0; mcuRequestTotal=total; mcuRequestCrc=crc;
  char request[48];
  snprintf(request,sizeof(request),"@IAP:REQUEST,%lu,%lu\n",(unsigned long)total,(unsigned long)crc);
  Serial.print(request);
  uint32_t start=millis();
  while(uint32_t(millis()-start)<1800 && !mcuFailed && mcuApproval==0) { handleF407Receive(); delay(1); }
  if(mcuApproval!=1) {
    mcuMessage="F407 did not enter user-confirmation state";
    mcuUpdateServer.send(409,"application/json","{\"ok\":false,\"err\":\"F407 did not accept request\"}"); return;
  }
  mcuMessage="Waiting for F407 K-UP confirmation";
  mcuUpdateServer.send(202,"application/json","{\"ok\":true,\"approval\":\"waiting_user\"}");
}
static void mcuUpload() {
  HTTPUpload &u=mcuUpdateServer.upload();
  if(u.status==UPLOAD_FILE_START) {
    /* 新的multipart不能覆盖已提交镜像的重启校验账本。 */
    mcuUploadRefused=mcuVerifying;
    if(mcuUploadRefused) return;
    /* Some ESP8266WebServer versions can replay FILE_START after a stalled
       multipart read.  Do not reset the sequence/byte counters or send a
       second META while an upload is already active. */
    if(mcuUpdating) {
      DBG.println("[IAP] duplicate upload START ignored");
      return;
    }
    mcuFailed=false; mcuCommitted=false; mcuError=0; mcuFill=0; mcuSeq=0;
    mcuMessage="Upload started";
    uint32_t expectedCrc;
    if(!mcuAuthorized() || (cameraBusy() || audioBusy()) || mcuApproval!=2 ||
       !mcuNumber(mcuUpdateServer.arg("total"),mcuTotal) ||
       !mcuNumber(mcuUpdateServer.arg("crc"),expectedCrc) || mcuTotal!=mcuRequestTotal ||
       expectedCrc!=mcuRequestCrc || mcuTotal<8 || mcuTotal>229376u) {
      mcuFailed=true; mcuMessage="Unauthorized, recording busy, or invalid image size/CRC"; return;
    }
    mcuVerifyMatched=false;
    mcuExpectedSlotCrc=mcuPaddedCrc(expectedCrc,mcuTotal);
    mcuUpdating=true; mcuGot=0;
    // Probe first: legacy apps are rejected before sending BEGIN/erasing flash.
    uint32_t serial=mcuStateSerial;
    Serial.print("@IAP:INFO\n");
    if(!mcuWait(0,serial,2500) || mcuBase!=0x08008000 || mcuCapacity!=229376u || mcuLayout!=0x00020001u) {
      mcuFailed=true; mcuMessage="Install the expanded bootstrap HEX via ROM UART first; F407 must be idle"; return;
    }
    uint8_t meta[12]={0}; iapWrite32(meta,mcuTotal);iapWrite32(meta+4,expectedCrc);meta[8]=1;
    serial=mcuStateSerial; mcuFrame(0x13,0,meta,sizeof(meta));
    if(!mcuWait(2,serial,12000)) return;
    mcuMessage="Transferring to F407";
  } else if(u.status==UPLOAD_FILE_WRITE && mcuUpdating && !mcuFailed) {
    if(mcuUploadRefused || mcuVerifying) return;
    mcuFeed(u.buf,u.currentSize);
  } else if(u.status==UPLOAD_FILE_END) {
    if(mcuUploadRefused || mcuVerifying) return;
    if(mcuUpdating && !mcuFailed) {
      if(!mcuFlushTail()) mcuFailed=true;
      if(!mcuFailed) {
        uint8_t cmd=3; uint32_t serial=mcuStateSerial;
        mcuFrame(0x15,0,&cmd,1);
        if(mcuWait(4,serial,5000)) {
          cmd=4; serial=mcuStateSerial; mcuFrame(0x15,0,&cmd,1);
          if(mcuWait(5,serial,6000)) {
            mcuCommitted=true; mcuMessage="Image verified and committed; reboot requested";
            mcuVerifying=true; mcuVerifyMatched=false; mcuVerifyStarted=millis();
            Serial.print("@IAP:JUMP\n");
          }
        }
      }
    }
    /* 传输结束不是校验结束：保留mcuUpdating，让loop与接收器继续阻止音频和相机。
     * 保留到真实新应用CRC匹配（含应答宽限），或180秒有界超时。 */
    if(mcuVerifying) { mcuApproval=0; mcuRequestTotal=0; mcuRequestCrc=0; }
    else mcuAbort();
  } else if(u.status==UPLOAD_FILE_ABORTED) {
    if(mcuUploadRefused || mcuVerifying) return; /* COMMIT之后HTTP断开不代表安装失败，不能撤销校验。 */
    mcuFailed=true; mcuMessage="HTTP upload interrupted"; mcuAbort();
  }
}
static void mcuUpdateBegin() {
  mcuUpdateServer.collectHeaders("Authorization");
  mcuUpdateServer.on("/f407/status",HTTP_GET,mcuStatusRequest);
  mcuUpdateServer.on("/f407/request",HTTP_POST,mcuRequestUpdate);
  mcuUpdateServer.on("/f407/update",HTTP_POST,[](){
    if(!mcuAuthorized()) { mcuUpdateServer.send(401,"text/plain","Unauthorized"); return; }
    if(mcuUploadRefused) {
      mcuUpdateServer.send(409,"application/json","{\"ok\":false,\"err\":\"Previous image is still being verified; no new image accepted\"}"); return;
    }
    String body="{\"ok\":"+String(mcuCommitted?"true":"false")+",\"message\":\""+mcuMessage+
      "\",\"error\":"+String(mcuError)+",\"received\":"+String(mcuGot)+"}";
    mcuUpdateServer.send(mcuCommitted?200:409,"application/json",body);
  },mcuUpload);
  mcuUpdateServer.begin();
  DBG.println("[IAP] authenticated F407 updater on port 8080");
}
#endif
