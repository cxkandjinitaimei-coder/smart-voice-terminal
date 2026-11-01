# -*- coding: utf-8 -*-
"""P2 语音链路自测（PC 侧，**不需要板子**）。

双击 `测试P2语音链路.bat` 就是跑这个。

它做的事：把 `语音识别/wav` 里那个已验证的 wav **切成分块**，一块一块喂给真正的
`/api/audio/chunk`（走 Flask 的 test client，**不占端口、不用起服务器**），
然后检查这条链路的**每一条硬规矩**是不是真的成立 —— 包括几条"错了会很难查"的：

  · 没带 token 的 chunk **必须被拦**（否则同网段谁都能塞音频）
  · 跳号 **必须报 409 + gap**（静默丢块是最难查的故障）
  · 重发已收到的块 **必须算 dup 而不是报错**（ESP 会重传）
  · 落盘的 wav **正文必须逐字节等于源 PCM**（边收边写最容易在这处悄悄写坏）
  · 收不齐 **必须不送识别**（拿半个 wav 去识别会得到"看着正常但内容错"的结果）
  · total 超 30 秒上限 **必须当场拒**（防跑飞的客户端把盘写满）
  · `/end` **必须立刻返回**（ESP 的 HTTP 超时只有 3000ms，而模型加载要 4~5 秒）
  · 模式必须**按当时配置分两路验**：auto（2026-09-23 起是默认）识别完**自动回灌、
    不等确认**；manual 则**挂起等确认、不自动发**
  · 回灌 `/api/data` 的 body 形状 **必须和 `语音识别/语音识别.py::upload()` 一致**

★ 四条注意：
  1. **绝不打印 token**（config.json 里的那个）。
  2. 它会**真的调一次识别模型**（实测首次加载 4~5 秒）—— 这是有意的：不真跑一遍
     就验不出"模型能不能加载"。想跳过就把 `SKIP_ASR=1` 设上。
  3. 它会在 `语音识别/wav` 里生成几个 `rec_*_selftest*.wav`，**跑完自己删掉**；
     失败时**留着**并告诉你路径，方便听一下到底收到的是什么。
  4. **模式进出必须一致**（2026-09-23 修）：本自测以前结尾写死"切回 manual"，
     等于跑一次就把语音模式从 auto 偷偷改成手动 —— 用户的语音全自动就此失效，
     而且现象是"昨天还好好的"。现在进来先记下模式，结束（含异常退出）原样恢复。
     同理，回灌打到的是**本进程里的假服务器**，绝不碰真 5000（否则真 AI 花钱、
     真记忆被塞测试话）；且必须在 `/end` **之前**就把假地址装上 —— auto 的回灌是
     后台线程自己发的，晚一步它就已经打到真地址去了。

退出码：0 = 全绿；1 = 有 FAIL（详见输出）。
"""
import io
import json
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
CHUNK = 4096                       # 故意用个小块：多切几刀才验得出 seq/dup/gap
SID = "selftest01"

RESULT = []
CREATED = []


def chk(name, cond, extra=""):
    RESULT.append(bool(cond))
    print("%s %s %s" % ("[PASS]" if cond else "[FAIL]", name, extra))
    return bool(cond)


def find_src_wav():
    """找一条能识别的源 wav。优先用那条已验证过的固定件。"""
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


