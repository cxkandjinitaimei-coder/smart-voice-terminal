# -*- coding: utf-8 -*-
"""音频会话：收分块 → 边收边写 wav → 交 asr.py 识别 → 按模式回灌。

ESP 侧的上行协议（正文是**裸 PCM 字节**，不是 hex —— 见 合作表 §4.6）：
    POST /api/audio/chunk?token=..&sess=<id>&seq=0&rate=16000&bits=16&ch=1&total=<字节数>
        正文 = 第 0 块。★ 只有 seq=0 需要带 rate/bits/ch/total。
    POST /api/audio/chunk?token=..&sess=<id>&seq=N         正文 = 第 N 块
    POST /api/audio/end?token=..&sess=<id>                 （可选 &samples=N 对账）
    GET  /api/audio/status                                网页看状态
    POST /api/audio/send                                  手动模式下"确认发送"
    POST /api/audio/mode  {"mode":"manual"|"auto"}        切手动/自动

★ 五个设计要点，每条都是被现实逼出来的，别简化掉：

  1. **边收边写，不攒内存** —— P3 要录 10 秒以上，攒到收完再写是没必要的风险。
     每来一块立刻 append 到文件，进程内存只占一个块。
  2. **wav 头在收到第 0 块时就能写对** —— 因为第 0 块带了 `total`，
     RIFF/data 两个长度当场算得出来，**不用先写占位头再 seek 回去回填**。
  3. **收不齐绝不送去识别** —— 对不上就把头按"实收"改短（留一个能播的残件给人看），
     但**不进识别**。★ 拿半个 wav 去识别会得到一个"看着正常但内容错"的结果，最难查。
  4. **`/end` 必须立刻返回** —— ESP 的 `HTTP_TIMEOUT_MS` 只有 3000 ms，
     而实测模型加载 4.3 秒、6.4 秒音频识别 2.4 秒。⇒ 识别丢后台线程，结果挂状态接口。
  5. **绝不打印 token** —— 注入用的 token 由 app.py 传进来，本模块不读 config.json。
"""

import json
import os
import struct
import threading
import time
import queue
import re
import uuid
import audio_archive
import audio_retention

import asr

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJ = os.path.dirname(_HERE)
WAV_DIR = os.path.join(_PROJ, "语音识别", "wav")
_STATE_FILE = os.path.join(_HERE, "audio_state.json")

# ---- 默认参数 / 上限 ----
DEFAULT_RATE = 16000
DEFAULT_BITS = 16
DEFAULT_CH = 1
MAX_TOTAL_BYTES = 30 * DEFAULT_RATE * 2      # 30 秒上限，防跑飞的客户端把盘写满
VAD_MIN_BYTES = 10 * 2048                    # 0.64 秒；短于它是触发噪声，不进 ASR
SESSION_IDLE_SEC = 30                        # 超过这么久没新块 → 判死、扔掉
MAX_SESSIONS = 4

_lock = threading.Lock()
_sessions = {}          # sid -> Session
_last = None            # 最近一次会话的结果（给状态接口）
_pending = None         # 手动模式下等"确认发送"的文字
_sending = None         # Background send; UI must not wait for a cloud request.
_send_result = None
_jobs = queue.Queue()
_worker_lock = threading.Lock()
_worker = None
_archive_lock = threading.Lock()
_archive_dirs = set()
_mode = None            # None = 还没从磁盘读过


# ==================================================================
# 模式（manual / auto）—— 存本模块自己的小文件，**不碰 config.json**
# ==================================================================
def get_mode():
    global _mode
    with _lock:
        if _mode is None:
            _mode = "manual"
            try:
                with open(_STATE_FILE, "r", encoding="utf-8") as f:
                    m = str((json.load(f) or {}).get("mode", "")).strip().lower()
                if m in ("manual", "auto"):
                    _mode = m
            except Exception:
                pass          # 文件没有 / 坏了 → 用默认 manual（安全侧）
        return _mode


