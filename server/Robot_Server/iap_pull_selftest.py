# -*- coding: utf-8 -*-
"""云端 OTA 下行「设备主动拉」自测（PC 侧，**不需要板子、不需要真设备**）。

它拿一个**假设备**（就是本进程里的一个 Flask test client）把 `iap_pull` 这条链路
从头走到尾：建任务 → 领任务 → 逐块拉 → 报进度 → 报 slot_crc32 → PASS/FAIL，
再逐条验这条链路**错了会很难查**的那些硬规矩：

  · 没 token 的 `/api/iap/chunk` **必须被拦**（否则同网段谁都能把固件拖走）
  · **未登录**建任务 / 看任务 **必须被拦**（★ 这条最关键：`/api/iap/` 与 `/api/iapl/`
    只差一个字母，一旦把 `/api/iapl/` 也放进 token 白名单，等于把"谁能给板子刷固件"
    交给任何一个拿到 token 的 URL ⇒ 本测试就是钉住这个字母的）
  · **网页的轮询领不走任务**（`from_web=True`）—— 浏览器抢了设备的活是静默的
  · 逐块拼回来 **必须逐字节等于源文件**，且 `crc32(拼回) == 任务 crc32`
    （★ 这就是"末块不补齐"换来的端到端不变量，见 `iap_pull.py` 模块头）
  · `seq == blocks` **必须回 409 + eof**（设备据此收手，不是靠猜）
  · **没认领过的设备拉块必须 409**（不是 200 + 空，空会让设备静默写 0 字节）
  · `slot_crc32` 报**对**才 PASS、报**错**必须 FAIL（PASS 判据一松，坏镜像会显示成功）
  · 取消任务后 `state` 必须变 failed（否则会一直占着"活动任务"让后面的建不进来）
  · **公告里的 `slot_crc32` / `block` 必须齐**（ESP 侧 `bridge_iap_pull.h` 直接依赖：
    前者是它 FINISH 阶段的**停机条件**，后者是块大小对账，缺了就得靠"传到一半失败"才发现）
  · **进度由服务端自己数**：设备**一次 report 都不发**，`got_blocks` 也得随拉取前进，
    且越界的 `seq` **不许**把进度顶上去（退回"每块报一次"就是 24 条多余的跨网 TCP）

★ 四条注意：
  1. **绝不打印 token**（config.json 里的那个）。
  2. **不占端口**：全程走 `app.test_client()`，所以**可以**在你自己那个 Flask
     正跑着 5000 的时候跑它。
  3. **有副作用的部分全部被挡掉了**（这是有意的，见 `_quiet()`）：本测试会把
     `asr.warmup`（要加载 500MB 模型）、`start_beacon`（**要抢 UDP 5001**，
     会打扰你正跑着的 Flask 的 ESP 自动发现）、`start_auto_tick`（会真调 AI 花钱）、
     `start_store`、以及 `append_line`（会写真的 `data_history.txt`）**全部换成空操作**，
     结束（含异常退出）**原样恢复**。本测试不碰 ASR、不碰真数据文件。
  4. 它会在 `flask_relay/firmware_f407/` 里生成 `selftest_iap_pull*.bin`
     （名字一看就是自测件）并**在结尾删掉**；跑挂了会留下，手动删即可。

退出码：0 = 全绿；1 = 有 FAIL（详见输出）。
"""
import io
import os
import struct
import sys
import zlib

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJ = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)
os.chdir(_HERE)                    # app.load_config() 按相对路径找 config.json

DEV = "esp-selftest-iap"           # 假设备的名字（一看就不是真设备）
FW_SMALL = "selftest_iap_pull_small.bin"
FW_EXACT = "selftest_iap_pull_exact.bin"
CREATED = []

RESULT = []


def chk(name, cond, extra=""):
    RESULT.append(bool(cond))
    print("%s %s %s" % ("[PASS]" if cond else "[FAIL]", name, extra))
    return bool(cond)


def fake_image(size):
    """造一个**能过 `f407_ota_relay.validate_image()`** 的假 BIN。

    只要两个头字段对：SP 合法 + PC 落在 Slot A 内并置 Thumb 位。正文随便填，
    但要**不是全同字节**，否则"拼回来的块顺序错了"这类 bug 验不出来。
    """
    import iap_pull
    buf = bytearray(size)
    struct.pack_into("<I", buf, 0, 0x20001000)                       # SP
    struct.pack_into("<I", buf, 4,
                     iap_pull.f407_ota_relay.SLOT_A_BASE + 0x101)    # PC（Thumb 位）
    for i in range(8, size):
        buf[i] = (i * 7 + (i >> 8)) & 0xFF
    return bytes(buf)


