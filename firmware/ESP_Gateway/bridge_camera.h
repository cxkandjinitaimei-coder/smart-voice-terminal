/*
 * 文件：bridge_camera.h
 *
 * 职责：提供不依赖ESP SDK的单张图片停等接收器，检查块CRC与整帧CRC。
 * 使用：实现CameraSink，构造CameraReceiver(sink)；请求拍照前arm(非零会话号)。
 *   将已通过外层拆帧/XOR校验的0x30/0x31/0x32帧传给frame()；结束后reset()取消接收。
 * 流程：START验证协议/会话/尺寸 → DATA逐块落盘并ACK → END验证总长和整帧CRC后提交。
 * 格式：320×240、RGB565、153600B，共300块；DATA载荷为512B像素数据+4B小端CRC32。
 *   CRC32反射多项式0xEDB88320，初始0xFFFFFFFF，最终再异或0xFFFFFFFF。
 * 确认：flag=0接受/补ACK、1重传、2不可恢复错误；同一块重传不重复写入。
 * 归属：像素指针只在frame调用期间有效，CameraSink.appendImage必须同步保存需要的数据。
 *   接收器仅保存序号和CRC，不申请整帧SRAM；实际文件/网络资源由CameraSink管理。
 * 边界：本类不控制VSYNC/WEN、不读取摄像头寄存器、不做人脸识别，也不决定上传服务器。
 */

#ifndef BRIDGE_CAMERA_H
#define BRIDGE_CAMERA_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// 图像落盘/确认接口；一次仅传一个 320×240 RGB565 帧，不分配整帧 SRAM。
struct CameraSink {
  virtual ~CameraSink() {}
  // START调用：准备空缓存；ACK丢失可能重复调用，首块前重开文件应是安全操作。
  virtual bool beginImage() = 0;
  // 同步保存一个已通过CRC的像素块；失败返回false，接收器不推进序号。
  virtual bool appendImage(const uint8_t *, size_t) = 0;
  // 整帧通过后通知上层，参数为最终CRC；上层可关闭写文件并进入上传阶段。
  virtual void endImage(uint32_t) = 0;
  virtual void cameraAck(uint16_t, uint16_t, uint8_t) = 0;
};
// 停等接收器：START、512B DATA+CRC32、END；会话由外部 arm 指定。
class CameraReceiver {
  CameraSink &sink;
  // sid为非零拍照会话，next为下一块序号；begun防止DATA越过START直接写入。
  uint16_t sid = 0, next = 0;
  uint32_t checksum = 0xFFFFFFFFu;
  bool ended = false, begun = false;
  static uint16_t u16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
  }
  static uint32_t u32(const uint8_t *p) {
    return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16;
  }

public:
  explicit CameraReceiver(CameraSink &s) : sink(s) {}
  // 可分块累计CRC；中间返回值不要先做最终异或，全部数据结束后再异或一次。
  static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    while (n--) {
      c ^= *p++;
      for (unsigned b = 0; b < 8; b++)
        c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0u);
    }
    return c;
  }
  // 为一次新的拍照请求清空计数和CRC；调用前由上层保证旧事务资源已释放。
  void arm(uint16_t id) {
    sid = id;
    next = 0;
    checksum = 0xFFFFFFFFu;
    ended = false;
    begun = false;
  }
  // 清会话号即可拒绝后续迟到帧；文件和网络资源仍由CameraSink释放。
  void reset() {
    sid = 0;
  }
  // 表示END已核验并提交给上层，不表示图像已上传/识别完成。
  bool complete() const {
    return ended;
  }
  uint32_t bytes() const {
    return uint32_t(next) * 512u;
  }
  // 输入须已通过外层XOR和长度缓冲检查；本层继续验证相机会话及CRC。
  void frame(uint8_t type, uint16_t seq, const uint8_t *p, uint16_t n) {
    if (!sid)
      return;
    // 开始：确认版本、会话、分辨率、像素格式和总长度，再创建缓存。
    if (type == 0x30) {
      if (n != 12 || p[0] != 1 || u16(p + 1) != sid || u16(p + 3) != 320 || u16(p + 5) != 240 ||
          p[7] != 1 || u32(p + 8) != 153600) {
        sink.cameraAck(sid, 0, 2);
        return;
      }
      // 首块前重复START可重建缓存；已写入后不得因重复START擦掉前面的像素。
      if (!next && !ended && !sink.beginImage()) {
        sink.cameraAck(sid, 0, 2);
        return;
      }
      begun = true;
      sink.cameraAck(sid, 0, 0);
      // 数据：先核对 516B 长度及块 CRC32，再处理序号与写入。
    } else if (type == 0x31) {
      if (!begun) {
        sink.cameraAck(sid, seq, 2);
        return;
      }
      if (n != 516 || u32(p + 512) != (crc(0xFFFFFFFFu, p, 512) ^ 0xFFFFFFFFu)) {
        sink.cameraAck(sid, next, 1);
        return;
      }
      // 重复块只补 ACK，避免丢 ACK 后把相同图像数据追加两次。
      if (next && seq == uint16_t(next - 1)) {
        sink.cameraAck(sid, next, 0);
        return;
      }
      if (ended || seq != next || next >= 300) {
        sink.cameraAck(sid, next, 1);
        return;
      }
      // 写入成功后才推进整帧 CRC、期望序号及 ACK。
      if (!sink.appendImage(p, 512)) {
        sink.cameraAck(sid, next, 2);
        return;
      }
      // 先持久接收，再更新整体CRC与next；ACK丢失导致的重复块不会到此处。
      checksum = crc(checksum, p, 512);
      next++;
      sink.cameraAck(sid, next, 0);
      // 结束：300 块、153600B 和整帧 CRC 均匹配才提交；重复 END 不重复提交。
    } else if (type == 0x32) {
      if (n != 10 || u16(p) != sid || u32(p + 2) != 153600 || next != 300 ||
          u32(p + 6) != (checksum ^ 0xFFFFFFFFu)) {
        sink.cameraAck(sid, 301, 2);
        return;
      }
      if (!ended) {
        ended = true;
        sink.endImage(checksum ^ 0xFFFFFFFFu);
      }
      sink.cameraAck(sid, 301, 0);
    }
  }
};
#endif
