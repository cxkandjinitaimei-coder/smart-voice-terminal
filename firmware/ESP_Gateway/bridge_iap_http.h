/*
 * 文件：bridge_iap_http.h
 *
 * 职责：增量解析OTA下载的HTTP响应，确保写入F407前已拿到完整且长度准确的二进制块。
 * 使用：先reset(target,expectedBytes,now)，将收到的TCP片段逐次feed(data,len,now)。
 *   只有complete()==true才允许消费target；feed返回false后读取reason()/statusCode()判断失败。
 *   接收缓冲由调用方提供，本类不分配正文；目标容量必须至少等于expectedBytes。
 * 流程：累计HTTP头 → 解析状态行 → 校验唯一Content-Length → 原样复制正文 → 精确长度完成。
 * 校验：只接受HTTP/1.x 200；头部最多1023字节并拒绝NUL；正文期望1~4096B。
 *   缺长度、重复长度、长度不符、编码正文或多余字节都报错，不使用零填充掩盖截断。
 * 时间：feed收到非空数据才更新进展时间；调用方用idleExpired判断无数据超时。
 *   连接/重试总预算由上层管理，本类不建立TCP连接，不负责重试或发MCU ACK。
 * 依赖：仅标准C/C++头，可用于主机测试；对象与缓冲不得同时由多个任务修改。
 */

#ifndef BRIDGE_IAP_HTTP_H
#define BRIDGE_IAP_HTTP_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

// 固件 HTTP 拆包：先接完整块再允许写 MCU，截断响应可安全重拉。
class IapHttpBlock {
  char header[1024];
  size_t headUsed = 0, bodyUsed = 0, expected = 0;
  // 正文写入调用方缓冲，只复制实际收到的字节；reset后旧接收计数全部失效。
  uint8_t *output = nullptr;
  uint32_t progressAt = 0;
  bool headReady = false;
  const char *error = nullptr;
  unsigned code = 0;
  // HTTP字段名按ASCII大小写不敏感比较，不改变固件正文的原始字节。
  static bool same(const char *a, const char *b) {
    while (*a && *b) {
      if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
        return false;
    }
    return *a == *b;
  }
  // 状态行：仅接受 HTTP/1.x 200；固件正文不能按字符串处理。
  bool parseHeader() {
    unsigned major = 0, minor = 0;
    if (sscanf(header, "HTTP/%u.%u %u", &major, &minor, &code) != 3 || major != 1) {
      error = "invalid HTTP status line";
      return false;
    }
    if (code != 200) {
      error = "HTTP status is not 200";
      return false;
    }
    bool haveLength = false;
    char *line = strstr(header, "\r\n");
    if (!line) {
      error = "invalid HTTP header";
      return false;
    }
    line += 2;
    while (line[0] && line[0] != '\r') {
      char *end = strstr(line, "\r\n");
      if (!end) {
        error = "invalid HTTP header line";
        return false;
      }
      *end = 0;
      // 暂时以NUL隔开字段名和值，解析后恢复原分隔符；不修改正文。
      char *colon = strchr(line, ':');
      if (colon) {
        *colon = 0;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
          ++value;
        // 长度：必须存在且唯一、数字有效、恰好等于本次期望块长。
        if (same(line, "Content-Length")) {
          char *tail = nullptr;
          unsigned long length = strtoul(value, &tail, 10);
          // 数字后只容许空白；有尾随垃圾、重复字段或与期望不符立即拒绝。
          while (*tail == ' ' || *tail == '\t')
            ++tail;
          if (haveLength || !isdigit((unsigned char)*value) || *tail || length != expected) {
            error = "Content-Length differs from expected firmware block";
            return false;
          }
          haveLength = true;
          // 编码：拒绝 chunked 等传输编码，避免把编码边界写入固件。
        } else if (same(line, "Transfer-Encoding") && !same(value, "identity")) {
          error = "encoded response is not a raw firmware block";
          return false;
        }
        *colon = ':';
      }
      *end = '\r';
      line = end + 2;
    }
    if (!haveLength) {
      error = "missing Content-Length";
      return false;
    }
    return true;
  }

public:
  // target的生命周期必须覆盖接收过程，bytes为本次完整块长度，不是目标缓冲总容量。
  void reset(uint8_t *target, size_t bytes, uint32_t now) {
    output = target;
    expected = bytes;
    headUsed = bodyUsed = 0;
    progressAt = now;
    headReady = false;
    error = nullptr;
    code = 0;
    header[0] = 0;
    if (!target || !bytes || bytes > 4096)
      error = "invalid firmware block size";
  }
  // 接收：先找 CRLF CRLF，再逐字节复制正文；长度不足不完成，超长报错。
  bool feed(const uint8_t *data, size_t bytes, uint32_t now) {
    if (error)
      return false;
    // 只有收到实际数据才延后无进展超时；反复空feed不能让坏连接无限存活。
    if (bytes)
      progressAt = now;
    for (size_t i = 0; i < bytes; i++) {
      if (!headReady) {
        if (!data[i] || headUsed + 1 >= sizeof(header)) {
          error = "HTTP header too large or contains NUL";
          return false;
        }
        header[headUsed++] = char(data[i]);
        header[headUsed] = 0;
        // 头尾：只有完整头部通过状态、长度和编码校验后才进入正文态。
        if (headUsed >= 4 && !memcmp(header + headUsed - 4, "\r\n\r\n", 4)) {
          if (!parseHeader())
            return false;
          headReady = true;
        }
      } else {
        // 超过期望长度即报错，防止错误HTTP长度把后续数据混入固件块。
        if (bodyUsed >= expected) {
          error = "extra firmware body bytes";
          return false;
        }
        output[bodyUsed++] = data[i];
      }
    }
    return true;
  }
  // 三项同时满足才可写入MCU：头已通过、没有错误、正文字节数精确相等。
  bool complete() const {
    return headReady && !error && bodyUsed == expected;
  }
  // 使用无符号时间差兼容millis回绕；与整个HTTP块的总重试预算分别检查。
  bool idleExpired(uint32_t now, uint32_t limit) const {
    return uint32_t(now - progressAt) >= limit;
  }
  size_t received() const {
    return bodyUsed;
  }
  const char *reason() const {
    return error ? error : "incomplete HTTP response";
  }
  unsigned statusCode() const {
    return code;
  }
};
#endif