def _quiet(A):
    """把有副作用的后台初始化换成空操作，返回恢复用的原函数表。

    ★ 不这么做的话：`asr.warmup` 会加载模型、`start_beacon` 会**抢 UDP 5001**
      （你自己那个 Flask 正用着它给 ESP 报路）、`start_auto_tick` 会真调 AI 花钱、
      `append_line` 会把自测数据写进真的 `data_history.txt`。
      本测试与这些全无关系 —— 挡掉它们是为了**不给用户添乱**，不是为了掩盖失败。
    """
    saved = {}
    for name, repl in (
            ("start_auto_tick", lambda *a, **k: None),
            ("start_store", lambda *a, **k: None),
            ("start_beacon", lambda *a, **k: None),
            ("append_line", lambda *a, **k: None),
    ):
        if hasattr(A, name):
            saved[name] = getattr(A, name)
            setattr(A, name, repl)
    if hasattr(getattr(A, "asr", None), "warmup"):
        saved["asr.warmup"] = A.asr.warmup
        A.asr.warmup = lambda *a, **k: None
    return saved


def _restore(A, saved):
    for name, fn in saved.items():
        if name == "asr.warmup":
            A.asr.warmup = fn
        else:
            setattr(A, name, fn)


def main():
    try:
        import app as A
    except Exception as e:
        print("[FAIL] 起不来：import app 失败 %s: %s" % (type(e).__name__, e))
        return 1
    import iap_pull
    import ota_console

    saved = _quiet(A)
    try:
        return _run(A, iap_pull, ota_console)
    finally:
        _restore(A, saved)
        for p in CREATED:
            try:
                os.remove(p)
            except OSError:
                pass
        if CREATED:
            print("-" * 66)
            print("已清理自测固件 %d 个" % len(CREATED))