def main():
    print("=" * 66)
    print(" P2 语音链路自测（PC 侧，不需要板子）")
    print("=" * 66)

    try:
        import app as A
    except Exception as e:
        print("[FAIL] 起不来：import app 失败 %s: %s" % (type(e).__name__, e))
        print("       ⇒ 确认 Flask 那几个包装在 D:\\Python 下（见 flask_relay/使用说明.md）")
        return 1

    token = str((A.load_config() or {}).get("token", "") or "")
    print("token : %s（**不打印内容**）" % ("有" if token else "★ 空"))
    print("模型  : %s" % ("跳过（SKIP_ASR=1）" if SKIP_ASR else "会真跑一次识别"))

    src = find_src_wav()
    if not src:
        print("[FAIL] 找不到可用的源 wav ⇒ 放一个 .wav 到 语音识别\\wav\\ 再跑")
        return 1
    with wave.open(src, "rb") as w:
        rate, ch, bits = w.getframerate(), w.getnchannels(), w.getsampwidth() * 8
        pcm = w.readframes(w.getnframes())
    print("源音频: %s" % os.path.relpath(src, _PROJ))
    print("        %dHz %dch %dbit  %d 字节  %.2f 秒" % (
        rate, ch, bits, len(pcm), len(pcm) / float(rate * ch * bits // 8)))
    print("-" * 66)

    c = A.app.test_client()

    # ---- 1) 鉴权：没 token 的 chunk 必须被拦 ----
    r = c.post("/api/audio/chunk?sess=x&seq=0&total=8", data=b"12345678")
    chk("没带 token 的 chunk 被拦（401）", r.status_code == 401, "→ %d" % r.status_code)

    # 未登录读 status 也必须被拦（那是给网页控制台的）
    r2 = A.app.test_client().get("/api/audio/status")
    chk("未登录读 /api/audio/status 被拦（401）", r2.status_code == 401, "→ %d" % r2.status_code)

    # ---- 2) 分块灌进去 ----
    n, pos, bad = 0, 0, 0
    while pos < len(pcm):
        piece = pcm[pos:pos + CHUNK]
        if n == 0:
            q = ("/api/audio/chunk?sess=%s&seq=0&rate=%d&bits=%d&ch=%d&total=%d&token=%s"
                 % (SID, rate, bits, ch, len(pcm), token))
        else:
            q = "/api/audio/chunk?sess=%s&seq=%d&token=%s" % (SID, n, token)
        resp = c.post(q, data=piece)
        if resp.status_code != 200:
            bad += 1
            if bad <= 2:
                print("        seq=%d → %d %s" % (n, resp.status_code,
                                                  resp.get_data(as_text=True)[:110]))
        pos += len(piece)
        n += 1
    chk("分块全部 200", bad == 0, "%d 块，失败 %d" % (n, bad))

    # ---- 3) 缺块必须报 gap（不许静默收下）----
    r = c.post("/api/audio/chunk?sess=%s&seq=%d&token=%s" % (SID, n + 3, token), data=b"xx")
    j = r.get_json() or {}
    chk("跳号被识别为缺块（409 + gap）", r.status_code == 409 and j.get("gap") == n,
        "→ %s" % json.dumps(j, ensure_ascii=False))

    # ---- 4) 重发已收到的块 = dup，不算错 ----
    r = c.post("/api/audio/chunk?sess=%s&seq=0&token=%s" % (SID, token), data=b"yy")
    j = r.get_json() or {}
    chk("重发old块 ⇒ dup 且仍 200", r.status_code == 200 and j.get("dup") is True,
        "→ %s" % json.dumps(j, ensure_ascii=False))

    # ---- 5) total 超上限必须当场拒 ----
    r = c.post("/api/audio/chunk?sess=selftest03&seq=0&rate=%d&bits=%d&ch=%d&total=%d&token=%s"
               % (rate, bits, ch, 30 * 16000 * 2 + 1, token), data=b"x")
    chk("total 超 30 秒上限被当场拒（409）", r.status_code == 409,
        "→ %s" % json.dumps(r.get_json() or {}, ensure_ascii=False))

    # ---- 6) 状态接口（改用登录态）----
    with c.session_transaction() as s:
        s["authed"] = True
    j = c.get("/api/audio/status").get_json() or {}
    live = (j.get("live") or [{}])[0]
    chk("status 可读，且在收会话字节数与源一致",
        bool(j.get("live")) and live.get("received") == len(pcm),
        "→ received=%s/%s" % (live.get("received"), len(pcm)))

    # 服务端自己报的模式 = 这次要验哪条路（不猜、不写死）：
    #   auto   → 识别完自动回灌（用户 2026-09-23 定的默认）
    #   manual → 挂起等网页点确认
    mode0 = str(j.get("mode") or "manual")
    print("模式  : %s（跑完原样恢复，见 _restore_mode）" % mode0)

    # ---- 6.5) 假注入端点（**必须在 /end 之前装好**）----
    #  ★ 为什么要这么早：auto 的回灌是 audio_rx 后台识别线程**自己发的**，用的就是
    #     `/end` 那一刻的 cfg。等到第 10 步才把地址换掉，auto 那一发早就打到真 5000 了
    #     —— 真 AI 花钱、真 decision_log 被塞一条测试话、板子队列里多一条命令。
    #  ⚠️ 收尾用 atexit 而不是 try/finally：下面几十行不必整体缩进，
    #     而且正常返回和异常退出它都会跑（异常退出时"模式被改掉"才是最坏的）。
    import atexit
    import http.server
    got = {}

    class _H(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            got["path"] = self.path
            got["body"] = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            # ⚠️ 必须回 `queued_to_device: true`：audio_rx.inject() 靠它判定成功，
            #    只回 {"ok":true} 会被判成 auto_failed —— 那是**假服务器不真**，
            #    不是产品错。真 /api/data 在 enqueue_decision 分支就是回这个字段。
            self.wfile.write(b'{"ok":true,"queued_to_device":true}')

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    backup = A._audio_inject_cfg
    A._audio_inject_cfg = lambda: {"url": "http://127.0.0.1:%d/api/data" % srv.server_address[1],
                                   "token": token, "device_id": A.DEFAULT_DEVICE_ID}

    def _restore_mode():
        """把模式与注入端点原样还回去。★ 这条是**回归**：老版本结尾写死"切回 manual"，
        跑一次自测就把用户的 auto 默认值改掉了（语音全自动静默失效）。"""
        try:
            c.post("/api/audio/mode", json={"mode": mode0})
        except Exception:
            pass
        try:
            A._audio_inject_cfg = backup
            srv.shutdown()
        except Exception:
            pass

    atexit.register(_restore_mode)

    # ---- 7) /end 必须立刻返回 ----
    t0 = time.time()
    r = c.post("/api/audio/end?sess=%s&token=%s" % (SID, token))
    dt = time.time() - t0
    j = r.get_json() or {}
    chk("/end 立刻返回（<0.5s）", dt < 0.5, "→ %.3f 秒" % dt)
    chk("/end 报收齐", j.get("complete") is True and j.get("received") == len(pcm))
    saved = j.get("saved")
    if saved:
        CREATED.append(saved)

    # ---- 8) 落盘 wav 必须逐字节等于源 PCM ----
    size = os.path.getsize(saved) if saved and os.path.isfile(saved) else -1
    chk("落盘 wav = 44 + PCM", size == 44 + len(pcm), "→ %d 字节" % size)
    if size == 44 + len(pcm):
        with open(saved, "rb") as f:
            head, body = f.read(44), f.read()
        chk("wav 头正确（RIFF/WAVE/fmt/data + 长度字段）",
            head[:4] == b"RIFF" and head[8:12] == b"WAVE" and head[36:40] == b"data"
            and struct.unpack("<I", head[40:44])[0] == len(pcm))
        chk("wav 正文逐字节等于源 PCM", body == pcm)

    # ---- 9) 等后台识别 ----
    last = {}
    if SKIP_ASR:
        print("[SKIP] 跳过识别（SKIP_ASR=1）")
    else:
        print("等后台识别", end="")
        for _ in range(80):
            time.sleep(0.5)
            last = (c.get("/api/audio/status").get_json() or {}).get("last") or {}
            if last.get("text") or last.get("err"):
                break
            print(".", end="")
        print()
        chk("后台识别出文字", bool(last.get("text")),
            "→ %r%s" % (last.get("text"), ("  err=%s" % last.get("err")) if last.get("err") else ""))
        # ★ 字库只有 32 个字：文字再对，上屏也可能一排 `?` —— 那不是本链路的 bug
        # ★ 分两路判：这里以前写死"手动模式 ⇒ pending"，而默认已经改成 auto，
        #   那条断言会把**正确行为判成错**。判据取自服务端报的 mode0，不猜。
        if mode0 == "auto":
            for _ in range(40):                 # auto 的回灌是后台线程发的，等它到
                if got.get("body"):
                    break
                time.sleep(0.25)
            last = (c.get("/api/audio/status").get_json() or {}).get("last") or last
            try:
                bj = json.loads((got.get("body") or b"{}").decode("utf-8"))
            except Exception:
                bj = {}
            chk("auto 模式 ⇒ 识别完**自动**回灌，不用点确认",
                last.get("inject") == "auto" and bool(got.get("body")),
                "→ inject=%s path=%s" % (last.get("inject"), got.get("path")))
            chk("auto 回灌 body 形状与 语音识别.py::upload() 一致",
                bj.get("user") == last.get("text") and bj.get("mode") == "full"
                and bj.get("need_ai") is True and bj.get("enqueue_decision") is True
                and "device_id" in bj,
                "→ keys=%s" % sorted(bj.keys()))
            chk("token 带上了（不为空）", bool(bj.get("token")))
        else:
            chk("手动模式 ⇒ 挂起等确认，没自动发", last.get("inject") == "pending",
                "→ inject=%s" % last.get("inject"))
            chk("手动模式下确实一个字节都没自动回灌", not got.get("body"),
                "→ path=%s" % got.get("path"))

    # ---- 10) 手动"确认发送"（auto 模式下按设计不适用，只钉 409 语义）----
    # ★ 假服务器已在 6.5 装好（必须在 /end 之前：auto 的回灌是后台线程自己发的）。
    if SKIP_ASR:
        print("[SKIP] 跳过回灌校验（没识别就没得发）")
    elif mode0 == "auto":
        r = c.post("/api/audio/send")
        chk("auto 模式没有待确认 ⇒ send 仍按契约 409（不是 200 假成功）",
            r.status_code == 409, "→ %d" % r.status_code)
        chk("auto 模式 send 后没有留下待确认",
            (c.get("/api/audio/status").get_json() or {}).get("pending") is None)
    else:
        r = c.post("/api/audio/send")
        j = r.get_json() or {}
        chk("手动 send 成功", r.status_code == 200 and j.get("ok"),
            "→ %s" % json.dumps(j, ensure_ascii=False)[:140])
        try:
            bj = json.loads((got.get("body") or b"{}").decode("utf-8"))
        except Exception:
            bj = {}
        chk("回灌 body 形状与 语音识别.py::upload() 一致",
            bj.get("user") == last.get("text") and bj.get("mode") == "full"
            and bj.get("need_ai") is True and "device_id" in bj,
            "→ keys=%s" % sorted(bj.keys()))
        chk("token 带上了（不为空）", bool(bj.get("token")))
        chk("send 后 pending 清空",
            (c.get("/api/audio/status").get_json() or {}).get("pending") is None)
        r = c.post("/api/audio/send")
        chk("没有待确认时再 send ⇒ 409", r.status_code == 409,
            "→ %s" % json.dumps(r.get_json() or {}, ensure_ascii=False)[:90])

    # ---- 11) 收不齐绝不送识别 ----
    SID2 = "selftest02"
    r = c.post("/api/audio/chunk?sess=%s&seq=0&rate=%d&bits=%d&ch=%d&total=%d&token=%s"
               % (SID2, rate, bits, ch, 500000, token), data=pcm[:1000])
    j = c.post("/api/audio/end?sess=%s&token=%s" % (SID2, token)).get_json() or {}
    chk("说 500000 只发 1000 ⇒ complete=False 且不排队识别",
        j.get("complete") is False and "queued" not in j,
        "→ %s" % json.dumps(j, ensure_ascii=False)[:150])
    if j.get("saved"):
        CREATED.append(j["saved"])
        chk("残件按实收改短（能播的残件）",
            os.path.getsize(j["saved"]) == 44 + 1000, "→ %d 字节" % os.path.getsize(j["saved"]))

    # ---- 12) 模式开关（**跑完必须回到进来时的模式**，见 6.5 的 _restore_mode）----
    r = c.post("/api/audio/mode", json={"mode": "auto"})
    chk("切 auto", (r.get_json() or {}).get("mode") == "auto")
    r = c.post("/api/audio/mode", json={"mode": "manual"})
    chk("切 manual", (r.get_json() or {}).get("mode") == "manual")
    r = c.post("/api/audio/mode", json={"mode": "乱写"})
    chk("非法模式被拒（400）", r.status_code == 400)
    chk("非法模式没把当前模式改坏（还是 manual）",
        (c.get("/api/audio/status").get_json() or {}).get("mode") == "manual",
        "→ %s" % (c.get("/api/audio/status").get_json() or {}).get("mode"))

    _restore_mode()
    print("模式  : 已恢复为进来时的 %s（★ 别把用户的 auto 默认值留成 manual）" % mode0)
    chk("跑完模式与进来时一致",
        (c.get("/api/audio/status").get_json() or {}).get("mode") == mode0,
        "→ %s / 期望 %s" % ((c.get("/api/audio/status").get_json() or {}).get("mode"), mode0))

    print("-" * 66)
    ok = all(RESULT)
    if ok:
        for p in CREATED:
            try:
                os.remove(p)
            except Exception:
                pass
        print("全绿：%d 项全过 ✅   （自测生成的 wav 已删掉）" % len(RESULT))
    else:
        print("★ 有 %d/%d 项没过 —— 上面每条 [FAIL] 后面就是现场"
              % (sum(1 for x in RESULT if not x), len(RESULT)))
        for p in CREATED:
            if os.path.isfile(p):
                print("  留在（可以放来听听收成了什么）：%s" % p)
    print("=" * 66)
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n（被中断）")
        sys.exit(130)


