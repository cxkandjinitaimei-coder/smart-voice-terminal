"""ota_console.py：固件库、ESP 固件版本发布、F407 升级任务与回执的网页接口。

使用：由 app 挂接蓝图；管理员上传固件并选择设备，镜像元数据先校验再交传输层。
时序与边界：文件名与路径需限定在固件目录；ESP 数字版本比较不接受原开发版本到公开 1.0.0 的直接降级。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
# -*- coding: utf-8 -*-
import ipaddress
import os
import re
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
from f407_ota_relay import MAX_APP, SLOT_A_BASE, RelayError  # noqa: E402

# ---- 固件库 ----
FW407_DIR = os.path.join(BASE_DIR, "firmware_f407")
#: 上传上限（含冗余）。F407 的应用 BIN 上限是 MAX_APP，留点余量好把
#: 「你传的其实是 ESP 固件」这类情况收下来给出人话理由，而不是无脑 413。
MAX_UPLOAD = 1024 * 1024
_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,95}\.bin$")

# ---- 任务 ----
JOB_KEEP = 12          # 最多保留多少个历史任务（超出淘汰最老的）
JOB_LOG_MAX = 400      # 单个任务的日志行上限

PHASE_TEXT = {
    "queued":       "已排队",
    "probing":      "正在探测设备 8080…",
    "waiting_user": "⚠️ 去按板子上的 PE3 或 K-UP",
    "uploading":    "正在上传并写 Flash —— 别断电、别重启 Flask",
    "verifying":    "设备重启自检中…",
    "done":         "PASS",
    "failed":       "FAIL",
}

#: 每个阶段**开始**时的日志文案（只在阶段切换时写一行，不刷屏）
PHASE_START = {
    "probing":      "开始探测设备的 8080 服务…",
    "waiting_user": "设备已进入确认状态：⚠️ 请按板子上的 PE3 或 K-UP（现在还没开始写 Flash）",
    "uploading":    "PE3 或 K-UP 已确认，开始上传并写入 Flash —— 别断电、别重启 Flask",
    "verifying":    "镜像已提交，设备正在重启自检…",
}
PHASE_LEVEL = {"waiting_user": "warn", "uploading": "warn"}


class OtaError(RuntimeError):
    """给用户看的、带中文原因的失败（路由直接回 400/404）。"""


class JobBusy(OtaError):
    """已有烧录任务在跑（路由回 409）。"""


# ======================================================================
# 固件库
# ======================================================================
def _safe_name(name):
    """白名单校验文件名，杜绝路径穿越。

     刻意**不**用 ``os.path.basename`` 静默取尾 —— 那会把 ``a/b.bin`` 静默
    存成 ``b.bin``：写不出去（安全上没事），但用户看到的文件名和他传的不一样。
    宁可当场报错。``/`` 与 反斜杠 都要判，因为部署到 Linux 后 反斜杠 不再是分隔符。
    """
    raw = str(name or "").strip()
    if not raw:
        raise OtaError("文件名是空的")
    if "/" in raw or "\\" in raw or os.path.basename(raw) != raw:
        raise OtaError("文件名里不能带路径：「%s」" % raw)
    if not _NAME_RE.match(raw):
        raise OtaError("只接受 .bin 文件名（字母/数字/._- ，且以 .bin 结尾）：收到「%s」" % raw)
    return raw


def _check(data):
    """本模块自己的镜像体检，返回 ``(ok, reason)``。

     与 ``f407_ota_relay.validate_image()`` 是**同一套判据**，但把「不像 F407
    Slot A 镜像」细分成能一眼看懂的原因（尤其是"你传的其实是别的东西"）。
    ``perform_lan_ota()`` 起线程时**还会再校验一遍**，所以这里即便将来跑偏，
    也不会导致烧错 —— 最多是理由文案不如这里清楚。
    """
    n = len(data)
    if n < 8:
        return False, "文件只有 %d 字节，不是有效的 BIN" % n
    if n > MAX_APP:
        return False, ("%d 字节，超过 Slot A 槽位容量 %d 字节（F407 应用 BIN 上限）"
                       % (n, MAX_APP))
    sp = int.from_bytes(data[0:4], "little")
    pc = int.from_bytes(data[4:8], "little")
    if not (pc & 1):
        return False, ("首向量 PC=0x%08X 没置 Thumb 位 ⇒ 不是 ARM 应用 BIN。"
                       "若这是 ESP8266 固件，请走 ESP 的升级通道 —— 本页只升 F407。" % pc)
    entry = pc & ~1
    if not (SLOT_A_BASE + 8 <= entry < SLOT_A_BASE + n):
        if 0x08000000 <= entry < SLOT_A_BASE:
            return False, ("入口 0x%08X 落在 Slot A（0x%08X）之前 ⇒ 这是链接到 "
                           "0x08000000 的**普通（非 OTA）** F407 工程，本页只能烧"
                           "链接到 Slot A 的镜像。" % (entry, SLOT_A_BASE))
        if 0x08000000 <= entry <= 0x08FFFFFF:
            return False, ("入口 0x%08X 超出本镜像范围（0x%08X~0x%08X）⇒ BIN 与长度不匹配。"
                           % (entry, SLOT_A_BASE, SLOT_A_BASE + n))
        return False, ("入口 0x%08X 不在 STM32 flash 内 ⇒ 更像 ESP8266 或其它芯片的固件，"
                       "本页只升 F407。" % entry)
    if (sp & 7) or not (0x20000000 < sp <= 0x20020000 or 0x10000000 < sp <= 0x10010000):
        return False, "初始栈指针 SP=0x%08X 不合法。" % sp
    return True, ""


def list_firmware():
    """列出固件库（每个文件都当场体检，页面看到的就是能烧的）。"""
    if not os.path.isdir(FW407_DIR):
        return []
    items = []
    for fn in sorted(os.listdir(FW407_DIR)):
        path = os.path.join(FW407_DIR, fn)
        if not os.path.isfile(path) or not _NAME_RE.match(fn):
            continue
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError as exc:
            items.append({"name": fn, "size": 0, "crc32": "", "valid": False,
                          "reason": "读不出来：%s" % exc, "uploaded": ""})
            continue
        ok, reason = _check(data)
        items.append({
            "name": fn,
            "size": len(data),
            "crc32": "%08X" % (zlib.crc32(data) & 0xffffffff),
            "valid": ok,
            "reason": reason,
            "uploaded": datetime.fromtimestamp(os.path.getmtime(path)).strftime("%Y-%m-%d %H:%M"),
        })
    items.sort(key=lambda x: x["uploaded"], reverse=True)
    return items


def firmware_path(name):
    """取库内某个固件的绝对路径；不在库里就报错（杜绝穿越与"烧到别的文件"）。"""
    safe = _safe_name(name)
    path = os.path.join(FW407_DIR, safe)
    if not os.path.isfile(path):
        raise OtaError("固件库里没有「%s」（先上传，或刷新列表）" % safe)
    return path


def save_firmware(filename, data):
    """存一个上传的 BIN 进库。

     **体检不过的文件不会落盘**（返回 ``valid=False`` + 原因给页面看），
    这样库里永远只有能烧的东西，``DELETE`` 的"只认库内清单"也才站得住。
    """
    name = _safe_name(filename)
    if not data:
        raise OtaError("上传的内容是空的")
    if len(data) > MAX_UPLOAD:
        raise OtaError("文件 %d 字节，超过上传上限 %d 字节" % (len(data), MAX_UPLOAD))
    crc = "%08X" % (zlib.crc32(data) & 0xffffffff)
    ok, reason = _check(data)
    if not ok:
        return {"ok": False, "name": name, "size": len(data), "crc32": crc,
                "valid": False, "reason": reason}
    os.makedirs(FW407_DIR, exist_ok=True)
    with open(os.path.join(FW407_DIR, name), "wb") as fh:
        fh.write(data)
    return {"ok": True, "name": name, "size": len(data), "crc32": crc,
            "valid": True, "reason": ""}


def delete_firmware(name):
    """删库内某个固件；不在库里 ⇒ 报错（不碰库外的任何路径）。"""
    path = firmware_path(name)
    os.remove(path)
    return os.path.basename(path)


# ======================================================================
# 目标解析
# ======================================================================
def resolve_target(target, device_ip_map):
    """把用户填的「IP 或名字」解析成可直连的 IPv4。

    返回 ``(esp_ip, known_device_id)``；``known_device_id`` 为空串表示用户直接
    填的 IP。解析不出来就抛 :class:`OtaError`，文案要能指导用户下一步。
    """
    raw = str(target or "").strip()
    if not raw:
        raise OtaError("请填目标设备的 IP 或名字")
    try:
        addr = ipaddress.ip_address(raw)
    except ValueError:
        addr = None
    if addr is not None:
        if addr.version != 4:
            raise OtaError("只支持局域网 IPv4 地址（收到「%s」）—— 域名不放行，"
                           "因为本页只在局域网直连设备。请填 IPv4，例如 192.168.1.9" % raw)
        return str(addr), ""
    # 「像地址但不是合法 IPv4」的形态（URL / 域名 / 写错的 IP）单独给话术。
    # 避免它掉进下面那句「服务端还不知道设备 X 的 IP」——那会让人误以为
    # "等它上报一次就好"，而实际上他填的根本不是个 IP。
    if "//" in raw or "." in raw or ":" in raw:
        raise OtaError("「%s」不是有效的 IPv4 地址。本页只在局域网直连设备，"
                       "不接受域名或 URL —— 请填 IPv4，例如 192.168.1.9" % raw)
    known = str((device_ip_map or {}).get(raw, "") or "")
    if known:
        return known, raw
    raise OtaError("服务端还不知道设备「%s」的 IP：它必须**在线并上报过至少一次**，"
                   "服务端才能从上报来源学到它的地址。请确认设备在线，"
                   "或直接填它的局域网 IP。" % raw)


# ======================================================================
# 任务注册表
# ======================================================================
_jobs = {}
_job_order = []
_active_id = ""
_jobs_lock = threading.Lock()


def _now_hms():
    return time.strftime("%H:%M:%S")


def _log(job, text, level="info"):
    job["log"].append({"t": _now_hms(), "level": level, "text": text})
    if len(job["log"]) > JOB_LOG_MAX:
        del job["log"][:-JOB_LOG_MAX]


def _snapshot(job):
    """给前端的任务快照（内部键 ``_`` 开头的不外传）。"""
    with _jobs_lock:
        out = {k: v for k, v in job.items() if not k.startswith("_")}
        out["log"] = [dict(x) for x in job["log"]]
        if job.get("state") == "running" and job.get("_started_mono"):
            out["elapsed_sec"] = int(time.monotonic() - job["_started_mono"])
        return out


def active_job_id():
    with _jobs_lock:
        return _active_id


def get_job(job_id):
    with _jobs_lock:
        job = _jobs.get(str(job_id or ""))
    if job is None:
        raise OtaError("任务不存在或已被清理")
    return _snapshot(job)


def _run_job(job, data, token, runner, session):
    global _active_id
    started = job["_started_mono"]

    def progress(phase, info):
        with _jobs_lock:
            if phase != job["phase"]:
                job["phase"] = phase
                job["phase_text"] = PHASE_TEXT.get(phase, phase)
                if phase in PHASE_START:
                    _log(job, PHASE_START[phase], PHASE_LEVEL.get(phase, "info"))
            if "remaining_sec" in info:
                job["remaining_sec"] = info["remaining_sec"]
            if "elapsed_sec" in info:
                job["verify_elapsed_sec"] = info["elapsed_sec"]
            job["elapsed_sec"] = int(time.monotonic() - started)

    try:
        result = runner(job["esp_ip"], data, token, session=session, progress=progress)
        with _jobs_lock:
            job["state"] = "done"
            job["phase"] = "done"
            job["phase_text"] = PHASE_TEXT["done"]
            job["remaining_sec"] = None
            job["result"] = {
                "ok": True,
                "message": result.get("message", ""),
                "bytes": result.get("bytes"),
                "crc32": result.get("crc32"),
                "slot_a_crc32": result.get("slot_a_crc32"),
            }
            _log(job, "PASS：%s" % (result.get("message") or "烧录成功"), "good")
    except RelayError as exc:
        # RelayError 的中文原因就是给用户看的（含"未上传、未擦除固件"这类保证）
        with _jobs_lock:
            job["state"] = "failed"
            job["phase"] = "failed"
            job["phase_text"] = PHASE_TEXT["failed"]
            job["remaining_sec"] = None
            job["result"] = {"ok": False, "message": str(exc)}
            _log(job, "FAIL：%s" % exc, "bad")
    except Exception as exc:                                   # noqa: BLE001
        with _jobs_lock:
            job["state"] = "failed"
            job["phase"] = "failed"
            job["phase_text"] = PHASE_TEXT["failed"]
            job["remaining_sec"] = None
            job["result"] = {"ok": False, "message": "服务端内部错误：%r" % exc}
            _log(job, "FAIL：服务端内部错误 %r" % exc, "bad")
    finally:
        with _jobs_lock:
            job["finished"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            job["elapsed_sec"] = int(time.monotonic() - started)
            if _active_id == job["id"]:
                _active_id = ""


def start_flash_job(target, name, token, device_ip_map,
                    runner=None, session=None, transport="lan_direct"):
    """起一个后台烧录任务，立即返回任务快照。

    ``runner``/``session`` 只给测试注入用：生产路径是
    ``f407_ota_relay.perform_lan_ota`` + 它自己建的 ``requests.Session``。

     线程里**没有 request/session 上下文**：``token``、固件字节、解析好的
    ``esp_ip`` 全部在起线程**之前**取好，线程里绝不碰 ``request``。
    """
    global _active_id
    runner = runner or f407_ota_relay.perform_lan_ota
    with _jobs_lock:
        cur = _jobs.get(_active_id) or {}
        if _active_id and cur.get("state") == "running":
            raise JobBusy("已有一个烧录任务在跑（%s，%s）；请等它结束或刷新页面看结果"
                          % (_active_id, cur.get("phase_text", "")))

    esp_ip, known = resolve_target(target, device_ip_map)
    path = firmware_path(name)
    with open(path, "rb") as fh:
        data = fh.read()
    ok, reason = _check(data)
    if not ok:
        raise OtaError("库里「%s」现在体检不过：%s" % (os.path.basename(path), reason))
    raw_crc = "%08X" % (zlib.crc32(data) & 0xffffffff)

    job = {
        "id": uuid.uuid4().hex[:12],
        "state": "running",
        "phase": "queued",
        "phase_text": PHASE_TEXT["queued"],
        "remaining_sec": None,
        "elapsed_sec": 0,
        "verify_elapsed_sec": None,
        "started": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "finished": "",
        "target": str(target or "").strip(),
        "known_device": known,
        "esp_ip": esp_ip,
        "transport": transport,
        "name": os.path.basename(path),
        "bytes": len(data),
        "crc32": raw_crc,
        "log": [],
        "result": None,
        "_started_mono": time.monotonic(),
    }
    with _jobs_lock:
        cur = _jobs.get(_active_id) or {}
        if _active_id and cur.get("state") == "running":
            raise JobBusy("已有一个烧录任务在跑（%s）；请等它结束再发起" % _active_id)
        _jobs[job["id"]] = job
        _job_order.append(job["id"])
        while len(_job_order) > JOB_KEEP:
            oldest = _job_order.pop(0)
            if oldest != job["id"]:
                _jobs.pop(oldest, None)
        _active_id = job["id"]
        _log(job, "排队：%s → %s（%s，%d 字节，CRC32 %s）"
                  % (job["name"], esp_ip, transport, len(data), raw_crc))
    #  _snapshot() 自己会拿 _jobs_lock（普通 Lock 不可重入）⇒ 必须放在锁**外**
    snap = _snapshot(job)

    threading.Thread(target=_run_job, args=(job, data, token, runner, session),
                     name="ota-job-%s" % job["id"], daemon=True).start()
    return snap
