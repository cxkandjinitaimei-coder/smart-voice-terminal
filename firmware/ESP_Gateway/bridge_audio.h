/*
 * 文件：bridge_audio.h
 *
 * 职责：提供不依赖 Wi-Fi/Flash 的串口拆帧器 BridgeWire 和音频会话接收器 BridgeAudio。
 * 使用：先实现 AudioTransport，再构造 BridgeAudio(transport)；每个 UART 字节调用 feed(c,now)。
 *   feed返回1时读取text，返回2时读取type/seq/len/payload并分发；返回-1时处理损坏输入。
 *   音频帧调用 audio.frame(...)，主循环持续调用 audio.tick(millis())，以处理背压和超时。
 * 数据格式：A5 5A | type:u8 | seq:LE16 | len:LE16 | payload[len] | XOR:u8。
 *   XOR覆盖type到payload，不含A5 5A；文本以CR/LF结束，最多191字节有效内容。
 * 音频流程：0x01 START确认格式并申请资源 → 0x02 DATA按2048B块停等 → 0x03 END核对实际块数。
 *   START载荷包含时长、总长、采样率、位深、声道；可选flags位0表示VAD短句。
 * 确认规则：ACK的next表示下一期望块号，flags=0成功、1请求重传、2会话失败。
 *   DATA确认后仍可能暂存在本类tail中，确认不代表已落盘或上传成功。
 * 缓冲归属：payload/text属BridgeWire，下次feed会继续使用；不要长期保存其地址。
 *   tail保留一块尾数据，deferred在传输层背压时临时保存下一块；END校验后才交付真正尾块。
 * 约束：本类由同一主循环串行调用，不具备线程安全性；tick间隔须满足2500ms会话超时。
 *   UART帧中途空闲超过250ms会重新同步；损坏/丢样的录音不能标为完整。
 */

#ifndef BRIDGE_AUDIO_H
#define BRIDGE_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// 串口拆帧：文本与 A5 5A 二进制共用接收流，分段到达时保留状态。
class BridgeWire {
  // state：0文本/找头、1等5A、2读5字节头、3读载荷、4读XOR。
  // check仅累计type/seq/len/payload；pos按当前阶段记录写入偏移。
  uint8_t state = 0, check = 0, hdr[5];
  uint16_t pos = 0, textUsed = 0;
  uint32_t last = 0;
  // 文本溢出后持续丢弃至换行，避免把坏行尾部误当有效命令；utf8Left保护中文续字节。
  bool discard = false;
  uint8_t utf8Left = 0;

public:
  uint8_t type = 0, payload[2048];
  uint16_t seq = 0, len = 0;
  char text[192];
  // 清理半帧和半行以重新同步；公开payload/text的旧内容不再作为新输入提交。
  void reset() {
    state = 0;
    pos = textUsed = 0;
    discard = false;
    utf8Left = 0;
  }

  // 返回：1=完整文本，2=完整二进制帧，-1=输入损坏，0=继续收。
  int feed(uint8_t c, uint32_t now) {
    // 未收完的二进制帧空闲超过 250ms，丢弃并重新找帧头。
    if (state && uint32_t(now - last) > 250)
      reset();
    last = now;
    // 起始：识别 A5；UTF-8 续字节中的 A5 不作为帧头。
    // 文本遇 CR/LF 才提交，超长或含 NUL 的行整行丢弃。
    if (!state) {
      if (c == 0xA5 && !utf8Left) {
        state = 1;
        textUsed = 0;
        discard = false;
        return 0;
      }
      if (c == '\r' || c == '\n') {
        int result = discard ? -1 : (textUsed ? 1 : 0);
        text[textUsed] = 0;
        textUsed = 0;
        discard = false;
        utf8Left = 0;
        return result;
      }
      // 始终给字符串结尾NUL留一字节；NUL或超长标记坏行，换行才报告错误。
      if (!c || textUsed >= sizeof(text) - 1)
        discard = true;
      else if (!discard) {
        text[textUsed++] = char(c);
        if (utf8Left && (c & 0xC0) == 0x80)
          utf8Left--;
        else if (c >= 0xC2 && c <= 0xDF)
          utf8Left = 1;
        else if (c >= 0xE0 && c <= 0xEF)
          utf8Left = 2;
        else if (c >= 0xF0 && c <= 0xF4)
          utf8Left = 3;
        else
          utf8Left = 0;
      }
      return 0;
    }
    // 同步：确认第二字节 5A；连续 A5 时保留重新同步机会。
    if (state == 1) {
      if (c == 0x5A) {
        state = 2;
        pos = 0;
        check = 0;
      } else
        state = (c == 0xA5) ? 1 : 0;
      return 0;
    }
    // 头部：类型 1B + 序号 LE16 + 长度 LE16；累加 XOR。
    if (state == 2) {
      hdr[pos++] = c;
      check ^= c;
      if (pos == 5) {
        type = hdr[0];
        seq = uint16_t(hdr[1]) | uint16_t(hdr[2]) << 8;
        len = uint16_t(hdr[3]) | uint16_t(hdr[4]) << 8;
        pos = 0;
        // 长度：先验证缓冲上限 2048B，再进入数据态；零长度直达校验态。
        if (len > sizeof(payload)) {
          reset();
          return -1;
        }
        state = len ? 3 : 4;
      }
      return 0;
    }
    // 数据：按声明长度接收，载荷中的换行与帧头字节不再单独解释。
    if (state == 3) {
      payload[pos++] = c;
      check ^= c;
      if (pos == len)
        state = 4;
      return 0;
    }
    // 校验：与头部和载荷的 XOR 比较，只有一致才交付完整帧。
    state = 0;
    return c == check ? 2 : -1;
  }
};

