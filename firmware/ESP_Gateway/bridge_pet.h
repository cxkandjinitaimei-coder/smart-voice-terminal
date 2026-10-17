/*
 * 文件：bridge_pet.h
 *
 * 职责：解析F407的@POSE文本，将三舵机绝对角度、运动与待机状态保存为一次完整快照。
 * 使用：UART行完成后调用pose.accept(line,millis())；成功返回true并设置valid和seen。
 *   主程序上报前再根据seen检查新鲜度，不能把网页下发目标角度当作板端当前姿态。
 * 格式：@POSE:uptime_ms,mask,s1,s2,s3,moving,idle,revision；必须恰好8个字段且无额外字符。
 *   mask低3位表示有效舵机；moving取0/1；idle取0~4；revision对应待机配置修订。
 * 数据：uptime为F407运行时间，seen为ESP接收时间；两者不同源，不能直接相减。
 * 校验：角度0~180、mask≤7，全部字段通过后才整体更新，错误输入保留上一有效快照。
 * 边界：这里保存的是F407上报估计位置，不是带编码器的机械实测角；本类不发命令或驱动PWM。
 * 依赖：仅标准整数与sscanf，适用于ESP固件与主机测试。
 */

#ifndef BRIDGE_PET_H
#define BRIDGE_PET_H
#include <stdint.h>
#include <stdio.h>
// 板端姿态快照：角度是绝对值；seen 为接收时间，发送上云前另检查新鲜度。
struct BridgePose {
  // uptime为板端时基，seen为ESP时基；revision是待机配置修订，不是固件版本。
  uint32_t uptime = 0u, revision = 0u, seen = 0u;
  uint16_t angle[3] = {0u, 0u, 0u};
  // mask位0/1/2分别对应三舵机有效位；invalid/超时姿态由上层决定是否上报。
  uint8_t mask = 0u, moving = 0u, idle = 0u;
  bool valid = false;
  // 输入应为去掉CR/LF的完整@POSE行；失败不更新seen，调用方可据此让旧姿态过期。
  bool accept(const char *text, uint32_t now) {
    unsigned long up, rev;
    unsigned m, a, b, c, mov, id;
    char extra;
    // 先校验字段数、额外字符及范围，通过后整组更新，避免部分姿态污染。
    if (sscanf(text, "@POSE:%lu,%u,%u,%u,%u,%u,%u,%lu%c", &up, &m, &a, &b, &c, &mov, &id, &rev,
               &extra) != 8 ||
        up > 0xffffffffUL || rev > 0xffffffffUL || m > 7u || a > 180u || b > 180u || c > 180u ||
        mov > 1u || id > 4u)
      return false;
    // 所有检查通过才整体提交字段，保证一次上报里的角度/运动状态来自同一行。
    uptime = (uint32_t)up;
    revision = (uint32_t)rev;
    seen = now;
    mask = (uint8_t)m;
    angle[0] = (uint16_t)a;
    angle[1] = (uint16_t)b;
    angle[2] = (uint16_t)c;
    moving = (uint8_t)mov;
    idle = (uint8_t)id;
    valid = true;
    return true;
  }
};
#endif
