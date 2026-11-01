# -*- coding: utf-8 -*-
"""P3 推流自测：ESP→PC 这条音频上行扛不扛得住 32000 B/s（PC 侧，不需要板子）。

双击 `测试P3推流.bat` 就是跑这个。P3 方案里的 P0 步 —— **先量，再写固件**。

为什么要先量：P3 要把语音链路改成「裸二进制帧 + 921600」，而 ESP→PC 这一段走
`POST /api/audio/stream`（**一次 POST 把整段 PCM 流完**）。这条路一旦不够，整个
ESP 侧设计都要推翻；量一次比事后返工便宜得多。

★★ 它和 `audio_selftest.py`（P2 那套 24 条）的分工：
  · `audio_selftest.py` 用 Flask 的 `test_client()` —— **进程内直调，一个字节都不出网卡**
    ⇒ 它**根本测不出连接行为**，只测业务规矩。
  · 本脚本起私有服务器（werkzeug，`threaded=True`，跟 `app.py:1519` 同款），
    用 `http.client` 走**真 TCP** ⇒ 才量得到连接数、往返、实测吞吐。
  · ⇒ **P2 那套断言仍然要跑**（改动 `audio_rx.Session` 之后尤其要跑）：`测试P2语音链路.bat`。

★ 为什么是流式而不是"每块一次 POST"（这是实测定的，不是拍的）：
  werkzeug 3.1.8 的开发服务器**不支持 keep-alive**（`protocol_version = "HTTP/1.0"`，
  每个响应都带 `Connection: close`；客户端显式要 keep-alive、把 handler 改成 HTTP/1.1
  —— 两种绕法都实测无效），⇒ 每块一次 POST 就是**每块一次全新的 TCP 握手**。
  10 秒录音 156 块 = 156 次握手，而 ESP8266 走 WiFi，握手远比传 2048 B 贵。
  ⇒ 改成一次 POST 流式推完 = **一次录音 = 一个请求 = 一条连接**。

★ 哪些刀走真 TCP、哪些走进程内（**因为鉴权不一样**，见 `app.py:265-294`）：
  · `/api/audio/stream`、`/api/audio/chunk`、`/api/audio/end` —— **ESP 发的，
    query 带 token 就放行** ⇒ **量的这几刀全走真 TCP**（这才有连接数可言）。
  · `/api/audio/status`、`/api/audio/mode` —— **给网页控制台的，必须登录**
    ⇒ 脚本登不了真 5000（口令在 config.json），改用**进程内 test_client** 读/切。
    它们是"读表"和"设置"，**不参与传输测量**，混用不影响结论。

★ 输出里有两档，别混：
  · `[PASS]`/`[FAIL]` —— **进退出码**：吞吐、收齐、逐字节、陷阱用例这些**代码该做到**的事。
  · `[OK]`/`[注意]` —— **不进退出码**：连接数这种**服务器自身行为**的观察值。

★ 四条注意：
  1. **绝不打印 token**（config.json 里的那个）。
  2. 它**不起真 5000**，用私有端口（正式 `app.py` 开着也能跑）。代价是
     **可能再加载一份识别模型**（内存翻倍）—— 不想加载就 `SKIP_ASR=1`。
  3. 它**不触发定时唤醒**（`start_auto_tick` 被临时换掉），并且**强制切手动模式**
     —— 免得自测顺手花你的 AI 钱。
  4. 落盘的 `rec_*_p3*.wav` 跑完自己删；**失败时留着**并告诉你路径。

退出码：0 = 全绿；1 = 有 FAIL（详见输出）。
"""
import http.client
import json
import logging
import os
import struct
import sys
import threading
import time
import wave

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJ = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)
os.chdir(_HERE)                    # app.load_config() 按相对路径找 config.json

SKIP_ASR = str(os.environ.get("SKIP_ASR", "")).strip() not in ("", "0", "false")

# ★ inmp441.h:92 —— 16kHz / 16bit / 单声道 = 32000 B/s。整条链路的靶子就是这个数。
REQ_RATE = 32000
SECONDS = 10                       # 用户拍板"先按 10 秒跑稳"
HALF_BLOCK = 1024                  # 半块 = 512 样本 = 1024 B = 32 ms（inmp441.h:93）
CHUNK = 2048                       # 链路内部块大小（F407→ESP 那一跳），64 ms
SID = "p3"                         # 会话号前缀（每次都不一样，见 陷阱③）

