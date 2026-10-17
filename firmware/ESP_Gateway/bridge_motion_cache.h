/*
 * 文件：bridge_motion_cache.h
 *
 * 职责：缓存已验证的舵机配置，使F407重新启动或断网时可恢复上一次限位/回正参数。
 * 使用：构造SavedMotionProfile<fs::FS>，LittleFS挂载后begin(LittleFS)，在线配置到达调用accept(hex)。
 *   wire()/revision()读取当前有效配置，cached()判断是否已成功持久化；由主循环负责重发及等ACK。
 * 流程：先检查在线配置 → 更新RAM → 临时文件写入 → 回读复核 → rename替换正式文件。
 * 文件：/servo-profile.hex为正式配置，/servo-profile.tmp为过渡文件；与音频/图片缓存分开。
 * 返回：accept=true表示在线配置有效且已进入RAM，不等同于Flash写入成功；持久化看cached()。
 * 失败：存储不可用仍可运行新配置，但保留原正式文件；启动读到损坏文件则视为没有缓存。
 *   不自动格式化文件系统，不凭空生成90度配置，也不修改服务器中的参数契约。
 * 依赖：bridge_motion.h负责格式/CRC检查；FileSystem需提供open/rename/remove及对应文件读写接口。
 * 边界：断电后的文件恢复依赖LittleFS重命名语义；本类不负责网页参数迁移或舵机立即运动。
 */

#ifndef BRIDGE_MOTION_CACHE_H
#define BRIDGE_MOTION_CACHE_H
#include "bridge_motion.h"

// 离线舵机配置独立保存；只加载有效文件，不格式化 LittleFS。
template <class FileSystem> class SavedMotionProfile {
  FileSystem *fs = nullptr;
  // RAM中最多204个十六进制字符加结尾NUL；rev/durable分别描述有效内容和持久化结果。
  char saved[205] = {0};
  uint32_t rev = 0;
  bool durable = false;
  static const char *path() {
    return "/servo-profile.hex";
  }
  static const char *temporary() {
    return "/servo-profile.tmp";
  }

public:
  // 启动读取并校验；损坏或截断文件不能提供回正角度。
  void begin(FileSystem &storage) {
    fs = &storage;
    saved[0] = 0;
    rev = 0;
    durable = false;
    auto f = fs->open(path(), "r");
    if (!f)
      return;
    size_t n = f.size();
    // 文件长度须容得下终止符且一次读完；只有CRC/字段均有效才接受启动缓存。
    if (n < sizeof(saved) && n && f.read((uint8_t *)saved, n) == (int)n) {
      saved[n] = 0;
      if (motionProfileValid(saved, rev))
        durable = true;
    }
    f.close();
    // 无效文件不填充默认角度，清空RAM修订号让调用方等待服务器配置。
    if (!durable) {
      saved[0] = 0;
      rev = 0;
    }
  }
  // 有效在线参数先用于运行；持久化失败时 cached=false，旧文件保留。
  bool accept(const char *wire) {
    uint32_t revision = 0;
    if (!motionProfileValid(wire, revision))
      return false;
    // 相同且已成功持久化的配置直接返回，避免每秒轮询都重写Flash。
    if (durable && rev == revision && !strcmp(saved, wire))
      return true;

    // 长度已由motionProfileValid限制；先应用有效新内容，再单独尝试持久化。
    strcpy(saved, wire);
    rev = revision;
    durable = false;
    if (!fs)
      return true;
    // 先写临时文件并回读校验，成功后原子重命名替换，失败只删除临时文件。
    auto f = fs->open(temporary(), "w");
    if (!f)
      return true;
    size_t n = strlen(wire);
    bool ok = f.write((const uint8_t *)wire, n) == n;
    f.close();
    // 写成功还要回读长度、内容与revision，防止未完整保存就替换上次好配置。
    if (ok) {
      auto check = fs->open(temporary(), "r");
      char content[205];
      ok = check && check.size() == n && check.read((uint8_t *)content, n) == (int)n;
      if (check)
        check.close();
      if (ok) {
        content[n] = 0;
        uint32_t r = 0;
        ok = motionProfileValid(content, r) && r == rev;
      }
    }
    // 仅回读校验通过才重命名覆盖正式文件；失败删除临时文件，不删旧正式文件。
    if (ok)
      durable = fs->rename(temporary(), path());
    if (!durable)
      fs->remove(temporary());
    return true;
  }
  const char *wire() const {
    return saved;
  }
  uint32_t revision() const {
    return rev;
  }
  // false可表示有效在线配置尚未落盘，不意味着当前RAM参数无效。
  bool cached() const {
    return durable;
  }
};
#endif
