"""iap_pull.py：设备主动拉取 F407 OTA 的任务状态机。

使用：网页创建任务，目标 ESP 轮询领取、申请按键确认、分块拉取镜像并上报进度。
时序与边界：任务锁保护领取者和阶段；重启后的板端 CRC/版本确认才表示完成，传完字节不能直接判成功。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
# -*- coding: utf-8 -*-
import os
import sys
import threading
import time
import uuid
import zlib
from datetime import datetime

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
if BASE_DIR not in sys.path:
    sys.path.insert(0, BASE_DIR)

import f407_ota_relay                       # noqa: E402  （先修 sys.path 再导入）
import ota_console                          # noqa: E402  （只读它的固件库，不改它）

MAX_APP = f407_ota_relay.MAX_APP

#: 一次 HTTP 拉多少字节。见模块头「块大小 4096 的由来」。 别改小。
BLOCK_BYTES = 4096

#: 设备认领后多久没进展就**放回去**给别人（也给它自己）重领。
#: 拔网 / 断电 / 设备重启都会走到这 —— 不设这个，任务就永久卡在 claimed。
CLAIM_STALE_SEC = 300

JOB_KEEP = 8           # 最多留几个历史任务（超出淘汰最老的）
JOB_LOG_MAX = 200      # 单任务日志行上限
DEVICE_FRESH_SEC = 30  # 设备多久没来问就算"没在等"，用于网页侧显示

PHASE_TEXT = {
    "waiting_device": "等待设备来领（设备每秒问一次）",
    "waiting_user":   "⚠️ 去按板子上的 K-UP（现在还没开始写 Flash）",
    "uploading":      "设备正在拉取并写 Flash —— 别断电、别重启 Flask",
    "verifying":      "设备重启自检中…",
    "done":           "PASS",
    "failed":         "FAIL",
}
PHASE_START = {
    "waiting_user": ("设备已进入确认状态：⚠️ 请按板子上的 K-UP"
                     "（现在还没开始写 Flash）"),
    "uploading":    "K-UP 已确认，设备开始逐块拉取并写 Flash —— 别断电、别重启 Flask",
    "verifying":    "镜像已提交，设备正在重启自检…",
}
PHASE_LEVEL = {"waiting_user": "warn", "uploading": "warn"}


class IapError(RuntimeError):
    """给用户看的、带中文原因的失败（路由直接回 400/404）。"""


class IapBusy(IapError):
    """已有下发任务在跑（路由回 409）。"""


# ======================================================================
# 任务注册表
# ======================================================================
_jobs = {}
_job_order = []
_active_id = ""
_jobs_lock = threading.Lock()

#: 设备最近一次来问的时刻（device_id → monotonic），只给网页侧看"设备在不在"。
_device_seen = {}


def _now_hms():
    return time.strftime("%H:%M:%S")


def _log(job, text, level="info"):
    job["log"].append({"t": _now_hms(), "level": level, "text": text})
    if len(job["log"]) > JOB_LOG_MAX:
        del job["log"][:-JOB_LOG_MAX]


def _snapshot(job):
    """给前端/设备的任务快照（内部键 ``_`` 开头的不外传）。"""
    out = {k: v for k, v in job.items() if not k.startswith("_")}
    out["log"] = [dict(x) for x in job["log"]]
    if job.get("state") == "running" and job.get("_started_mono"):
        out["elapsed_sec"] = int(time.monotonic() - job["_started_mono"])
    return out


def _set_phase(job, phase):
    """切阶段（只在真的换了时写一行日志，不刷屏）。**调用方持锁。**"""
    if phase == job["phase"]:
        return
    job["phase"] = phase
    job["phase_text"] = PHASE_TEXT.get(phase, phase)
    if phase in PHASE_START:
        _log(job, PHASE_START[phase], PHASE_LEVEL.get(phase, "info"))


def _active_job():
    """当前活动任务（**调用方持锁**）。"""
    return _jobs.get(_active_id) or {}


# ======================================================================
# 建任务（给网页 / CLI 调；**必须登录**见 app.py 的 /api/iapl/ 前缀）
# ======================================================================
def create_job(name, target_device=""):
    """从**现成的**固件库建一个"设备主动拉"的下载任务，返回快照。

    ``name`` 必须是 ``firmware_f407/`` 里已有的文件名 —— 这里只做读取，
    固件的"入库"归 ``ota_console.save_firmware()``（上传入口在网页那一页）。
    """
    global _active_id
    with _jobs_lock:
        cur = _active_job()
        if _active_id and cur.get("state") == "running":
            raise IapBusy("已有一个下发任务在跑（%s，%s）；请等它结束或刷新看结果"
                          % (_active_id, cur.get("phase_text", "")))

    # 固件库的读权限交给 ota_console（它已经把"文件名白名单/路径穿越/体检"都做完了）
    try:
        path = ota_console.firmware_path(name)
    except ota_console.OtaError as exc:
        raise IapError(str(exc)) from exc
    with open(path, "rb") as fh:
        data = fh.read()
    try:
        raw_crc, padded_crc = f407_ota_relay.validate_image(data)
    except f407_ota_relay.RelayError as exc:
        # 与 ota_console._check() 同一套判据；那句中文原因已经能指导用户
        raise IapError("库里「%s」现在体检不过：%s" % (os.path.basename(path), exc)) from exc

    blocks = (len(data) + BLOCK_BYTES - 1) // BLOCK_BYTES
    target_device = str(target_device or "").strip()
    job = {
        "id": uuid.uuid4().hex[:12],
        "state": "running",
        "phase": "waiting_device",
        "phase_text": PHASE_TEXT["waiting_device"],
        "transport": "device_pull",
        "name": os.path.basename(path),
        "bytes": len(data),
        "crc32": "%08X" % raw_crc,                    # 原文（给 @IAP:REQUEST）
        "slot_crc32_padded": "%08X" % padded_crc,     #  PASS 判据
        "blocks": blocks,
        "got_blocks": 0,
        "claimed_by": "",
        # 空值兼容早期 CLI/脚本：任何设备可领取。网页公网模式会强制选设备，
        # 防止多块 ESP 同时在线时把任务发给错误的测试板。
        "target_device": target_device,
        "claimed_at": "",
        "device_seen_at": "",
        "started": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "finished": "",
        "elapsed_sec": 0,
        "log": [],
        "result": None,
        "_data": data,
        "_started_mono": time.monotonic(),
        "_claimed_mono": 0.0,
    }
    with _jobs_lock:
        if _active_id and _active_job().get("state") == "running":
            raise IapBusy("已有一个下发任务在跑（%s）；请等它结束再发起" % _active_id)
        _jobs[job["id"]] = job
        _job_order.append(job["id"])
        while len(_job_order) > JOB_KEEP:
            oldest = _job_order.pop(0)
            if oldest != job["id"]:
                _jobs.pop(oldest, None)
        _active_id = job["id"]
        target = ("指定设备「%s」" % target_device) if target_device else "任意在线设备"
        _log(job, "排队：%s（%d 字节，%d 块 × %d B，CRC32 %s）⇒ %s 领取"
                  % (job["name"], len(data), blocks, BLOCK_BYTES, job["crc32"], target))
    return _snapshot(job)


# ======================================================================
# 设备侧：领任务
# ======================================================================
def announce(device_id, from_web):
    """设备每秒 ``POST /api/data`` 时调一次：有它该干的活就**认领**并回公告。

    返回给设备的**小**字典（只带它干活要用的），没活就回 ``None``。

     认领规则**照抄** ``app.py:1932-1936`` 那条既有约定：**只有"设备自己的轮询"
      才有资格领**（``from_web=False``）。用户 PC 上打开的网页也打同一个端点，
      让它把任务领走就等于"浏览器抢了设备该干的活"。

     **幂等**：已经由这台设备认领、且还在跑的任务会**再回一次**。
      设备重启/重连后接着拉，不会因为"已经认领过"就再也拿不到。
    """
    if from_web or not device_id:
        return None
    now = time.monotonic()
    with _jobs_lock:
        _device_seen[device_id] = now
        job = _jobs.get(_active_id)
        if not job or job.get("state") != "running":
            return None
        # 网页指定的设备以 device_id 为准，不靠公网 IP，也不允许另一块在线 ESP 抢任务。
        if job.get("target_device") and job.get("target_device") != device_id:
            return None
        # 已被这台认领 ⇒ 幂等回给它
        mine = job.get("claimed_by") == device_id
        stale = (now - float(job.get("_claimed_mono") or 0.0)) > CLAIM_STALE_SEC
        if job.get("claimed_by") and not mine and not stale:
            return None                       # 别人在干（或刚交给别人），别抢
        if not mine:
            job["claimed_by"] = device_id
            job["claimed_at"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            _log(job, "设备「%s」领走了这个任务" % device_id)
        job["_claimed_mono"] = now
        job["device_seen_at"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        return _device_view(job)


def _device_view(job):
    """设备干活要用到的字段（**调用方持锁**）。

    ``crc32`` 与 ``slot_crc32`` 是**两个不同的 CRC**，别搞混（见模块 docstring）：
    前者是原文、设备塞进 ``@IAP:REQUEST``；后者是补到 224 KB 的、**PASS 判据**。
    后者发给设备是让 ESP **自己**有个明确的停机条件（等 ``mcuCrc`` 撞上它），
    同时也让它能把观测值回报上来 —— 服务端 ``report()`` 那边还会再判一次。
    """
    return {
        "id": job["id"],
        "name": job["name"],
        "bytes": job["bytes"],
        "crc32": int(job["crc32"], 16),      #  数值：设备要直接塞进 @IAP:REQUEST
        "capacity": f407_ota_relay.MAX_APP, "layout_id": f407_ota_relay.LAYOUT_ID,
        "slot_crc32": int(job["slot_crc32_padded"], 16),   #  期望的 slot CRC（PASS 判据）
        "block": BLOCK_BYTES,                #  设备核对用：不一致就当场拒收，别等"字节数对不上"
        "blocks": job["blocks"],
        "phase": job["phase"],
    }


def _claimed_job(device_id):
    """取这台设备当前认领的任务（**调用方持锁**）。"""
    job = _jobs.get(_active_id)
    if not job or job.get("state") != "running":
        return None
    if job.get("claimed_by") != device_id:
        return None
    return job


def read_chunk(device_id, seq):
    """设备拉第 ``seq`` 块（**0 基**）。返回 ``(data, eof, err)``。
    
        - 正常：``(bytes, False, "")``
        - 拉完了：``(b"", True, "")`` —— 设备该去走 END / COMMIT 了
        - 没认领 / 序号越界：``(b"", True, "原因")``
    
         ``seq`` 是**块号不是字节偏移**（``docs/protocols.md`` 登记的是 ``seq``，
          这里把它定义成块号，并在 ``app.py`` 的端点上写清楚）。
        """
    if not device_id:
        return b"", True, "缺少 device_id"
    with _jobs_lock:
        job = _claimed_job(device_id)
        if job is None:
            return b"", True, "这台设备当前没有认领到的任务"
        job["_claimed_mono"] = time.monotonic()
        job["device_seen_at"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        #  进度由**服务端自己数**：设备拉到第 seq 块 ⇒ 它至少走完了 seq+1 块。
        #   这样设备端**不必每块再发一次 report** —— 24 块就省下 24 条跨网 TCP 连接
        #   （werkzeug 每个响应都 Connection: close，无保活）。顺带把 `_claimed_mono`
        #   也刷新了，所以"认领 300 秒不过期"在传输期间自动成立。
        #    只在这一块确实在被拉时前进；越界的 seq 不许把进度推上去。
        if 0 <= seq < job["blocks"] and seq + 1 > job["got_blocks"]:
            job["got_blocks"] = seq + 1
        total = job["bytes"]
        data = job["_data"]
        blocks = job["blocks"]
    if seq < 0:
        return b"", True, "块号不能是负数"
    if seq >= blocks:
        return b"", True, ""
    start = seq * BLOCK_BYTES
    end = min(start + BLOCK_BYTES, total)     #  末块不补齐，见模块头
    return data[start:end], False, ""


def report(device_id, payload):
    """设备报进度 / 报结果。

    ``payload`` 认这些键（其余忽略）：
    ``phase``（设备侧的阶段，映射到本模块的阶段名）、``got_blocks``、
    ``err``（非空 ⇒ 任务失败）、``slot_crc32``（``@IAP:INFO`` 回来的 ``slot_crc``，
    **十六进制字符串或整数都收**）—— 它等于任务的 ``slot_crc32_padded`` 才是 PASS。
    """
    with _jobs_lock:
        job = _claimed_job(device_id)
        if job is None:
            return {"ok": False, "err": "这台设备当前没有认领到的任务"}
        job["_claimed_mono"] = time.monotonic()
        job["device_seen_at"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

        got = payload.get("got_blocks")
        if isinstance(got, int) and got >= 0:
            job["got_blocks"] = min(got, job["blocks"])

        phase = str(payload.get("phase") or "").strip()
        if phase in PHASE_TEXT:
            _set_phase(job, phase)

        err = str(payload.get("err") or "").strip()
        if err:
            job["state"] = "failed"
            _set_phase(job, "failed")
            job["result"] = {"ok": False, "message": err}
            _log(job, "FAIL：设备报告 %s" % err, "bad")
            _finish(job)
            return {"ok": True, "phase": job["phase"]}

        slot = payload.get("slot_crc32")
        if slot is not None:
            if isinstance(slot, int):
                got_crc = "%08X" % (slot & 0xffffffff)
            else:
                s = str(slot).strip().lower().replace("0x", "")
                got_crc = "%08X" % (int(s, 16) & 0xffffffff) if s else ""
            want = job["slot_crc32_padded"]
            job["result"] = {
                "ok": got_crc == want,
                "message": ("Slot A 校验通过（slot_crc32=%s 与镜像一致）" % got_crc
                            if got_crc == want else
                            "Slot A 校验不一致：设备报 %s，镜像应为 %s"
                            % (got_crc or "(空)", want)),
                "bytes": job["bytes"],
                "crc32": job["crc32"],
                "slot_a_crc32": got_crc,
            }
            job["state"] = "done" if got_crc == want else "failed"
            _set_phase(job, "done" if got_crc == want else "failed")
            _log(job, ("PASS：%s" if got_crc == want else "FAIL：%s")
                      % job["result"]["message"], "good" if got_crc == want else "bad")
            _finish(job)
        return {"ok": True, "phase": job["phase"]}


def _finish(job):
    """收尾（**调用方持锁**）。"""
    global _active_id
    job["finished"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    if job.get("_started_mono"):
        job["elapsed_sec"] = int(time.monotonic() - job["_started_mono"])
    if _active_id == job["id"]:
        _active_id = ""
    job.pop("_data", None)          # 96 KB 不必留到下次


# ======================================================================
# 网页 / CLI 侧
# ======================================================================
def list_jobs():
    with _jobs_lock:
        out = [_snapshot(j) for j in _jobs.values()]
    return out


def get_job(job_id):
    with _jobs_lock:
        job = _jobs.get(str(job_id or ""))
        if job is None:
            raise IapError("任务不存在或已被清理")
        return _snapshot(job)


def abort_job(job_id):
    """把任务标记失败（用户改主意了 / 设备一直不来）。**不碰设备**。"""
    with _jobs_lock:
        job = _jobs.get(str(job_id or ""))
        if job is None:
            raise IapError("任务不存在或已被清理")
        if job.get("state") != "running":
            return _snapshot(job)
        job["state"] = "failed"
        job["result"] = {"ok": False, "message": "已取消（未开始写 Flash 的任务直接作废）"}
        _set_phase(job, "failed")
        _log(job, "已取消这个下发任务", "bad")
        _finish(job)
        return _snapshot(job)


def device_fresh(device_id):
    """这台设备最近有没有在问（给网页显示"设备在不在线"）。"""
    with _jobs_lock:
        t = _device_seen.get(str(device_id or ""))
    return bool(t) and (time.monotonic() - t) <= DEVICE_FRESH_SEC