RESULT = []
CREATED = []
CLOSE_COUNT = [0]                  # 服务器回 Connection: close 的总次数


def chk(name, cond, extra=""):
    RESULT.append(bool(cond))
    print("%s %s %s" % ("[PASS]" if cond else "[FAIL]", name, extra))
    return bool(cond)


def note(name, cond, extra=""):
    """★ 只报告、**不进退出码**。

    像"服务器每块都关连接"这种是**服务器本身的行为**（不是代码缺陷），
    混进 FAIL 会让人去查一个不存在的 bug。
    """
    print("%s %s %s" % ("[OK]" if cond else "[注意]", name, extra))
    return bool(cond)


def find_src_wav():
    """找一条能识别的源 wav。优先用那条已验证过的固定件。（抄 audio_selftest.py:57-70）"""
    fixed = os.path.join(_PROJ, "语音识别", "wav", "测试_你好我是机器人.wav")
    if os.path.isfile(fixed):
        return fixed
    cands = []
    for d in (os.path.join(_PROJ, "语音识别", "wav"),
              os.path.join(_PROJ, "ST32", "AIAssistant", "Tools", "micrec")):
        if os.path.isdir(d):
            cands += [os.path.join(d, f) for f in os.listdir(d) if f.lower().endswith(".wav")]
    cands = [c for c in cands if "selftest" not in c.lower() and "rec_" not in c]
    if not cands:
        return None
    return max(cands, key=os.path.getmtime)