def set_mode(m):
    global _mode
    m = str(m or "").strip().lower()
    if m not in ("manual", "auto"):
        return None
    with _lock:
        _mode = m
    try:
        with open(_STATE_FILE, "w", encoding="utf-8") as f:
            json.dump({"mode": m}, f)
    except Exception as e:
        print("[AUDIO][WARN] 模式没存下来（%s）—— 本次运行仍生效，重启会回到 manual"
              % type(e).__name__)
    print("[AUDIO] 模式切到 %s（%s）" % (m, "录完自动回灌" if m == "auto" else "录完挂起等确认"))
    return m


# ==================================================================
# wav 头
# ==================================================================
def _wav_header(rate, bits, ch, data_bytes):
    """标准 44 字节 PCM 头。★ 长度当场就能算对 —— 不需要占位再回填。"""
    block_align = max(1, bits // 8) * ch
    byte_rate = rate * block_align
    return (b"RIFF" + struct.pack("<I", 36 + data_bytes) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, ch, rate, byte_rate, block_align, bits)
            + b"data" + struct.pack("<I", data_bytes))


# ==================================================================
# 会话
# ==================================================================
class Session(object):
    def __init__(self, sid, rate, bits, ch, total, vad_short=False):
        self.sid = sid
        self.record_id = uuid.uuid4().hex
        self.rate = rate
        self.bits = bits
        self.ch = ch
        self.total = total
        self.vad_short = bool(vad_short)
        self.received = 0
        self.next_seq = 0
        self.ok = True
        self.err = None
        self.t0 = time.time()
        self.last_at = time.time()
        os.makedirs(WAV_DIR, exist_ok=True)
        self.path = os.path.join(WAV_DIR, "rec_%s_%s.wav" % (
            time.strftime("%Y%m%d_%H%M%S"), self.record_id + "_" + sid))
        self.f = open(self.path, "wb")
        self.f.write(_wav_header(rate, bits, ch, total))   # ★ 第 0 块就写对
        # ★ 会话自己的锁：流式上行要连着写好几秒，**绝不能**霸着模块级的 _lock
        #   （那会把 status / end / 别的会话全卡住）。见 append_raw。
        self.lock = threading.Lock()

    def append(self, seq, data):
        self.last_at = time.time()
        if seq < self.next_seq:
            return {"ok": True, "dup": True, "need": self.next_seq,
                    "received": self.received}
        if seq > self.next_seq:
            # ★ 有空洞 —— 明确告诉对方"我要第几块"，让它补，别静默收下
            return {"ok": False, "gap": self.next_seq, "received": self.received,
                    "err": "缺块：期望 seq=%d，收到 %d" % (self.next_seq, seq)}
        if self.received + len(data) > MAX_TOTAL_BYTES:
            self.ok = False
            self.err = "超过 %d 字节上限" % MAX_TOTAL_BYTES
            return {"ok": False, "err": self.err}
        self.f.write(data)
        self.received += len(data)
        self.next_seq += 1
        return {"ok": True, "need": self.next_seq, "received": self.received}

    def append_raw(self, data):
        """流式上行：往会话里灌裸字节（**不按 seq**，见 `open_stream`）。

        ★ 三点与 `append` 不同，都是有意的：
          1. **自己拿会话锁**，不碰模块级 `_lock` —— 一条 10 秒的流不能把
             `status` / `/end` / 别的会话堵 10 秒。
          2. **多收的字节宁可截断也不写超**：wav 头在第 0 块就按 `total` 写死了，
             写超一个字节整段录音就废了。
          3. 写盘出错**不抛**，标 `ok=False` 让收尾走"残件"那条路。
        """
        with self.lock:
            self.last_at = time.time()
            if not data:
                return {"ok": True, "received": self.received, "need_bytes": self.total - self.received}
            room = self.total - self.received
            if room <= 0:
                self.ok = False
                self.err = "对方多发：已经收满 %d 字节" % self.total
                return {"ok": False, "err": self.err, "received": self.received}
            if len(data) > room:
                data = data[:room]
                self.ok = False
                self.err = "对方多发，截到 total=%d（后面丢掉 %d 字节）" % (self.total, room)
            if self.received + len(data) > MAX_TOTAL_BYTES:
                self.ok = False
                self.err = "超过 %d 字节上限" % MAX_TOTAL_BYTES
                return {"ok": False, "err": self.err, "received": self.received}
            try:
                self.f.write(data)
            except Exception as e:
                self.ok = False
                self.err = "写盘失败 %s: %s" % (type(e).__name__, e)
                return {"ok": False, "err": self.err, "received": self.received}
            self.received += len(data)
            # 流式这边 next_seq 只是"写了几次"的进度计数，不是块号（state 里显示用）
            self.next_seq += 1
            return {"ok": True, "received": self.received,
                    "need_bytes": self.total - self.received}

    def mark_err(self, msg):
        """外部（路由）发现的问题：标成"这段不完整"，让收尾走残件那条路。"""
        with self.lock:
            self.ok = False
            self.err = msg

    def close(self):
        """收尾：VAD 的正常短句可小于声明上限；其它残件一律不进识别。"""
        short_ok = (self.vad_short and self.received >= VAD_MIN_BYTES and
                    self.received < self.total and self.received % 2048 == 0)
        complete = (self.received > 0 and self.ok and
                    (self.received == self.total or short_ok))
        try:
            if self.received != self.total:
                self.f.seek(0)
                self.f.write(_wav_header(self.rate, self.bits, self.ch, self.received))
            self.f.close()
        except Exception as exc:
            complete = False
            self.err = "关闭录音文件失败：%s" % type(exc).__name__
            try:
                self.f.close()
            except Exception:
                pass
        self.complete = complete
        self.actual_total = self.received if complete else self.total
        return complete