def _run(A, iap_pull, ota_console):
    token = str((A.load_config() or {}).get("token", "") or "")
    print("token : %s（**不打印内容**）" % ("有" if token else "★ 空"))
    print("块大小: %d B（iap_pull.BLOCK_BYTES）" % iap_pull.BLOCK_BYTES)
    print("-" * 66)

    # ---- 造两个假镜像并入库 ----
    # small: 20000 B ⇒ 5 块，**末块 3616 B（短块）** —— 验"末块不补齐"
    # exact: 98304 B = MAX_APP ⇒ 恰好 **24 块**，末块也是 4096 —— 验整槽边界
    imgs = {}
    for nm, size in ((FW_SMALL, 20000), (FW_EXACT, 98304)):
        data = fake_image(size)
        res = ota_console.save_firmware(nm, data)
        if not res.get("ok"):
            print("[FAIL] 假镜像 %s 没通过体检：%s" % (nm, res.get("reason")))
            return 1
        p = os.path.join(ota_console.FW407_DIR, nm)
        CREATED.append(p)
        imgs[nm] = data
        print("入库: %-32s %6d B" % (nm, size))

    dev = A.app.test_client()          # 假设备：**没登录**，只有 token
    web = A.app.test_client()          # 网页：登录
    r = web.post("/login", data={"token": token})
    if r.status_code not in (200, 302):
        print("[FAIL] 登录失败（HTTP %d）—— 先确认 config.json 的 token" % r.status_code)
        return 1

    print("-" * 66)

    # ---- 1) 鉴权：设备端点要 token，控制台端点要登录 ----
    r = dev.get("/api/iap/chunk?device_id=%s&seq=0" % DEV)
    chk("没 token 拉 /api/iap/chunk 被拦（401）", r.status_code == 401,
        "→ %d" % r.status_code)

    r = A.app.test_client().post("/api/iapl/job", json={"name": FW_SMALL})
    chk("未登录建任务被拦（401）", r.status_code == 401, "→ %d" % r.status_code)
    r = A.app.test_client().get("/api/iapl/jobs")
    chk("未登录看任务列表被拦（401）", r.status_code == 401, "→ %d" % r.status_code)

    # ---- 2) 建任务 ----
    r = web.post("/api/iapl/job", json={"name": FW_SMALL})
    j = r.get_json() or {}
    chk("登录后建任务成功", r.status_code == 200 and j.get("ok"), "→ %d" % r.status_code)
    if not j.get("ok"):
        print("   %s" % j.get("error"))
        return 1
    jid, blocks = j["job_id"], j["blocks"]
    n = len(imgs[FW_SMALL])
    want_blocks = (n + iap_pull.BLOCK_BYTES - 1) // iap_pull.BLOCK_BYTES
    chk("块数 = ceil(%d/%d) = %d" % (n, iap_pull.BLOCK_BYTES, want_blocks),
        blocks == want_blocks, "→ %d" % blocks)
    chk("transport = device_pull", j.get("transport") == "device_pull",
        "→ %s" % j.get("transport"))

    # ---- 3) 网页的轮询领不走（from_web=True） ----
    r = web.post("/api/data", json={"token": token, "device_id": DEV, "user": "",
                                    "mode": "idle", "deliver": True})
    chk("网页轮询领不走 iap 任务", "iap" not in (r.get_json() or {}),
        "→ %r" % ((r.get_json() or {}).get("iap"),))

    # ---- 4) 设备认领：公告搭在 POST /api/data 的响应里 ----
    r = dev.post("/api/data", json={"token": token, "device_id": DEV, "user": "",
                                    "sensor": "", "mode": "idle"})
    body = r.get_json() or {}
    iap = body.get("iap")
    chk("设备轮询领到任务（响应里有 iap）", bool(iap), "→ %r" % (iap,))
    if not iap:
        return 1
    chk("公告 crc32 是 int（要直接塞进 @IAP:REQUEST）", isinstance(iap.get("crc32"), int),
        "→ %r" % (iap.get("crc32"),))
    chk("公告 crc32 == 镜像 crc32",
        "%08X" % (iap["crc32"] & 0xffffffff) == j["crc32"], "")
    # ★ 下面两条是 ESP 侧 bridge_iap_pull.h **直接依赖**的字段：
    #   `slot_crc32` = 它的**停机条件**（FINISH 阶段每 2 s 探一次，等 mcuCrc 撞上它）；
    #   `block`      = 它和 IAP_PULL_BLOCK 当场对账用（不一致**拒收**，而不是等
    #                  传到一半才以"字节数对不上"失败 —— 那样报错根本指不到病根）。
    chk("公告 slot_crc32 是 int 且 == 镜像 padded CRC",
        isinstance(iap.get("slot_crc32"), int)
        and "%08X" % (iap["slot_crc32"] & 0xffffffff) == j["slot_crc32_padded"],
        "→ %r vs %s" % (iap.get("slot_crc32"), j.get("slot_crc32_padded")))
    chk("公告 block == BLOCK_BYTES（设备据此对账块大小）",
        iap.get("block") == iap_pull.BLOCK_BYTES,
        "→ %r vs %d" % (iap.get("block"), iap_pull.BLOCK_BYTES))

    # ---- 5) 别的设备拉不走 ----
    r = dev.get("/api/iap/chunk?token=%s&device_id=esp-other&seq=0" % token)
    chk("别的设备拉不动这个任务（409）", r.status_code == 409, "→ %d" % r.status_code)

    # ---- 6) 逐块拉 ----
    chunks, seq = [], 0
    while True:
        r = dev.get("/api/iap/chunk?token=%s&device_id=%s&seq=%d" % (token, DEV, seq))
        if r.status_code == 409:
            chk("拉到 seq=%d 时收手（409 + eof）" % seq,
                bool((r.get_json() or {}).get("eof")),
                "→ %r" % (r.get_json(),))
            break
        if r.status_code != 200:
            chk("seq=%d 拉块失败" % seq, False, "→ %d" % r.status_code)
            return 1
        b = r.data
        if seq < blocks - 1:
            chk("seq=%d 拿到整块 %d B" % (seq, iap_pull.BLOCK_BYTES),
                len(b) == iap_pull.BLOCK_BYTES, "→ %d" % len(b))
        else:
            chk("末块 %d B（**不补齐到 %d**）" % (len(b), iap_pull.BLOCK_BYTES),
                len(b) == n - (blocks - 1) * iap_pull.BLOCK_BYTES, "→ %d" % len(b))
        chunks.append(b)
        # ★ 服务端**自己数**进度：设备从头到尾**一次 report 都没发**，got_blocks
        #   却必须随"服务端发出去第几块"前进。这条一旦坏掉，ESP 就得退化成
        #   "每块发一条跨网 report"（werkzeug 每个响应 Connection: close、无保活）
        #   ⇒ 24 块 = 24 条多余的 TCP 连接。所以这里逐块钉住它。
        gb = (((web.get("/api/iapl/job/%s" % jid).get_json() or {}).get("job")
               or {}).get("got_blocks"))
        chk("拉完 seq=%d 后服务端 got_blocks 自动到 %d（设备一次 report 都没发）"
            % (seq, seq + 1), gb == seq + 1, "→ %r" % gb)
        seq += 1
        if seq > blocks + 2:
            chk("拉块没有正常收手", False, "已经拉了 %d 块" % seq)
            return 1

    joined = b"".join(chunks)
    chk("块数对得上（%d）" % blocks, len(chunks) == blocks, "→ %d" % len(chunks))
    chk("★ 逐字节等于源镜像（%d B）" % n, joined == imgs[FW_SMALL],
        "→ 首处不同 %r" % _first_diff(joined, imgs[FW_SMALL]))
    chk("★ crc32(拼回) == 任务 crc32",
        "%08X" % (zlib.crc32(joined) & 0xffffffff) == j["crc32"], "")

    # ---- 7) 越界 ----
    r = dev.get("/api/iap/chunk?token=%s&device_id=%s&seq=999" % (token, DEV))
    chk("seq 越界回 409 + eof", r.status_code == 409
        and bool((r.get_json() or {}).get("eof")), "→ %d" % r.status_code)
    # ★ 越界的 seq **不许**把进度推上去（服务端那条 `0 <= seq < blocks` 守卫的另一半）。
    #   少这半边，一个乱序/重发的设备就能把进度条直接顶到 100%。
    gb2 = (((web.get("/api/iapl/job/%s" % jid).get_json() or {}).get("job")
            or {}).get("got_blocks"))
    chk("越界 seq=999 **没有**把 got_blocks 推上去（仍是 %d）" % blocks, gb2 == blocks,
        "→ %r" % gb2)

    # ---- 8) 报进度 ----
    r = dev.post("/api/iap/report?token=%s" % token,
                 json={"device_id": DEV, "phase": "waiting_user"})
    chk("报 waiting_user 被接受", (r.get_json() or {}).get("phase") == "waiting_user",
        "→ %r" % (r.get_json(),))
    r = web.get("/api/iapl/job/%s" % jid).get_json() or {}
    chk("网页看得到 phase 变成 waiting_user",
        (r.get("job") or {}).get("phase") == "waiting_user",
        "→ %s" % (r.get("job") or {}).get("phase"))

    # ---- 9) slot_crc32 报错 ⇒ FAIL ----
    r = dev.post("/api/iap/report?token=%s" % token,
                 json={"device_id": DEV, "slot_crc32": 0xDEADBEEF})
    bad = (web.get("/api/iapl/job/%s" % jid).get_json() or {}).get("job") or {}
    chk("slot_crc32 不一致 ⇒ FAIL", bad.get("state") == "failed"
        and not (bad.get("result") or {}).get("ok"),
        "→ state=%s" % bad.get("state"))

    # ---- 10) 正确 ⇒ PASS（另起一个任务）----
    j2 = (web.post("/api/iapl/job", json={"name": FW_EXACT}).get_json() or {})
    chk("槽满镜像（%d B = MAX_APP）块数 = 24" % len(imgs[FW_EXACT]),
        j2.get("blocks") == 24, "→ %r" % j2.get("blocks"))
    dev.post("/api/data", json={"token": token, "device_id": DEV, "user": "",
                                "sensor": "", "mode": "idle"})
    r = dev.post("/api/iap/report?token=%s" % token,
                 json={"device_id": DEV, "phase": "verifying",
                       "got_blocks": j2.get("blocks")})
    chk("报 verifying 被接受", (r.get_json() or {}).get("phase") == "verifying",
        "→ %r" % (r.get_json(),))
    padded = j2.get("slot_crc32_padded")
    r = dev.post("/api/iap/report?token=%s" % token,
                 json={"device_id": DEV, "slot_crc32": padded})
    good = (web.get("/api/iapl/job/%s" % j2["job_id"]).get_json() or {}).get("job") or {}
    chk("slot_crc32 一致 ⇒ PASS", good.get("state") == "done"
        and (good.get("result") or {}).get("ok"),
        "→ state=%s msg=%s" % (good.get("state"),
                               (good.get("result") or {}).get("message")))
    chk("PASS 判据用的是 **padded** CRC（不是原文 CRC）",
        (good.get("result") or {}).get("slot_a_crc32") == padded, "→ %s" % padded)

    # ---- 11) 取消 ----
    j3 = (web.post("/api/iapl/job", json={"name": FW_SMALL}).get_json() or {})
    r = web.post("/api/iapl/job/%s/abort" % j3["job_id"])
    ab = (r.get_json() or {}).get("job") or {}
    chk("取消后 state=failed", ab.get("state") == "failed", "→ %s" % ab.get("state"))
    j4 = (web.post("/api/iapl/job", json={"name": FW_SMALL}).get_json() or {})
    chk("取消后能再建新任务（活动位已释放）", j4.get("ok"))
    web.post("/api/iapl/job/%s/abort" % j4.get("job_id"))    # 收尾，别留着活动任务

    print("-" * 66)
    ok = sum(1 for x in RESULT if x)
    print("结果: %d/%d 通过" % (ok, len(RESULT)))
    print("ALL PASS: iap_pull" if ok == len(RESULT) else "★ 有 FAIL，见上面 [FAIL] 行")
    return 0 if ok == len(RESULT) else 1


def _first_diff(a, b):
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return i
    return -1 if len(a) == len(b) else min(len(a), len(b))


if __name__ == "__main__":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8",
                                  errors="backslashreplace")
    sys.exit(main())
