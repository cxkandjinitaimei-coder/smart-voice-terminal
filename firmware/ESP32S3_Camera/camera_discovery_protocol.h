/* 与ESP8266共用的网页发现应答解析，不依赖Arduino，供设备与主机测试共用。
 * 输入网页UDP应答、发送方IP、本机STA地址和掩码，成功输出http://IP:port。
 * 按段检查起始标识、四段地址、端口和结尾；不接受不同网段或伪造的应答地址。
 * UDP仅用于找地址，业务请求仍使用原有API Token认证，不从广播读取密钥。
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

inline bool cameraDiscoveryNumber(const char *&p, const char *end, unsigned max, unsigned &value) {
  value=0;
  if(p==end || *p<'0' || *p>'9')return false;
  while(p!=end && *p>='0' && *p<='9') {
    value=value*10+unsigned(*p++-'0');
    if(value>max)return false;
  }
  return true;
}

inline bool cameraParseDiscovery(const char *data, size_t length, const uint8_t sender[4],
    const uint8_t local[4], const uint8_t mask[4], char *url, size_t capacity) {
  const char prefix[]="ARDP_IP:";
  if(length<17 || length>63 || std::memcmp(data,prefix,sizeof(prefix)-1)!=0)return false;
  const char *p=data+sizeof(prefix)-1,*end=data+length;
  unsigned ip[4],port;
  // 起始标识通过后，严格消费四段十进制地址，不允许夹带路径或额外数据。
  for(unsigned i=0;i<4;++i) {
    if(!cameraDiscoveryNumber(p,end,255,ip[i]) || p==end || *p++!=(i==3?':':'.'))return false;
  }
  // 端口必须完整结束；拒绝0端口、溢出和尾随命令。
  if(!cameraDiscoveryNumber(p,end,65535,port) || !port || p!=end)return false;
  if(ip[0]==0 || ip[0]==127 || ip[0]>=224 || (ip[0]==169 && ip[1]==254))return false;
  bool same=true,network=true,broadcast=true,hasMask=false,hasLocal=false;
  for(unsigned i=0;i<4;++i) {
    if(ip[i]!=sender[i] || (ip[i]&mask[i])!=(local[i]&mask[i]))return false;
    same=same && ip[i]==local[i];
    network=network && (ip[i]&uint8_t(~mask[i]))==0;
    broadcast=broadcast && (ip[i]&uint8_t(~mask[i]))==uint8_t(~mask[i]);
    hasMask=hasMask || mask[i]!=0;hasLocal=hasLocal || local[i]!=0;
  }
  if(same || network || broadcast || !hasMask || !hasLocal)return false;
  int written=std::snprintf(url,capacity,"http://%u.%u.%u.%u:%u",ip[0],ip[1],ip[2],ip[3],port);
  return written>0 && size_t(written)<capacity;
}