def _new_session(sid, rate, bits, ch, total, vad_short=False):
    _ensure_archive()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,96}", sid):
        return None, "sess 只能包含字母、数字、下划线和短横线（最多96位）"
    # 清掉死会话，免得越攒越多
    now = time.time()
    for k in list(_sessions.keys()):
        s = _sessions[k]
        if now - s.last_at > SESSION_IDLE_SEC:
            try:
                s.close()
            except Exception:
                pass
            del _sessions[k]
    if len(_sessions) >= MAX_SESSIONS:
        return None, "同时在收的会话太多（%d）" % len(_sessions)
    s = Session(sid, rate, bits, ch, total, vad_short)
    _sessions[sid] = s
    return s, None


# ==================================================================
# 收块 / 收尾
# ==================================================================
def feed_chunk(sid, seq, data, rate=None, bits=None, ch=None, total=None):
    """收一块。sid 第一次出现时用 rate/bits/ch/total 建会话。"""
    if not sid:
        return {"ok": False, "err": "缺 sess"}
    with _lock:
        s = _sessions.get(sid)
        if s is None:
            try:
                total = int(total)
            except (TypeError, ValueError):
                return {"ok": False, "err": "新会话必须带 total（总字节数）"}
            if total <= 0 or total > MAX_TOTAL_BYTES:
                return {"ok": False, "err": "total 不合法（%s）" % total}
            s, err = _new_session(sid,
                                  int(rate or DEFAULT_RATE),
                                  int(bits or DEFAULT_BITS),
                                  int(ch or DEFAULT_CH),
                                  total)
            if s is None:
                return {"ok": False, "err": err}
            print("[AUDIO] 新会话 %s：%.1f 秒 @%dHz/%dbit/%dch，共 %d 字节"
                  % (sid, total / float(int(rate or DEFAULT_RATE) * max(1, int(bits or 16) // 8) * int(ch or 1)),
                     int(rate or DEFAULT_RATE), int(bits or DEFAULT_BITS),
                     int(ch or DEFAULT_CH), total))
        return s.append(int(seq), data)


# ==================================================================
# 流式上行（P3，见 合作表 §4.6）
# ==================================================================
def open_stream(sid, rate=None, bits=None, ch=None, total=None, vad_short=False):
    """流式上行开口：给 `POST /api/audio/stream` 建会话，正文用 `Session.append_raw` 灌。

    ★ 为什么走流式，而不是每块一次 POST：werkzeug 的开发服务器**不支持 keep-alive**
      （`WSGIRequestHandler.protocol_version = "HTTP/1.0"`，每个响应都带
      `Connection: close` —— 三种绕法都实测过），⇒ 每块一次 POST 就是**每块一次全新的
      TCP 握手**。10 秒录音 156 块 = 156 次握手，而 ESP8266 走 WiFi 握手远比传 2048 B 贵。

    ★ `sess` **必须每次录音都是新的**：这里拒绝复用还在收的会话 —— 流式没有 seq，
      复用会把第二段录音**静默接**在第一段后面，这正是最难查的那种坏法。

    ★ **不做断点续传**，是做不到而不是没做：F407 那边只有 16KB 环形缓冲，
      已经推给 ESP 的音频它自己也拿不回来了，断了就只能重说一遍。
    """
    if not sid:
        return None, "缺 sess"
    try:
        total_i = int(total)
    except (TypeError, ValueError):
        return None, "流式上行的第一个请求必须带 total（总字节数）"
    if total_i <= 0 or total_i > MAX_TOTAL_BYTES:
        return None, "total 不合法（%s）" % total
    with _lock:
        if sid in _sessions:
            return None, "这个 sess 还在收（%s）—— 每次录音请换一个新的 sess" % sid
        s, err = _new_session(sid, int(rate or DEFAULT_RATE),
                              int(bits or DEFAULT_BITS), int(ch or DEFAULT_CH), total_i,
                              vad_short=vad_short)
    if s is None:
        return None, err
    bps = s.rate * max(1, s.bits // 8) * s.ch
    print("[AUDIO] 新流式会话 %s：%.1f 秒 @%dHz/%dbit/%dch，共 %d 字节%s"
          % (sid, total_i / float(bps), s.rate, s.bits, s.ch, total_i,
             "（VAD 可正常短句结束）" if s.vad_short else ""))
    return s, None


def finish(sid, inject_cfg=None, samples=None):
    """收尾。★ **立刻返回**，识别丢后台（见模块头第 4 条）。"""
    with _lock:
        s = _sessions.pop(sid, None)
    if s is None:
        return {"ok": False, "err": "没有这个会话：%s" % sid}
    complete = s.close()
    got = s.received
    expect = s.total
    actual_total = getattr(s, "actual_total", expect)
    out = {"ok": True, "sess": sid, "received": got, "total": actual_total,
           "declared_total": expect,
           "vad_short": s.vad_short,
           "dur": round(got / float(s.rate * max(1, s.bits // 8) * s.ch), 3),
           "complete": complete, "saved": s.path}
    if samples is not None:
        out["samples_claimed"] = samples
    if not complete:
        out["err"] = s.err or ("收不齐（%d/%d 字节）—— 不送识别" % (got, expect))
        _set_last({"id": s.record_id, "sess": sid, "at": _now(), "saved": s.path, "complete": False,
                   "received": got, "total": actual_total, "declared_total": expect,
                   "vad_short": s.vad_short, "dur": out["dur"],
                   "text": "", "inject": "skipped", "err": out["err"]})
        print("[AUDIO] %s 收不齐（%d/%d）⇒ 残件留在 %s，**不送识别**"
              % (sid, got, expect, s.path))
        return out
    _set_last({"id": s.record_id, "sess": sid, "at": _now(), "saved": s.path, "complete": True,
               "received": got, "total": actual_total, "declared_total": expect,
               "vad_short": s.vad_short, "dur": out["dur"],
               "text": "", "inject": "queued", "err": None})
    out["queued"] = True
    print("[AUDIO] %s 收齐 %d 字节（%.2f 秒）⇒ 丢后台识别，结果看 /api/audio/status"
          % (sid, got, out["dur"]))
    _jobs.put((s, inject_cfg))
    _start_worker()
    return out


# ==================================================================
# 识别 → 回灌
# ==================================================================
def _ensure_archive():
    directory = os.path.realpath(WAV_DIR)
    with _archive_lock:
        if directory not in _archive_dirs:
            audio_archive.import_existing(directory)
            _apply_retention()
            _archive_dirs.add(directory)


def _apply_retention():
    settings = audio_retention.get()
    if settings["enabled"]:
        return audio_archive.prune(WAV_DIR, settings["limit"])
    return {"deleted": 0, "protected": 0, "failed": 0}


def set_retention(limit):
    _ensure_archive()
    settings = audio_retention.save(limit)
    return dict(settings, **_apply_retention())


def history(offset=0):
    _ensure_archive()
    return audio_archive.page(WAV_DIR, offset)


def recording(key):
    _ensure_archive()
    return audio_archive.get(WAV_DIR, key)


def _start_worker():
    global _worker
    with _worker_lock:
        if _worker is None or not _worker.is_alive():
            _worker = threading.Thread(target=_pipeline_worker, name="audio-fifo", daemon=True)
            _worker.start()


def _pipeline_worker():
    while True:
        session, cfg = _jobs.get()
        try:
            rec = recording(session.record_id)
            if rec:
                rec["inject"] = "processing"
                _set_last_if_current(rec)
            _pipeline(session, cfg)
        except Exception as exc:
            rec = recording(session.record_id)
            if rec:
                rec.update(inject="skipped", err="处理失败：" + type(exc).__name__)
                _set_last_if_current(rec)
        finally:
            _jobs.task_done()


def _pipeline(s, inject_cfg):
    global _pending
    try:
        r = asr.transcribe(s.path)
    except Exception as exc:
        r = {"error": "识别失败：%s" % type(exc).__name__}
    text = (r.get("text") or "").strip()
    mode = get_mode()
    rec = {"id": s.record_id, "sess": s.sid, "at": _now(), "saved": s.path, "complete": True,
           "received": s.received, "total": getattr(s, "actual_total", s.total),
           "declared_total": s.total,
           "vad_short": s.vad_short,
           "dur": round(s.received / float(s.rate * max(1, s.bits // 8) * s.ch), 3),
           "text": text, "asr_sec": r.get("sec"), "vad": r.get("vad"),
           "inject": "none", "err": r.get("error"), "asr_error_code": r.get("error_code")}
    print("[AUDIO] %s 识别完成（%s 秒）：%r" % (s.sid, r.get("sec"), text))

    if r.get("error"):
        rec["inject"] = "skipped"
    elif not text:
        rec["inject"] = "skipped"
        rec["err"] = "识别结果为空（可能没说清 / 全是静音）"
    elif mode == "auto":
        rec["inject"] = "sending"
        _set_last_if_current(rec)
        ok, detail = inject(text, inject_cfg)
        rec["inject"] = "auto" if ok else "auto_failed"
        if not ok:
            rec["err"] = detail
    else:
        rec["inject"] = "pending"
        print("[AUDIO] 手动模式 ⇒ 挂在 /api/audio/status 等网页确认（不是自动发出）")

    _set_last_if_current(rec)


def _set_last_if_current(rec):
    global _last, _pending
    _ensure_archive()
    audio_archive.save(WAV_DIR, rec)
    _apply_retention()
    pending = audio_archive.pending(WAV_DIR)
    with _lock:
        if _last and _last.get("id") == rec.get("id"):
            _last = dict(rec)
        _pending = dict(pending[0]) if pending else None


def queue_pending(inject_cfg, record_id=None):
    global _sending, _send_result
    _ensure_archive()
    candidates = audio_archive.pending(WAV_DIR)
    item = recording(record_id) if record_id else (candidates[0] if candidates else None)
    if not item or item.get("inject") not in ("pending", "manual_failed", "auto_failed") or not item.get("text"):
        return {"ok": False, "err": "这条录音没有待发送的识别文字"}
    with _lock:
        if _sending:
            return {"ok": False, "err": "正在发送，请等待结果"}
        _sending = dict(item)
        _send_result = None
    item["inject"] = "sending"
    _set_last_if_current(item)
    threading.Thread(target=_send_worker, args=(item, inject_cfg), daemon=True).start()
    return {"ok": True, "queued": True, "text": item["text"], "id": item["id"]}


def _send_worker(item, cfg):
    global _sending, _send_result
    try:
        ok, detail = inject(item["text"], cfg)
    except Exception as exc:
        ok, detail = False, "发送失败：%s" % type(exc).__name__
    item.update(inject="manual" if ok else "manual_failed", err=None if ok else detail)
    _set_last_if_current(item)
    with _lock:
        _sending = None
        _send_result = {"ok": ok, "detail": detail, "sess": item["sess"], "id": item["id"]}


def inject(text, cfg):
    """把文字 POST 给本机 /api/data —— 与 `语音识别/语音识别.py::upload()` **同一条路、
    同一份 body 形状**（那条已经手工验过）。★ 只打 URL，绝不打印 token。"""
    if not text:
        return False, "文字为空"
    if not cfg or not cfg.get("url"):
        return False, "没拿到注入配置（app.py 没传 url）"
    try:
        import requests
        body = {"token": cfg.get("token", ""),
                "user": text,
                "device_id": cfg.get("device_id", ""),
                "mode": "full",
                "enqueue_decision": True,
                "need_ai": True}
        r = requests.post(cfg["url"], json=body, timeout=60)
        if r.status_code != 200:
            return False, "注入失败 HTTP %d：%s" % (r.status_code, r.text[:200])
        result = r.json()
        if not result.get("queued_to_device"):
            return False, "服务器未确认指令入队，请确认本地服务已更新"
        print("[AUDIO] 已回灌 /api/data（HTTP 200）")
        return True, "HTTP 200"
    except Exception as e:
        return False, "注入异常 %s: %s" % (type(e).__name__, str(e)[:200])


def send_pending(inject_cfg):
    """Legacy synchronous caller; production web uses queue_pending."""
    _ensure_archive()
    items = audio_archive.pending(WAV_DIR)
    if not items:
        return {"ok": False, "err": "没有待确认文字"}
    item = items[0]
    ok, detail = inject(item["text"], inject_cfg)
    item.update(inject="manual" if ok else "manual_failed", err=None if ok else detail)
    _set_last_if_current(item)
    return {"ok": ok, "text": item["text"], "detail": detail}


def drop_pending(record_id=None):
    _ensure_archive()
    with _lock:
        if _sending and (not record_id or _sending.get("id") == record_id):
            return {"ok": False, "err": "正在发送，暂时不能丢弃"}
    items = audio_archive.pending(WAV_DIR)
    item = recording(record_id) if record_id else (items[0] if items else None)
    if item and item.get("inject") in ("pending", "manual_failed", "auto_failed"):
        item["inject"] = "dropped"
        _set_last_if_current(item)
        return {"ok": True, "dropped": True}
    return {"ok": True, "dropped": False}


# ==================================================================
# 状态
# ==================================================================
def _now():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def _set_last(rec):
    global _last
    _ensure_archive()
    audio_archive.save(WAV_DIR, rec)
    _apply_retention()
    with _lock:
        _last = dict(rec)


def status():
    global _last, _pending
    archive = history()
    pending = audio_archive.pending(WAV_DIR)
    with _lock:
        if not _last and archive["items"]:
            _last = dict(archive["items"][0])
        _pending = dict(pending[0]) if pending else None
        last = dict(_last) if _last else None
        pend = dict(_pending) if _pending else None
        sending = dict(_sending) if _sending else None
        send_result = dict(_send_result) if _send_result else None
        live = [{"sess": s.sid, "received": s.received, "total": s.total,
                 "vad_short": s.vad_short,
                 "next_seq": s.next_seq, "age": round(time.time() - s.last_at, 1)}
                for s in _sessions.values()]
    # ★ mode 必须在 _lock **外面**取：get_mode() 自己也要拿 _lock，
    #   而 threading.Lock 不可重入 —— 写在 with 里面会当场死锁（我第一版就是这么写的）。
    return {
        "history": archive["items"],
        "history_total": archive["total"],
        "pending_count": len(pending),
        "processing_queue": _jobs.qsize(),
        "retention": audio_retention.get(),
        "mode": get_mode(),
        "model": asr.model_status(),
        "last": last,
        "pending": pend,
        "sending": sending,
        "send_result": send_result,
        "live": live,
        "wav_dir": WAV_DIR,
    }