// 传输适配接口；pcm 只在可安全接收时接受数据，ACK 由协议层发出。
struct AudioTransport {
  // 申请会话，返回true才发送开始ACK；允许拒绝新录音，但不得破坏上一段仍在上传的会话。
  virtual bool open(uint16_t session, uint32_t total, uint8_t flags) = 0;
  // 背压查询：当前是否能安全接收下一块；false时协议层保留数据并暂缓ACK。
  virtual bool readyForPcm(uint16_t len) {
    (void)len;
    return true;
  }
  // 接受一块完整PCM；适配器必须保存需要的数据，调用返回后源缓冲可能被复用。
  virtual bool pcm(const uint8_t *data, uint16_t len) = 0;
  // 完成采集侧输入，传入实际PCM字节数；上传是否完成由适配器后续状态判断。
  virtual bool finish(uint32_t actualBytes) = 0;
  virtual void abort() = 0;
  // next为下一期望序号，flags=0接受、1重传、2拒绝/中止；由适配器构造线协议。
  virtual void ack(uint16_t next, uint8_t flags) = 0;
  virtual uint32_t clockNow(uint32_t fallback) {
    return fallback;
  }
  virtual void error(const char *why) {
    (void)why;
  }
};

// 音频接收：START → DATA 停等确认 → END；保留一块尾数据直到结束校验。
class BridgeAudio {
  AudioTransport &io;
  // start保存原始START供重复识别；tail保留末块，deferred保存受背压影响的新块。
  uint8_t start[11], tail[2048], deferred[2048];
  // session对应START/END会话号；next为已接收/确认的数据块数，blocks为最大声明块数。
  uint16_t session = 0, next = 0, blocks = 0;
  uint32_t touched = 0;
  bool running = false, shortSentence = false, tailPending = false, deferredPending = false,
       endPending = false;
  static uint16_t u16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
  }
  static uint32_t u32(const uint8_t *p) {
    return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16;
  }

