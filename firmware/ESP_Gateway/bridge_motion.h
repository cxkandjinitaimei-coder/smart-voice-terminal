/*
 * 文件：bridge_motion.h
 *
 * 职责：校验完整舵机配置，并将服务器提供的十六进制动作载荷封装为F407二进制帧。
 * 使用：配置先motionProfileValid(hex,revision)验证，成功才缓存或下发；
 *   动作调用motionSendHex(port,hex,type,seq)，port需支持write(buffer,len)与write(byte)。
 * 配置格式：版本1B、舵机数1B、revision LE32，随后每舵机6B：编号/最小角/最大角/回正角/角色/保留。
 *   1~16路，无重复编号/角色；角度使用0~180绝对值，回正必须在该舵机限位之内。
 * 版本校验：revision由舵机条目CRC32导出，CRC为0时归一为1；不是公开固件版本号。
 * 发送：先验证所有十六进制字符和长度，再输出A5 5A头、原始载荷、XOR，避免半帧。
 *   0x23待机配置上限708B，其余路径上限584B；含义与执行安全检查仍由具体协议/F407承担。
 * 边界：motionSendHex只检查编码与长度，不替代每种动作载荷的语义校验；不在ESP执行舵机动作。
 * 依赖：仅标准整数/字符串接口，Port使用模板注入，便于硬件串口和主机夹具共用。
 */

#ifndef BRIDGE_MOTION_H
#define BRIDGE_MOTION_H
#include <stdint.h>
#include <string.h>

// revision按小端读取，不直接将不对齐字节数组解释为uint32_t指针。
static uint32_t motionRead32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
// 与服务端配置revision计算一致：初值/终值均异或0xFFFFFFFF，多项式0xEDB88320。
static uint32_t motionCrc32(const uint8_t *p, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (unsigned b = 0; b < 8; b++)
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
  }
  return crc ^ 0xFFFFFFFFu;
}
// 合法十六进制字符返回0~15，其他返回-1；先校验再拼字节。
static int motionHexDigit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
// 舵机配置：先解十六进制，再核对版本、长度、角度范围和角色唯一性。
static inline bool motionProfileValid(const char *hex, uint32_t &revision) {
  size_t chars = strlen(hex);
  if (chars < 24u || chars > 204u || (chars & 1u))
    return false;
  // 最多16个舵机条目，共6B头+16×6B数据；先检查字符长度再解码。
  uint8_t p[102];
  size_t n = chars / 2u;
  for (size_t i = 0; i < n; i++) {
    int hi = motionHexDigit(hex[2 * i]), lo = motionHexDigit(hex[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    p[i] = (uint8_t)((hi << 4) | lo);
  }
  if (p[0] != 1u || !p[1] || p[1] > 16u || n != 6u + 6u * p[1])
    return false;
  // mask检测编号重复，roles检测非零语义角色重复；0角色允许多个普通舵机。
  uint16_t mask = 0u;
  uint8_t roles = 0u;
  for (unsigned i = 0; i < p[1]; i++) {
    const uint8_t *s = p + 6u + 6u * i;
    if (!s[0] || s[0] > 16u || s[1] > s[2] || s[2] > 180u || s[3] < s[1] || s[3] > s[2] ||
        s[4] > 2u || s[5])
      return false;
    uint16_t bit = (uint16_t)(1u << (s[0] - 1u));
    // 同一个物理通道不能在同一配置中出现两次，否则限位和回正的归属不明确。
    if (mask & bit)
      return false;
    mask |= bit;
    if (s[4]) {
      uint8_t role = (uint8_t)(1u << s[4]);
      if (roles & role)
        return false;
      roles |= role;
    }
  }
  // revision 来自配置载荷 CRC，校验通过才允许缓存或下发。
  uint32_t computed = motionCrc32(p + 6, n - 6);
  // CRC结果为0时归一为1，以保留revision=0作为无有效配置的标记。
  if (!computed)
    computed = 1u;
  revision = motionRead32(p + 2);
  return revision == computed;
}

template <class Port>
// 先校验完整编码，再写 UART；避免发送半帧或超长文本行。
static bool motionSendHex(Port &port, const char *hex, uint8_t type, uint16_t seq) {
  size_t chars = strlen(hex);
  if (!chars || (chars & 1u) || chars > (type == 0x23u ? 1416u : 1168u))
    return false;
  // 写出任何帧头前验证全部字符，避免校验到中间才发现错误而留下UART半帧。
  for (size_t i = 0; i < chars; i++)
    if (motionHexDigit(hex[i]) < 0)
      return false;
  uint16_t n = (uint16_t)(chars / 2u);
  // 按二进制帧发送：头部 → 数据 → XOR，序号与长度均小端。
  uint8_t h[7] = {
      0xA5, 0x5A, type, (uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)n, (uint8_t)(n >> 8)};
  uint8_t sum = 0;
  for (unsigned i = 2; i < 7; i++)
    sum ^= h[i];
  // 头部和后续载荷连续输出到同一Port；调用方须保证没有其他任务穿插写UART。
  port.write(h, 7);
  for (size_t i = 0; i < chars; i += 2) {
    uint8_t b = (uint8_t)((motionHexDigit(hex[i]) << 4) | motionHexDigit(hex[i + 1]));
    port.write(b);
    sum ^= b;
  }
  port.write(sum);
  return true;
}
#endif