def total_for(seconds):
    """★ 这几行是 P3 最容易埋雷的地方，所以自测也用它算，而不是写死 319488。

    `audio_rx.py` 判收齐用的是 `received == total`，对不上就**不送识别** —— 而且
    **不报错**。数据颗粒度是半块 32 ms，10 秒 = 312.5 个半块，凑不齐 ⇒ 必须向下取整：
    312 半块 = 319488 B = 9.984 秒。写 `seconds * 32000` = 320000 就永远收不齐：
    录 10 秒、零报错、**永远不出字**。（下面 陷阱① 专门把这条钉住。）
    """
    return (seconds * 1000 // 32) * HALF_BLOCK


def tile(pcm, nbytes):
    """把源 PCM 首尾接起来凑够 nbytes（只做吞吐测试，内容无所谓）。"""
    if not pcm:
        return b""
    out = bytearray()
    while len(out) < nbytes:
        out += pcm[:min(len(pcm), nbytes - len(out))]
    return bytes(out)


def paced(data, chunk, rate):
    """按音频真实节拍吐：chunk/rate 秒一块。"""
    per = chunk / float(rate)
    t0 = time.perf_counter()
    sent = 0
    while sent < len(data):
        k = sent // chunk
        d = t0 + k * per - time.perf_counter()
        if d > 0:
            time.sleep(d)
        yield data[sent:sent + chunk]
        sent += chunk


def fast(data, chunk):
    sent = 0
    while sent < len(data):
        yield data[sent:sent + chunk]
        sent += chunk


class Pusher(object):
    """真 TCP 客户端。★ 全程只 new 一次 HTTPConnection —— 量的就是"用了几条连接"。"""

    def __init__(self, port, token):
        self.port = port
        self.token = token
        self.conn = http.client.HTTPConnection("127.0.0.1", port, timeout=120)
        self.posts = 0
        self.conns_opened = 0      # 这一刀开始时连上没有连接 ⇒ 必然新建了一条
        self.server_closes = 0     # 服务器回 Connection: close 的次数
        self.rtt = []

    def post(self, path, body):
        t0 = time.perf_counter()
        if self.conn.sock is None:
            self.conns_opened += 1
        # ★ 故意**不发** Connection: close —— 本自测量的一部分就是连接复用。
        #   body 给生成器 ⇒ http.client 自动用 Transfer-Encoding: chunked（流式就靠它）。
        self.conn.request("POST", path, body=body,
                          headers={"Content-Type": "application/octet-stream"})
        resp = self.conn.getresponse()
        data = resp.read()
        dt = time.perf_counter() - t0
        self.posts += 1
        self.rtt.append(dt)
        if str(resp.getheader("Connection") or "").lower() == "close":
            self.server_closes += 1
            CLOSE_COUNT[0] += 1
        try:
            js = json.loads(data.decode("utf-8"))
        except Exception:
            js = {}
        return resp.status, js, dt

    def close(self):
        try:
            self.conn.close()
        except Exception:
            pass

    def keepalive_ok(self):
        return self.server_closes == 0 and self.conns_opened <= 1

    def conn_note(self):
        if self.conns_opened <= 1:
            return "**全程 %d 条连接**" % max(1, self.conns_opened)
        return ("**新增 %d 条 TCP 连接**（服务器每块都 `Connection: close`）"
                % self.conns_opened)


def stream_url(port, sid, total, rate=16000, bits=16, ch=1, token=""):
    return ("/api/audio/stream?sess=%s&rate=%d&bits=%d&ch=%d&total=%d&token=%s"
            % (sid, rate, bits, ch, total, token))


def main():
    print("=" * 66)
    print(" P3 推流自测：一次 POST 流式推完，够不够 32000 B/s（PC 侧，不需要板子）")
    print("=" * 66)

    try:
        import app as A
        from werkzeug.serving import make_server
    except Exception as e:
        print("[FAIL] 起不来：import 失败 %s: %s" % (type(e).__name__, e))
        print("       ⇒ 确认 Flask 那几个包装在 D:\\Python 下（见 flask_relay/使用说明.md）")
        return 1

    token = str((A.load_config() or {}).get("token", "") or "")
    print("token : %s（**不打印内容**）" % ("有" if token else "★ 空"))
    if not token:
        print("[FAIL] config.json 里没有 token ⇒ 上行会被 401 拦掉，先配上再跑")
        return 1

    src = find_src_wav()
    if not src:
        print("[FAIL] 找不到可用的源 wav ⇒ 放一个 .wav 到 语音识别\\wav\\ 再跑")
        return 1
    with wave.open(src, "rb") as w:
        s_rate, s_ch, s_bits = w.getframerate(), w.getnchannels(), w.getsampwidth() * 8
        s_pcm = w.readframes(w.getnframes())
    s_bps = s_rate * s_ch * s_bits // 8
    print("源音频: %s" % os.path.relpath(src, _PROJ))
    print("        %dHz %dch %dbit  %d 字节  %.2f 秒"
          % (s_rate, s_ch, s_bits, len(s_pcm), len(s_pcm) / float(s_bps)))
    print("靶子  : %d B/s（16kHz/16bit/单声道，inmp441.h:92）" % REQ_RATE)
    print("模型  : %s" % ("跳过（SKIP_ASR=1）" if SKIP_ASR else "会真跑一次识别"))

    # ---- 起私有服务器（真 TCP）；顺手掐掉定时唤醒，别花用户的钱 ----
    logging.getLogger("werkzeug").setLevel(logging.ERROR)
    A.start_auto_tick = lambda: None
    if SKIP_ASR:
        A.asr.warmup = lambda: None
    srv = make_server("127.0.0.1", 0, A.app, threaded=True)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("私有服务器: http://127.0.0.1:%d（不占 5000，正式 app.py 开着也能跑）" % port)
    print("★ 不触发定时唤醒；若正式 app.py 也开着，会各加载一份模型")

    # ★ status / mode 要登录态，走进程内 test_client（见模块头）
    tc = A.app.test_client()
    with tc.session_transaction() as s:
        s["authed"] = True

    def status():
        return tc.get("/api/audio/status").get_json() or {}

    def set_mode(m):
        return tc.post("/api/audio/mode", json={"mode": m}).get_json() or {}

    orig_mode = status().get("mode") or "manual"
    set_mode("manual")
    print("模式  : 强制 manual（原为 %s，跑完还原）—— 识别结果不自动回灌" % orig_mode)
    print("-" * 66)

    total = total_for(SECONDS)
    audio = tile(s_pcm, total)
    try:
        # ---- ① 鉴权：没 token 必须被拦 ----
        for path, label in (("/api/audio/stream?sess=x&total=8", "stream"),
                            ("/api/audio/chunk?sess=x&seq=0&total=8", "chunk")):
            c0 = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            c0.request("POST", path, body=b"12345678")
            r0 = c0.getresponse()
            r0.read()
            chk("没带 token 的 %s 被拦（401）" % label, r0.status == 401, "→ %d" % r0.status)
            c0.close()

        # ---- ② 流式：上限（不限速，量服务端读通路有多少余量）----
        p = Pusher(port, token)
        st, js, dt = p.post(stream_url(port, SID + "fast", total, token=token), fast(audio, CHUNK))
        rate = js.get("received", 0) / dt if dt > 0 else 0
        print("② 不限速推满 10 秒：客户端墙钟 %.3f s ⇒ 上限 %.1f MB/s（靶子 %.2f×）"
              % (dt, rate / 1e6, rate / float(REQ_RATE)))
        chk("② 不限速：收齐 319488", st == 200 and js.get("complete") is True,
            "→ %s" % json.dumps({k: js.get(k) for k in ("complete", "received", "total", "err")},
                                ensure_ascii=False))
        note("② 连接数（一次请求就该只有一条）", p.keepalive_ok(), p.conn_note())
        if js.get("saved"):
            CREATED.append(js["saved"])
        p.close()

        # ---- ②b 手动固定录音：已知总长，ESP 用 Content-Length + 原始 PCM ----
        # Bytes body makes http.client emit Content-Length (not chunked).  This is
        # deliberately distinct from the generator cases above: a 10-second manual
        # recording does know its exact size when START arrives.
        p = Pusher(port, token)
        st, js, dt = p.post(stream_url(port, SID + "length", total, token=token), audio)
        chk("②b Content-Length 原始PCM：收齐且 complete",
            st == 200 and js.get("complete") is True and js.get("received") == total,
            "→ %s" % json.dumps({k: js.get(k) for k in ("complete", "received", "total", "err")},
                                  ensure_ascii=False))
        if js.get("saved"):
            CREATED.append(js["saved"])
        p.close()

        # ---- ③ 流式：按真实节拍（主项 —— ESP 就长这样）----
        sid3 = SID + "paced"
        p = Pusher(port, token)
        st, js, dt = p.post(stream_url(port, sid3, total, token=token),
                            paced(audio, CHUNK, REQ_RATE))
        print("③ 按 32000 B/s 真实节拍推 %.3f 秒音频：客户端墙钟 %.3f s" % (total / float(REQ_RATE), dt))
        chk("③ 真实节拍：收齐且 complete", js.get("complete") is True and js.get("received") == total,
            "→ received=%s/%s %s" % (js.get("received"), js.get("total"), js.get("err") or ""))
        chk("③ 真实节拍：跟得上（墙钟 ≤ 音频时长×1.1）", dt <= total / float(REQ_RATE) * 1.1,
            "→ %.3f s vs %.3f s" % (dt, total / float(REQ_RATE)))
        chk("③ wrote == total（服务端读到的字节数）", js.get("wrote") == total,
            "→ wrote=%s" % js.get("wrote"))
        note("③ 连接数", p.keepalive_ok(), p.conn_note())
        saved3 = js.get("saved")
        if saved3:
            CREATED.append(saved3)
            fsize = os.path.getsize(saved3)
            chk("③ 落盘 wav = 44 + %d" % total, fsize == 44 + total, "→ %d 字节" % fsize)
            with open(saved3, "rb") as f:
                head, body = f.read(44), f.read()
            chk("③ wav 头（RIFF/WAVE/fmt/data + 长度字段）",
                head[:4] == b"RIFF" and head[8:12] == b"WAVE" and head[36:40] == b"data"
                and struct.unpack("<I", head[40:44])[0] == total)
            chk("③ 正文**逐字节**等于源 PCM（边收边写没写坏一个字节）", body == audio)
        p.close()
        print("-" * 66)

        # ---- ④ 陷阱①：total 写死 seconds*32000 ⇒ 永远收不齐、零报错 ----
        # ★ 这是 P3 最阴的坑，所以钉成常驻用例：故意声明 320000，只发 319488。
        bad_total = SECONDS * REQ_RATE
        p = Pusher(port, token)
        st, js, dt = p.post(stream_url(port, SID + "bad", bad_total, token=token),
                            fast(audio, CHUNK))
        chk("陷阱① total 写死 %d（=秒×32000）⇒ 判**收不齐**、不送识别"
            % bad_total,
            js.get("complete") is False and js.get("received") == total
            and "queued" not in js,
            "→ %s" % json.dumps({k: js.get(k) for k in ("complete", "received", "total", "err")},
                                ensure_ascii=False))
        if js.get("saved"):
            CREATED.append(js["saved"])
        p.close()

        # ---- ⑤ 陷阱②：中途断了 ⇒ 残件能播、不送识别、会话位释放 ----
        sid5 = SID + "cut"
        part = 65536
        p = Pusher(port, token)
        st, js, dt = p.post(stream_url(port, sid5, total, token=token),
                            fast(audio[:part], CHUNK))
        chk("陷阱② 只发 %d/%d ⇒ complete=False 且不排队识别" % (part, total),
            js.get("complete") is False and "queued" not in js,
            "→ %s" % json.dumps({k: js.get(k) for k in ("complete", "received", "err")},
                                ensure_ascii=False))
        sp = js.get("saved")
        if sp:
            CREATED.append(sp)
            chk("陷阱② 残件按实收改短（能播）", os.path.getsize(sp) == 44 + part,
                "→ %d 字节" % os.path.getsize(sp))
        p.close()
        # 会话位必须已经释放（否则连着几次失败就把 MAX_SESSIONS=4 占满）
        live = [x.get("sess") for x in status().get("live") or []]
        chk("陷阱② 失败后会话位已释放（live 里没有它）", sid5 not in live, "→ live=%s" % live)

        # ---- ⑥ 陷阱③：并发复用同一个 sess ⇒ 必须拒（两条流交叉写会废掉 wav）----
        sid6 = SID + "same"
        holder = {}

        def slow_stream():
            pp = Pusher(port, token)
            small = total_for(1)            # 1 秒，够第二个请求挤进来
            tiny = tile(s_pcm, small)
            holder["st"], holder["js"], _ = pp.post(
                stream_url(port, sid6, small, token=token),
                paced(tiny, CHUNK, REQ_RATE))
            pp.close()

        th = threading.Thread(target=slow_stream, daemon=True)
        th.start()
        time.sleep(0.35)
        p = Pusher(port, token)
        st, js, dt = p.post(stream_url(port, sid6, total_for(1), token=token), b"")
        chk("陷阱③ 同一个 sess 并发第二条流 ⇒ 409 拒绝",
            st == 409, "→ %d %s" % (st, json.dumps(js, ensure_ascii=False)[:100]))
        p.close()
        th.join(20)
        chk("陷阱③ 第一条流本身正常收齐", (holder.get("js") or {}).get("complete") is True,
            "→ %s" % json.dumps({k: (holder.get("js") or {}).get(k)
                                 for k in ("complete", "received")}, ensure_ascii=False))
        if (holder.get("js") or {}).get("saved"):
            CREATED.append(holder["js"]["saved"])
        # 先后复用同一个 sess 是**无害**的（上一次已落盘，会开新会话新文件）—— 也钉一下
        p = Pusher(port, token)
        st, js, dt = p.post(stream_url(port, sid6, total_for(1), token=token),
                            fast(tile(s_pcm, total_for(1)), CHUNK))
        chk("陷阱③ 先后复用同一个 sess 无害（开新会话、照常收齐）",
            js.get("complete") is True and st == 200,
            "→ %s" % json.dumps({k: js.get(k) for k in ("complete", "received")},
                                ensure_ascii=False))
        if js.get("saved"):
            CREATED.append(js["saved"])
        p.close()
        print("-" * 66)

        # ---- ⑦ 对照：老的"每块一次 POST"通路（P2 的，已退为对照组）----
        # 只跑一次 2048 B，为了把**连接数**并排摆出来；P2 那 24 条断言在 audio_selftest.py
        sid7 = SID + "chunk"
        p = Pusher(port, token)
        n, bad, first_bad = 0, 0, ""
        pos = 0
        while pos < len(audio):
            piece = audio[pos:pos + CHUNK]
            if n == 0:
                q = ("/api/audio/chunk?sess=%s&seq=0&rate=16000&bits=16&ch=1&total=%d&token=%s"
                     % (sid7, total, token))
            else:
                q = "/api/audio/chunk?sess=%s&seq=%d&token=%s" % (sid7, n, token)
            stt, jj, _ = p.post(q, piece)
            if stt != 200:
                bad += 1
                if bad == 1:
                    first_bad = "seq=%d → %d %s" % (n, stt, json.dumps(jj, ensure_ascii=False)[:80])
            pos += len(piece)
            n += 1
        chk("⑦ 对照：分块通路 %d 块全部 200" % n, bad == 0, first_bad)
        note("⑦ 对照：分块通路的连接数（这就是被换掉的原因）", p.keepalive_ok(), p.conn_note())
        st, js, dt = p.post("/api/audio/end?sess=%s&token=%s" % (sid7, token), b"")
        chk("⑦ 对照：/end 判收齐", js.get("complete") is True,
            "→ %s" % json.dumps({k: js.get(k) for k in ("complete", "received")},
                                ensure_ascii=False))
        if js.get("saved"):
            CREATED.append(js["saved"])
        p.close()

        # ---- ⑧ 端到端：真语音（放最后，免得前面那些拼接音频抢 last / 占识别队列）----
        want = (len(s_pcm) // CHUNK) * CHUNK      # 取块整数倍，丢尾巴几个字节
        sid8 = SID + "e2e"
        p = Pusher(port, token)
        st, js, dt = p.post(
            stream_url(port, sid8, want, s_rate, s_bits, s_ch, token),
            fast(s_pcm[:want], CHUNK))
        chk("⑧ 端到端：真语音 %d 字节收齐" % want, js.get("complete") is True,
            "→ %s" % json.dumps({k: js.get(k) for k in ("complete", "received", "dur", "err")},
                                ensure_ascii=False))
        note("⑧ 端到端：连接数", p.keepalive_ok(), p.conn_note())
        if js.get("saved"):
            CREATED.append(js["saved"])
        p.close()

        if SKIP_ASR:
            print("[SKIP] 跳过识别（SKIP_ASR=1）")
        else:
            print("等后台识别", end="")
            last = {}
            for _ in range(120):          # 最多等 60 秒（前面几个拼接音频的识别排在它前面）
                time.sleep(0.5)
                x = status().get("last") or {}
                if x.get("sess") == sid8 and (x.get("text") or x.get("err")):
                    last = x
                    break
                print(".", end="")
                sys.stdout.flush()
            print()
            chk("⑧ 端到端：后台识别出文字", bool(last.get("text")),
                "→ %r%s" % (last.get("text"),
                            ("  err=%s" % last.get("err")) if last.get("err") else ""))
            chk("⑧ 手动模式 ⇒ 挂起等确认（没自动发出去）", last.get("inject") == "pending",
                "→ inject=%s" % last.get("inject"))
            chk("⑧ 挂起队列里就是要发的那句",
                (status().get("pending") or {}).get("text") == last.get("text"),
                "→ %r" % ((status().get("pending") or {}).get("text"),))
        print("-" * 66)
    finally:
        try:
            set_mode(orig_mode)
        except Exception:
            pass
        srv.shutdown()

    ok = all(RESULT)
    print("=" * 66)
    if CLOSE_COUNT[0]:
        print("★ 连接结论：服务器**不支持保活** —— 每个响应都带 `Connection: close`（共 %d 次）"
              % CLOSE_COUNT[0])
        print("  （werkzeug 3.1.8 的 `WSGIRequestHandler.protocol_version = \"HTTP/1.0\"`；")
        print("    实测把 handler 改成 HTTP/1.1、或客户端显式要 keep-alive，都没用。）")
        print("  ⇒ **流式那几刀各只有 1 条连接**（一次录音一个请求），这就是换它的原因；")
        print("     ⑦ 那一刀 156 条就是老方案的样子。")
    if ok:
        for q in CREATED:
            try:
                os.remove(q)
            except Exception:
                pass
        print("全绿：%d 项全过 ✅   （自测生成的 wav 已删掉）" % len(RESULT))
        print("★ 记住：这是 **PC 侧上限**。ESP8266 单核 80MHz 走 WiFi 必然打折 ——")
        print("  余量就是留给它的（实测上限那一行 ÷ 32000 = 有多少倍余量）。")
    else:
        print("★ 有 %d/%d 项没过 —— 上面每条 [FAIL] 后面就是现场"
              % (sum(1 for x in RESULT if not x), len(RESULT)))
        for q in CREATED:
            if os.path.isfile(q):
                print("  留在（可以放来听听收成了什么）：%s" % q)
    print("=" * 66)
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n（被中断）")
        sys.exit(130)