public:
  explicit BridgeAudio(AudioTransport &transport) : io(transport) {}
  // 这里只表示MCU音频会话仍在接收；上传阶段还需合并查询transport.busy()。
  bool busy() const {
    return running;
  }
  // 返回下一期望DATA序号，用于诊断或重传定位，不包含服务器确认进度。
  uint16_t expected() const {
    return next;
  }
  // 失败释放协议占用，并通知传输层清理缓存和向 F407 回报错误。
  void fail(const char *why = "capture interrupted") {
    if (running) {
      running = false;
      deferredPending = endPending = false;
      io.error(why);
      io.abort();
      io.ack(next, 2);
    }
  }
  // 外层XOR或UART损坏时要求重传当前块，不因一次坏帧立即删除整段录音。
  void badFrame() {
    if (running)
      io.ack(next, 1);
  }
  // 先处理尾块和延迟块，再检查超时；RAM 可用后才推进序号并 ACK。
  void tick(uint32_t now) {
    if (!running)
      return;
    // END已经验证，但尾块尚未进入适配器；先交尾块再finish，禁止提前宣告完整。
    if (endPending && io.readyForPcm(2048)) {
      if (!io.pcm(tail, 2048) || !io.finish(uint32_t(next) * 2048)) {
        fail("final PCM could not be saved");
        return;
      }
      endPending = false;
      running = false;
      return;
    }
    // 先提交旧tail，再把deferred升为新tail，最后next++与ACK，保持块顺序。
    if (deferredPending && io.readyForPcm(2048)) {
      if (!io.pcm(tail, 2048)) {
        fail();
        return;
      }
      memcpy(tail, deferred, 2048);
      deferredPending = false;
      next++;
      touched = now;
      io.ack(next, 0);
    }
    // 用无符号差值处理millis回绕；长期未推进则关闭会话，不能永久占用录音。
    if (uint32_t(now - touched) > 2500)
      fail("MCU frames timed out 2500ms");
  }
  void frame(uint8_t type, uint16_t seq, const uint8_t *p, uint16_t n, uint32_t now) {
    // 开始：检查头长度、重复会话、录音时长及 PCM 格式，再申请传输资源。
    if (type == 1) {
      if (n != 10 && n != 11) {
        if (running)
          fail();
        else
          io.ack(0, 2);
        return;
      }
      // 同一 START 重传仅补 ACK；不同会话不能覆盖正在录制的数据。
      if (running) {
        if (seq == session && n == (shortSentence ? 11 : 10) && !memcmp(start, p, n)) {
          touched = now;
          io.ack(next, 0);
        } else
          io.ack(0, 2);
        return;
      }
      // 格式：16kHz/16bit/单声道；每块 2048B，时长按 64ms 量化。
      const uint16_t seconds = u16(p);
      const uint32_t total = u32(p + 2);
      const uint8_t flags = (n == 11) ? p[10] : 0;
      if ((n == 11 && (flags & ~uint8_t(1))) || !seconds || seconds > 10 ||
          total != (uint32_t(seconds) * 1000 / 64) * 2048 || u16(p + 6) != 16000 || p[8] != 16 ||
          p[9] != 1) {
        io.ack(0, 2);
        return;
      }
      next = 0;
      blocks = uint16_t(total / 2048);
      session = seq;
      shortSentence = (flags & 1) != 0;
      tailPending = false;
      deferredPending = endPending = false;
      memcpy(start, p, n);

      // 上句可能仍在上传；open 拒绝时只拒收新会话，不中止旧上传。
      if (!io.open(seq, total, flags)) {
        io.ack(0, 2);
        return;
      }
      // 资源准备成功后发 READY ACK，F407 才开始采集。
      running = true;
      touched = io.clockNow(now);
      io.ack(0, 0);
      return;
    }
    if (!running)
      return;
    touched = now;
    // 数据：验证顺序和固定块长；重复序号只回 ACK，不重复写入。
    if (type == 2) {
      // 已有延迟块时不接受第二份新块，等待tick释放空间；F407停等/重传保证不越序。
      if (deferredPending)
        return;
      // 此前已接收的块通常是ACK丢失后的重发；仅补确认，不再次累加音频。
      if (seq < next && n == 2048) {
        io.ack(next, 0);
        return;
      }
      if (seq != next || n != 2048 || next >= blocks) {
        io.ack(next, 1);
        return;
      }

      // 背压：保存新块并延迟 ACK，等待上一尾块可交付后继续。
      if (tailPending && !io.readyForPcm(2048)) {
        memcpy(deferred, p, 2048);
        deferredPending = true;
        return;
      }
      if (tailPending && !io.pcm(tail, 2048)) {
        fail();
        return;
      }
      memcpy(tail, p, n);
      tailPending = true;
      next++;
      io.ack(next, 0);
      // 结束：核对会话、实际/确认块数和丢样计数；VAD 可短于最大时长。
    } else if (type == 3) {
      if (n != 6) {
        fail("MCU END length invalid");
        return;
      }
      const uint16_t actual = u16(p);
      // END中的丢半块计数必须为0；即使块数对上，有采样丢失也不能作为完整语音。
      if (u16(p + 4) != 0) {
        fail("MCU reported lost microphone samples");
        return;
      }
      if (seq != session || actual == 0 || actual > blocks || u16(p + 2) != actual ||
          next != actual || (!shortSentence && actual != blocks) || !tailPending) {
        fail("MCU ended before all blocks confirmed");
        return;
      }
      // 尾块仍受背压约束；tick 在可接收后完成，不提前关闭 HTTP 正文。
      if (!io.readyForPcm(2048)) {
        endPending = true;
        return;
      }
      if (!io.pcm(tail, 2048) || !io.finish(uint32_t(actual) * 2048)) {
        fail("final PCM could not be saved");
        return;
      }
      // 这里只结束串口采集会话；Flash排空、网络发送和最终READY仍由适配器负责。
      running = false;
    }
  }
};
#endif
