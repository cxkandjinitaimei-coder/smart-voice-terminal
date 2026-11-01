# -*- coding: utf-8 -*-
"""
app.py — 智能家居 AI 中转站（Flask 服务器）
==========================================
运行：
    1) 编辑 config.json，填入你的 AI Key（DeepSeek 默认启用）
    2) pip install -r requirements.txt
    3) python app.py
    4) ESP8266 或 curl/Postman 发 POST 即可

接口：
    POST /api/data      body: {"sensor":"温度:28.5,湿度:65","user":"我热了"}
    GET  /api/health    查看各家 provider 状态
    GET  /              网页状态页
"""
import ipaddress
import re
import json
import os
import shutil
import socket
import threading
import time
from collections import deque
from datetime import datetime, timedelta

from flask import (Flask, jsonify, render_template, request,
                   redirect, session, url_for, send_file)

import ai_relay
import analytics
import device_profile
import device_controls
import motion_timeline
import motion_delivery
import sampled_motion
import idle_pet
import presence
import board_pose
import lan_discovery
import store
import camera_workspace
import camera_follow
import ui_skins
from release_version import APP_VERSION

# === [01语音] BEGIN 语音链路（P2）—— 协议见 合作表 §4.6 ===
# 两个都是 01语音 独占区的新文件。asr 里的 faster_whisper 是**懒加载**的，
# import 本身很轻；真缺库时 app.py 照常起，只是识别那一步会报错（状态接口能看见）。
import asr
import audio_rx
import f407_ota_relay
import iap_pull          # 2026-09-24 云端 OTA 下行「设备主动拉」—— 只读 ota_console 的固件库
import ota_console
from runtime_config import load_session_key
# === [01语音] END ===

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(BASE_DIR, "config.json")
DATA_HISTORY = os.path.join(BASE_DIR, "data_history.txt")
DECISION_LOG = os.path.join(BASE_DIR, "decision_log.txt")
FW_DIR = os.path.join(BASE_DIR, "firmware")   # OTA 固件存放目录
DEFAULT_DEVICE_ID = "esp-bridge"

# ---- 调试模式（开发伙伴对话）—— 与设备的记忆/统计**彻底分开** ----
# ★ 为什么必须是另一个文件：decision_log.txt 是 decision_log 那条"成品机器人"
#   的记忆源，调试对话一旦混进去，AI 就又被成品机器人的腔调带跑了。
DEBUG_LOG = os.path.join(BASE_DIR, "debug_chat_log.txt")
DEBUG_ARCHIVE_DIR = os.path.join(BASE_DIR, "_debug_archive")   # 「新对话」的归档处
DEBUG_HISTORY_LINES = 200      # 调试会话一次最多回放多少轮
MEMORY_RESET_EVENT = "memory_reset"   # decision_log.txt 里的"记忆起点"标记
DEVICE_ONLINE_SEC = 90   # 最近 N 秒有上报视为在线
DEFAULT_TICK_PROMPT = ("[定时唤醒] 这不是用户说的话，是你自己主动开口的时刻："
                       "结合最新传感器/历史数据，主动说一句简短的关心或搭话，"
                       "并给 1~2 个动作（gesture / expression / servo）。")

# === [03网页端] BEGIN === 网页对话/互动上屏（2026-09-23，见 合作表 §4.1 / §4.5 / §5）
# `@TEXT:` 载荷的两道硬闸 —— F407 那边是**静默丢弃**，不报错、屏上什么都不显示：
#   ① esp_uart.c:333 在 rx_used >= 191 就整行丢（连 `@` 都还没解析）
#   ② ailink.c:605   再兜一道 strlen(line) >= 192
#   ⇒ "@TEXT:"(6 字节) + 正文 必须 < 191。
SCREEN_TEXT_MAX_BYTES = 168     # 6 + 168 = 174 < 191，留 17 字节余量
# 屏上 caption 区 media.h: CAPTION_X 4 / CAPTION_W 232 / CAPTION_H 130，行高 20（media.c:364）
# ⇒ 6 行；可打印 ASCII(32~126) 走 8px = 1 格，其余走 16px = 2 格（media.c:366-373）
# ⇒ 最宽 6 行 × 29 ASCII = 174 格，或 6 × 14 汉字 = 84 汉字 = 168 格。留点余量取 160。
SCREEN_TEXT_MAX_CELLS = 160
# 截断时优先切在这些标点后面（读起来是完整的一句话，而不是半句）
SCREEN_BREAK_CHARS = "。！？!?…；;，,、"
# 队列里已经有 N 条没被取走就别再灌：设备离线时语音/聊天会一直堆，等它重连一次性刷屏
DELIVER_MAX_QUEUE = 3
COMMAND_MAX_QUEUE = 8
# === [03网页端] END ===

app = Flask(__name__)
# 登录 session 使用每台服务本地生成的随机密钥；不把可伪造登录态的固定口令放进公开源码。
app.secret_key = load_session_key(BASE_DIR)
_io_lock = threading.Lock()
_cmd_lock = threading.Lock()
_pending_commands = {}   # device_id -> deque[dict]
_device_last_seen = {}   # device_id -> datetime
_device_last_ack = {}    # device_id -> last ack text
_audio_transport_status = {}  # ESP reports upload/capture failures without a debug cable.
_motion_status = {}  # Actual F407 task acknowledgements relayed by ESP.
_gateway_step = {}   # device_id -> {seen: monotonic, capable: 是否上报 step_playback}
# 分步动作必须由 ESP 网关本地计时；旧网关不报 step_playback 时，网页要明确提示
# 升级，而不是让动作永远停在“等待新版ESP领取”。
_device_last_user = {}   # device_id -> datetime（用户最后一次"说话"的时刻，定时唤醒的静默期用）
# === [03网页端] BEGIN === 2026-09-23 远程OTA页：设备来源 IP
# 服务端此前**完全不知道任何设备的 IP**（全库 grep remote_addr / getpeername /
# X-Forwarded 零命中），而局域网直刷必须知道往哪个 IP 发。ESP 每秒 POST
# /api/data 一次 ⇒ 在 api_data() 里顺手记下 request.remote_addr 就够了，
# **不改 ESP 一个字节**。★ 只记非浏览器请求（浏览器来自用户自己的 PC，会污染映射）。
_device_ip = {}          # device_id -> "192.168.x.x"（最后一次非浏览器上报的来源地址）
# === [03网页端] END ===

# ---- 异常判定（阈值）的状态：进程内有效，重启后重来 ----
_threshold_last_fired = {}   # code -> epoch，冷却用（见 analytics.evaluate_thresholds）
_prev_reading = {}           # device_id -> 上一条解析结果，on_change 规则用（人员翻转判定）

# ---- 定时唤醒（类"定时器中断"）的线程与状态 ----
_tick_lock = threading.Lock()
_auto_tick_started = False
_store_started = False
_tick_stat = {           # 只给 /api/auto_tick 看，纯观测用
    "count": 0,          # 本次进程内成功触发次数（含被条件跳过的）
    "last_tick": "",     # 上次触发时间字符串
    "last_reason": "",   # 上次触发结果：ok / skip:xxx / error:xxx
    "last_message": "",  # 上次 AI 主动说的话
    "last_error": "",    # 上次异常（没出过错就是空）
    "next_at": None,     # 下次触发的 epoch 秒（未武装时 None）
}


# ===================== 文件读写 =====================

def load_config():
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        return ai_relay.companion_config(json.load(f))


def save_config(cfg):
    """把改过的配置写回 config.json（网页上新增/切换模型用）。"""
    with _io_lock:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)


def _queue_len(device_id):
    with _cmd_lock:
        q = _pending_commands.get(device_id)
        return len(q) if q else 0


def enqueue_reply(device_id, cmd):
    """语音/聊天回复共用上限，检查与入队必须在同一把锁内。

    返回None代表拒绝本次投递；录音和识别文字保留，可在历史中重发。
    不挤掉队列中的动作，也不把积压回复静默改成最新一条。
    """
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    with _cmd_lock:
        q = _pending_commands.setdefault(device_id, deque())
        if len(q) >= DELIVER_MAX_QUEUE:
            return None
        q.append(cmd)
        presence.TRACKER.note(device_id)
        return len(q)


def _presence_drop_commands(device_id):
    with _cmd_lock:
        q = _pending_commands.get(device_id)
        if not q:
            return 0
        keep = deque(c for c in q if not c.get('_presence'))
        removed = len(q) - len(keep)
        if keep:
            _pending_commands[device_id] = keep
        else:
            _pending_commands.pop(device_id, None)
        return removed


def _presence_yield(device_id, stop=False):
    """用户操作优先：清掉待机待发命令；待机动作在跑时先发 STOP。"""
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    stage = presence.TRACKER.status(device_id).get('stage', 0)
    presence.TRACKER.note(device_id)
    _presence_drop_commands(device_id)
    if not (stop or stage > 0):
        return
    cmd = {'raw': '@TIMELINE:STOP#', 'note': 'presence_yield', '_presence': True,
           'device_id': device_id}
    with _cmd_lock:
        q = _pending_commands.setdefault(device_id, deque())
        if any(c.get('raw') == '@TIMELINE:STOP#' for c in q):
            return
        q.appendleft(cmd)
    motion_delivery.register(device_id, cmd)


def enqueue_command(device_id, cmd):
    """网页遥控命令入队（内存队列，进程重启会丢）。"""
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    if not cmd.get('_presence'):
        with _cmd_lock:
            q = _pending_commands.get(device_id)
            user_count = sum(1 for c in (q or ()) if not c.get('_presence'))
        if user_count >= COMMAND_MAX_QUEUE:
            raise ValueError('设备指令队列已满，请等待设备取走')
        _presence_yield(device_id)
    with _cmd_lock:
        q = _pending_commands.setdefault(device_id, deque())
        if cmd.get('_presence'):
            if len(q) >= COMMAND_MAX_QUEUE + 4:
                return None
        elif sum(1 for c in q if not c.get('_presence')) >= COMMAND_MAX_QUEUE:
            raise ValueError('设备指令队列已满，请等待设备取走')
        q.append(cmd)
        qlen = len(q)
    motion_delivery.register(device_id, cmd)
    follow.stop(device_id, '手动或AI指令优先，跟随等待新照片', hold=5)
    if not cmd.get('_presence'):
        presence.TRACKER.note(device_id)
    return qlen


def pop_command(device_id, timeline_busy=False):
    """ESP 取走一条待执行命令；队列空返回 None。

    用户命令优先于待机 timeline：timeline_busy 时若队头是 @TIMELINE，
    用户命令优先于待机 timeline：STOP 可以越过未结束的 timeline；
    动作库的 @TIMELINE 不再被后面的命令顶掉，保证舵机动作能执行。
    """
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    with _cmd_lock:
        q = _pending_commands.get(device_id)
        if not q:
            return None
        stop = next((c for c in q if c.get('raw') == '@TIMELINE:STOP#'), None) if timeline_busy else None
        if stop is not None:
            q.remove(stop)
            cmd = stop
        elif timeline_busy and q[0].get('raw', '').startswith('@TIMELINE:'):
            # 03 简洁链路：不再丢动作库的 @TIMELINE；等当前 timeline 结束再送。
            return None
        else:
            cmd = q.popleft()
        if not q:
            _pending_commands.pop(device_id, None)
    motion_delivery.dispatched(cmd)
    _note_pop(cmd)      # [03网页端] 账本：这条被设备领走了（拿锁顺序 cmd → deliver，不会反过来）
    return cmd


# === [03网页端] BEGIN === 投递账本（2026-09-23）—— 只给网页挂徽标用，不碰链路
# ★ 为什么不写进 decision_log.txt：那份是 AI 的**记忆源**（load_memory_context 取末尾 86 行），
#   记进去 AI 会把同一句话当第二个记忆轮再记一遍。所以这里只留**进程内**的账：
#   重启后旧行如实显示"—"（云端当时也没记过），**别当故障查**。
# ★ 为什么需要它：板子**没有回执**（F407 不实现 @ACK），云端永远不可能知道"上屏了"。
#   这个账本只复述**可观测**的事实：入队了没 / 设备领走了没 / 为什么没下发。
_deliveries = deque(maxlen=50)
_deliver_seq = 0
_deliver_lock = threading.Lock()


def _note_delivery(device_id, ts, user, raw, state, err=""):
    """记一条投递尝试，返回 id（塞进 `cmd["_did"]`，被领走时回填 taken）。"""
    global _deliver_seq
    with _deliver_lock:
        _deliver_seq += 1
        rec = {"id": _deliver_seq, "time": ts, "device_id": device_id,
               "user": user, "raw": raw, "state": state, "err": err,
               "pop_at": ""}
        _deliveries.append(rec)
        return rec["id"]


def _note_pop(cmd):
    """命令被设备领走 ⇒ 把账本里那条 queued 改成 taken。"""
    did = (cmd or {}).get("_did")
    if not did:
        return
    with _deliver_lock:
        for rec in _deliveries:
            if rec["id"] == did and rec["state"] == "queued":
                rec["state"] = "taken"
                rec["pop_at"] = now_str()
                return


def _delivery_view(limit=20):
    try:
        n = max(1, min(50, int(limit)))
    except (TypeError, ValueError):
        n = 20
    with _deliver_lock:
        return list(_deliveries)[-n:]
# === [03网页端] END ===


def touch_device(device_id, ack=None):
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    with _cmd_lock:
        _device_last_seen[device_id] = datetime.now()
        if ack:
            _device_last_ack[device_id] = str(ack)


def now_str():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def append_line(path, line):
    with _io_lock:
        with open(path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        # 存储裁剪：文件超过 2MB 时只保留末尾 history_max_lines 行，
        # 防止 data_history.txt / decision_log.txt 无限膨胀（O(1)检查，只在超限时读文件）
        if os.path.getsize(path) > 2 * 1024 * 1024:
            try:
                max_lines = int(load_config().get("history_max_lines", 5000))
            except Exception:
                max_lines = 5000
            with open(path, "r", encoding="utf-8") as f:
                lines = f.readlines()
            if len(lines) > max_lines:
                with open(path, "w", encoding="utf-8") as f:
                    f.writelines(lines[-max_lines:])


def read_page(path, per_page, page):
    """分页读取：page 从1开始，第1页=最新 per_page 条（新的在前）。"""
    if not os.path.exists(path):
        return []
    with _io_lock:
        with open(path, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
    total = len(lines)
    if total == 0:
        return []
    per_page = max(1, per_page)
    page = max(1, page)
    total_pages = (total + per_page - 1) // per_page
    page = min(page, total_pages)
    start = max(0, total - page * per_page)
    end = total - (page - 1) * per_page
    window = lines[start:end]
    window.reverse()   # 新的在前
    return window


def read_tail(path, n):
    if not os.path.exists(path):
        return []
    with _io_lock:
        with open(path, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
    return lines if n <= 0 else lines[-n:]


def count_lines(path):
    if not os.path.exists(path):
        return 0
    with _io_lock:
        with open(path, "r", encoding="utf-8") as f:
            return sum(1 for _ in f)


def load_memory_context(cfg):
    """从 data_history.txt / decision_log.txt 取上下文。

    返回 (history, decisions)：
      history   —— 格式化文本（传感器趋势背景，喂给 system）
      decisions —— 结构化 dict 列表（供 ai_relay 拼多轮对话记忆）
    """
    n = cfg.get("memory_window", 10)
    n_dec = max(n, int(cfg.get("memory_turns", 6)) + int(cfg.get("memory_digest_lines", 80)))
    history, decisions = [], []
    for line in read_tail(DATA_HISTORY, n):
        try:
            r = json.loads(line)
            history.append("%s | 传感器:%s | 用户:%s" % (
                r.get("time", ""), r.get("sensor", ""), r.get("user", "")))
        except Exception:
            history.append(line)
    for line in read_tail(DECISION_LOG, n_dec):
        try:
            r = json.loads(line)
        except Exception:
            continue
        # ★ 「新对话」的记忆起点：从**最后一条**标记往后才算记忆。
        #   这里按"旧→新"扫，遇到标记就清空已攒的 —— 等价于只保留最后一个
        #   标记之后的行。标记若已被 read_tail 挤出窗口，说明窗口里全部比它新，
        #   压根不会走到这一支 ⇒ **文件被 append_line 裁过也不会算错**。
        if r.get("event") == MEMORY_RESET_EVENT:
            decisions = []
            continue
        decisions.append(r)
    return history, decisions


# ===================== 页面 =====================

@app.route("/")
def index():
    # 可视化仪表盘（HTML/JS 前端，由后端直接托管，与 API 同端口运行）
    return render_template("index.html", app_version=APP_VERSION,
                           skins=ui_skins.registry(),
                           servo_contract=device_profile.servo_contract.CONTRACT)


@app.route('/api/ui/skins')
def api_ui_skins():
    return jsonify(skins=ui_skins.registry())


def camera_blockers(device):
    # Conservative shared-bus policy. This is advisory until the camera MCU
    # implements its own final arbitration with recording/OTA/motion.
    with audio_rx._lock:
        audio_busy = any(time.time() - s.last_at < 30 for s in audio_rx._sessions.values())
    report = _audio_transport_status.get(device, {})
    try:
        fresh = (datetime.now() - datetime.strptime(report.get('at', ''), '%Y-%m-%d %H:%M:%S')).total_seconds() < 20
    except ValueError:
        fresh = False
    audio_busy = audio_busy or (fresh and report.get('phase') in ('capturing', 'recording', 'receiving', 'uploading', 'sending'))
    ota_busy = any(job.get('state') == 'running' for job in iap_pull.list_jobs())
    active = ota_console.active_job_id()
    if active:
        try:
            ota_busy = ota_busy or ota_console.get_job(active).get('state') == 'running'
        except ota_console.OtaError:
            pass
    pose = board_pose.get(device)
    return dict(audio=bool(audio_busy), ota=ota_busy, motion=sampled_player.busy(device) or
                (pose.get('fresh', False) and pose.get('moving', False)))


sampled_player = sampled_motion.Player()
sampled_library = sampled_motion.Library(os.path.join(BASE_DIR, 'sampled_actions.json'))
camera = camera_workspace.CameraWorkspace(os.path.join(BASE_DIR, 'camera_data'), lambda: load_config(), camera_blockers)
follow = camera_follow.Controller()
camera.on_analysis = follow.observe
camera.on_settings = follow.stop
camera.follow_status = follow.status
app.register_blueprint(camera_workspace.blueprint(camera))


def _presence_enqueue(device, kind):
    """上位机待机动作：走普通命令队列，不占录制播放器。"""
    try:
        if kind == 'look':
            profile = device_profile.load()
            speed = device_controls.get(device)['servo_speed']
            action = next((a for a in profile['actions'] if a['id'] == 'scan_lr'), None)
            if not action:
                return
            action, raw = _action_plan(action, profile, speed)
            if not raw:
                return
            cmd = {'raw': raw, 'note': 'presence_look'}
        elif kind == 'sleep':
            cmd = {'raw': '@MOVE:servo,2,60,180#@MOVE:expression,sleep#', 'note': 'presence_sleep'}
        elif kind == 'wake':
            cmd = {'raw': '@MOVE:home#@MOVE:expression,smile#', 'note': 'presence_wake'}
        else:
            return
        cmd.update(device_id=device, _presence=True)
        if enqueue_command(device, cmd) is not None:
            print('[PRESENCE] %s -> %s' % (kind, device))
    except Exception as exc:
        print('[PRESENCE] %s failed: %s' % (kind, exc))


def _note_sound(device, event):
    """录音一进来就算“有人”：复位待机计时；若在观望/休眠则回正。"""
    woke = presence.TRACKER.note(device, sound=True)
    try:
        camera.sound_event(device, event)
    except Exception as exc:
        print('[PRESENCE] sound_event failed: %s' % exc)
    if woke:
        _presence_yield(device, stop=True)
        _presence_enqueue(device, 'wake')


# ===================== 登录与访问控制 =====================

def check_token():
    """从 config.json 读取访问口令，改了配置不用重启。"""
    try:
        return str(load_config().get("token", "") or "")
    except Exception:
        return ""


@app.route("/login", methods=["GET", "POST"])
def login():
    """登录页：输入 config.json 里的 token 才能进仪表盘。"""
    if request.method == "POST":
        pwd = (request.form.get("token") or "").strip()
        if pwd and pwd == check_token():
            session["authed"] = True
            return redirect(url_for("index"))
        return render_template("login.html", error="口令错误，请重试")
    return render_template("login.html", error=None)


@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login"))


@app.before_request
def require_auth():
    """访问控制：
    - 登录页 / 静态资源 放行
    - ESP 上报 POST /api/data 允许 body 带 token
    - 其余页面/接口需要先登录（session）
    """
    # 登录页、登出、静态资源（CSS/JS）无需鉴权
    if request.path in ("/login", "/logout") or request.path.startswith("/static/"):
        return None
    # 已登录的直接放行
    if session.get("authed"):
        return None
    # LAN OTA 客户端使用 Bearer token；浏览器仍可用登录 session。
    # 只接受配置中的完整 token，不把它写进 URL 或日志。
    auth = request.headers.get("Authorization", "")
    # Only these two device endpoints accept Bearer auth. Images, settings and
    # the personnel console still require a browser session.
    if request.path in ('/api/camera/device/poll', '/api/camera/device/upload', '/api/camera/device/report', '/api/camera/device/live-frame'):
        token = check_token()
        if token and auth == 'Bearer ' + token:
            return None
    if request.path == "/api/f407/ota" and auth == "Bearer " + check_token():
        return None
    # ESP 走 POST /api/data，body 里带 token
    if request.path == "/api/data" and request.method == "POST":
        body = request.get_json(silent=True) or {}
        if body.get("token") == check_token():
            return None
    # OTA 接口：query 带 token 放行（ESP 更新固件用，无 session）
    # ★ [01语音] 音频上行那几个端点同样是 ESP 发的，它没有 session ⇒ 一并走 query token。
    #   ⚠️ 只放行 chunk / stream / end 这三个**给 ESP 的**；status / send / mode / drop
    #   是给网页控制台的，**必须走登录**，别顺手写宽了。
    # ★ [01语音] 2026-09-24 云端 OTA：`/api/iap/chunk`（拉固件块）与 `/api/iap/report`
    #   （报进度）也是**设备**发的 ⇒ 同样走 query token。
    #   ⚠️ 注意前缀差一个字母：**`/api/iap/` 是设备的**；**建任务/看进度是 `/api/iapl/`**
    #   （多一个 l），**故意不在这张白名单里** ⇒ 天然要求登录。
    #   ❌ 别为了"顺手"把 `/api/iapl/` 也放进来 —— 那等于把"谁能给板子刷固件"交给 token。
    if (request.path.startswith("/api/ota/") or request.path.startswith("/api/fw/")
            or request.path.startswith("/api/audio/chunk")
            or request.path.startswith("/api/audio/stream")
            or request.path.startswith("/api/audio/end")
            or request.path.startswith("/api/tts/speech")
            or request.path.startswith("/api/iap/chunk")
            or request.path.startswith("/api/iap/report")):
        if request.args.get("token") == check_token():
            return None
    # 未登录的 API 返回 401，页面跳去登录
    if request.path.startswith("/api/"):
        return jsonify({"error": "未授权，需要口令"}), 401
    return redirect(url_for("login"))


@app.route("/api/history")
def api_history():
    """给仪表盘用：分页数据 + 分页决策 + 累计条数。

    参数：page（数据页码，默认1）、dpage（决策页码，默认1）、per_page（默认20）。
    第1页 = 最新 per_page 条（新的在前）。
    """
    per_page = max(1, request.args.get("per_page", 10, type=int))     # 数据流默认10条
    dper_page = max(1, request.args.get("dper_page", 20, type=int))   # 决策历史默认20条
    data_page = max(1, request.args.get("page", 1, type=int))
    dec_page = max(1, request.args.get("dpage", 1, type=int))

    data, decisions = [], []
    for line in read_page(DATA_HISTORY, per_page, data_page):
        try:
            r = json.loads(line)
            data.append({"time": r.get("time", ""),
                         "sensor": r.get("sensor", ""),
                         "user": r.get("user", "")})
        except Exception:
            pass
    for line in read_page(DECISION_LOG, dper_page, dec_page):
        try:
            r = json.loads(line)
            # 「新对话」的记忆起点：数据上报/回填都安全跳过它（store.py 里
            # `if not user: continue`），但**表里要看得见** —— 用户才知道
            # AI 的记忆从哪断的。
            if r.get("event") == MEMORY_RESET_EVENT:
                decisions.append({"time": r.get("time", ""),
                                  "message": "✨ 新对话（记忆起点）",
                                  "event": MEMORY_RESET_EVENT,
                                  "note": r.get("note", "")})
                continue
            dec = {"time": r.get("time", ""),
                   "provider": r.get("provider", ""),
                   "model": r.get("model", ""),
                   "key": r.get("key", ""),
                   "sensor": r.get("sensor", ""),
                   "user": r.get("user", ""),
                   "action": r.get("action", ""),
                   "value": r.get("value", ""),
                   "message": r.get("message", "")}
            # 扩展字段（angle/expression/duration_ms...）原样透传给前端
            for k, v in r.items():
                if k not in dec:
                    dec[k] = v
            decisions.append(dec)
        except Exception:
            pass
    data_total = count_lines(DATA_HISTORY)
    decision_total = count_lines(DECISION_LOG)
    return jsonify({
        "data": data,
        "decisions": decisions,
        "data_total": data_total,
        "decision_total": decision_total,
        "page": data_page,
        "dpage": dec_page,
        "per_page": per_page,
        "dper_page": dper_page,
        "data_pages": max(1, (data_total + per_page - 1) // per_page),
        "decision_pages": max(1, (decision_total + dper_page - 1) // dper_page),
        "server_time": now_str(),
    })


@app.route("/api/health")
def health():
    cfg = load_config()
    active = cfg.get("active_provider", "auto")
    providers = {}
    for name, p in cfg.get("providers", {}).items():
        would_use = (active == "auto" and p.get("enabled") and p.get("keys")) \
                    or (active == name and p.get("keys"))
        providers[name] = {
            "enabled": bool(p.get("enabled")),
            "model": p.get("model", ""),
            "key_count": len(p.get("keys", [])),
            "would_use": bool(would_use),
        }
    return jsonify({"status": "ok", "active_provider": active,
                    "providers": providers})


# ===================== 模型管理（网页上新增/切换模型） =====================

@app.route("/api/provider/add", methods=["POST"])
def provider_add():
    """网页新增一个AI模型（写入 config.json，无需重启）。"""
    body = request.get_json(silent=True) or {}
    name = str(body.get("name", "")).strip()
    model = str(body.get("model", "")).strip()
    key = str(body.get("key", "")).strip()
    base_url = str(body.get("base_url", "")).strip().rstrip("/")
    if not name or not model or not key:
        return jsonify({"error": "显示名称 / 模型名称 / API Key 不能为空"}), 400
    cfg = load_config()
    cfg.setdefault("providers", {})[name] = {
        "enabled": True,
        "base_url": base_url or "https://api.deepseek.com/v1",
        "model": model,
        "keys": [key],
        "temperature": 0.7,
        "max_tokens": 512,
        "json_mode": True,
    }
    save_config(cfg)
    return jsonify({"ok": True, "name": name})


@app.route("/api/provider/switch", methods=["POST"])
def provider_switch():
    """切换当前使用的模型（active_provider）。不可用的模型拒绝切换。"""
    body = request.get_json(silent=True) or {}
    name = str(body.get("provider", "")).strip()
    cfg = load_config()
    if name != "auto":
        p = cfg.get("providers", {}).get(name)
        if not p or not p.get("enabled") or not p.get("keys"):
            return jsonify({"error": "该模型不可用，无法切换"}), 400
    cfg["active_provider"] = name
    save_config(cfg)
    return jsonify({"ok": True, "active_provider": name})


# ===================== AI 设定（网页只开放 persona；决策帧/硬件接口锁死） =====================

@app.route("/api/settings", methods=["GET"])
def settings_get():
    """读取可在网页修改的 AI 设定。硬件接口 / 决策帧格式不开放。"""
    cfg = load_config()
    return jsonify({
        "persona": cfg.get("persona", ""),
        "locked": {
            "note": "决策信封+moves / move_types / hardware.servos / ESP 类型化帧 不可在网页修改",
            "move_types": cfg.get("move_types") or [],
            "hardware_servos": (cfg.get("hardware") or {}).get("servos") or [],
        },
    })


@app.route("/api/settings", methods=["POST"])
def settings_save():
    """只允许改 persona（
    性格/说话方式）。其它键一律忽略，防止改坏决策帧。"""
    body = request.get_json(silent=True) or {}
    if "persona" not in body:
        return jsonify({"error": "只支持修改 persona"}), 400
    persona = str(body.get("persona", "") or "").strip()
    if not persona:
        return jsonify({"error": "persona 不能为空"}), 400
    if len(persona) > 2000:
        return jsonify({"error": "persona 太长（最多 2000 字）"}), 400
    cfg = load_config()
    cfg["persona"] = persona
    # 明确不写入 move_types / hardware / rules 结构等，即使 body 里带了也丢掉
    save_config(cfg)
    return jsonify({"ok": True, "persona": persona})


# ===================== 管理员设备档案 / 动作库 =====================
# 登录 session 就是此小型单用户控制台的管理员权限。档案不混入 config.json，
# 因为 config.json 包含模型密钥；这里改的都是可验收的机械/表情参数。
@app.route("/api/admin/device-profile", methods=["GET"])
def admin_device_profile_get():
    return jsonify({"ok": True, "profile": device_profile.load()})


@app.route("/api/admin/device-profile", methods=["POST"])
def admin_device_profile_save():
    body = request.get_json(silent=True) or {}
    try:
        profile = (device_profile.save_parameters(body.get("profile")) if body.get("parameters_only") is True
                   else device_profile.save(body.get("profile")))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    return jsonify({"ok": True, "profile": profile,
                    "message": "管理员设备参数已保存；下一次 AI 决策立即使用新档案"})


@app.route("/api/actions", methods=["GET"])
def actions_get():
    profile = device_profile.load()
    device = str(request.args.get("device_id") or DEFAULT_DEVICE_ID)
    speed = device_controls.get(device)["servo_speed"]
    actions = [_action_plan(a, profile, speed)[0] for a in profile["actions"]]
    return jsonify({"ok": True, "actions": actions, "servo_speed": speed,
                    "expressions": profile["expressions"],
                    "servos": device_profile.effective(profile, speed)["servos"]})


@app.route("/api/admin/servo-speeds", methods=["POST"])
def admin_servo_speeds_save():
    body = request.get_json(silent=True) or {}
    try:
        profile = device_profile.save_speeds(body.get("speeds"))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    return jsonify({"ok": True, "profile": profile,
                    "message": "逐轴默认速度已保存；下一次AI、动作库和时间动作使用新速度"})


def _action_plan(action, profile, speed):
    """Compile from one current snapshot for preview and dispatch, never cached angles."""
    result = dict(action)
    if not result.get("ready"):
        return result, ""
    try:
        if result.get("timeline"):
            raw, plan = motion_timeline.compile_timeline(result["timeline"], profile, speed)
            result["plan"] = plan
            result["duration_ms"] = plan["duration_ms"] * plan["repeat"]
        elif result["id"] == "home":
            if any("speed" in s for s in profile["servos"]):
                raw, plan = motion_timeline.compile_timeline(
                    {"steps": [{"at_ms": 0, "move": {"type": "home"}}]}, profile, speed)
                result["plan"] = plan
            else:
                raw = "@MOVE:home,all"
        else:
            faces = {f["id"]: f["file"].rsplit(".", 1)[0].lower() for f in profile["expressions"]}
            raw = "@MOVE:expression," + faces[result["moves"][0]["face"]]
        return result, raw
    except (ValueError, KeyError) as exc:
        result.update(ready=False, unavailable_reason="当前参数无法编排：%s" % exc)
        return result, ""


@app.route("/api/actions", methods=["POST"])
def actions_run():
    body = request.get_json(silent=True) or {}
    action_id = str(body.get("action_id", "") or "").strip()
    device_id = str(body.get("device_id", "") or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    profile = device_profile.load()
    action = next((a for a in profile["actions"] if a["id"] == action_id), None)
    if action is None:
        return jsonify({"ok": False, "error": "动作库中没有这个动作"}), 404
    action, raw = _action_plan(action, profile, device_controls.get(device_id)["servo_speed"])
    if not action.get("ready"):
        return jsonify({"ok": False, "ready": False,
                        "error": action.get("unavailable_reason", "当前配置不能执行此动作"),
                        "action": action}), 409
    if not raw:
        return jsonify({"ok": False, "error": "动作配方没有可下发帧"}), 400
    ts = now_str()
    cmd = {"time": ts, "device_id": device_id, "raw": raw,
           "note": "action_library:" + action_id}
    try:
        qlen = enqueue_command(device_id, cmd)
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 409
    append_line(DATA_HISTORY, json.dumps({"time": ts, "sensor": "", "user": "",
        "mode": "action_library", "device_id": device_id, "event": "enqueue", "command": cmd},
        ensure_ascii=False))
    return jsonify({"ok": True, "action": action, "device_id": device_id,
                    "queue_len": qlen, "raw": raw})


@app.route("/api/timeline/preview", methods=["POST"])
def timeline_preview():
    body = request.get_json(silent=True) or {}
    device = str(body.get("device_id") or DEFAULT_DEVICE_ID)
    try:
        frame, timeline = motion_timeline.compile_timeline(body.get("timeline"), device_profile.load(),
                                            device_controls.get(device)["servo_speed"])
        return jsonify({"ok": True, "timeline": timeline,
                        "steps": len(timeline.get("steps", [])), "message": "预览完成，尚未下发"})
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400


@app.route('/api/recorded-motion/<operation>', methods=['GET', 'POST'])
def recorded_motion(operation):
    """登录网页使用；预览/保存无硬件副作用，只有start和stop改变播放状态。"""
    body = request.get_json(silent=True) or {}
    device = str(body.get('device_id') or request.args.get('device_id') or DEFAULT_DEVICE_ID)
    try:
        if not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', device):
            raise ValueError('设备编号无效')
        if request.method == 'GET' and operation == 'status':
            card = dict(_gateway_step.get(device) or {})
            if card.get('seen'):
                card['age_sec'] = round(time.monotonic() - card['seen'], 1)
            return jsonify(ok=True, playback=sampled_player.status(device), gateway=card)
        if request.method == 'GET' and operation == 'library':
            return jsonify(ok=True, actions=sampled_library.read())
        if request.method != 'POST' or operation not in ('preview', 'start', 'save', 'stop'):
            return jsonify(ok=False, error='接口或方法不支持'), 405
        if operation == 'stop':
            return jsonify(ok=True, playback=sampled_player.stop(device))
        if operation == 'start':
            return jsonify(ok=False,
                           error='03 简洁链路已禁用录制长动作；请使用动作库'), 410
        value = sampled_library.read().get(body['action_id']) if body.get('action_id') else body.get('action')
        profile = device_profile.effective(device_profile.load(), device_controls.get(device)['servo_speed'])
        compiled = sampled_motion.compile_recording(value, profile)
        if operation == 'preview':
            return jsonify(ok=True, compiled=compiled)
        if operation == 'save':
            return jsonify(ok=True, action_id=sampled_library.save(value))
        # 录音/OTA 忙时不要先取消待机；确认能发送后再让路。
        blockers = camera_blockers(device)
        if blockers['audio'] or blockers['ota']:
            return jsonify(ok=False, error='录音、OTA或运动中，请稍后发送'), 409
        # 用户发送录制动作优先：清掉待机命令，先停掉待机 timeline，再等舵机停稳。
        _presence_yield(device, stop=True)
        blockers = camera_blockers(device)
        if blockers['motion']:
            for _ in range(8):
                time.sleep(0.25)
                blockers = camera_blockers(device)
                if not blockers['motion']:
                    break
        if blockers['audio'] or blockers['ota'] or blockers['motion']:
            return jsonify(ok=False, error='录音、OTA或运动中，请稍后发送'), 409
        card = _gateway_step.get(device)
        if card and time.monotonic() - card['seen'] <= 15 and not card['capable']:
            return jsonify(ok=False, error='ESP网关固件过旧：未上报 step_playback，无法按时间轴执行；请升级 ESP8266 到 1.1.25'), 409
        with _cmd_lock:
            q = _pending_commands.get(device)
            if any(not c.get('_presence') for c in (q or [])):
                return jsonify(ok=False, error='普通指令队列尚未清空，请等待后发送'), 409
            playback = sampled_player.start(device, compiled)
        presence.TRACKER.note(device)
        follow.stop(device, '录制动作优先，等待完成后的新照片', hold=5)
        return jsonify(ok=True, playback=playback)
    except (ValueError, KeyError, TypeError, AttributeError) as exc:
        return jsonify(ok=False, error=str(exc)), 400


@app.route("/api/command", methods=["POST"])
def api_command():
    """网页遥控入队：ESP 下次 POST /api/data 时从响应里取走一条 command。"""
    body = request.get_json(silent=True) or {}
    device_id = str(body.get("device_id", "") or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    raw = str(body.get("raw", "") or "").strip()
    text = str(body.get("text", "") or "").strip()
    note = str(body.get("note", "") or "").strip()
    moves = body.get("moves")
    if moves is not None and not isinstance(moves, list):
        return jsonify({"error": "moves 必须是数组"}), 400
    timeline = body.get("timeline")
    try:
        motion_timeline.validate_raw(raw, device_profile.load())
        if timeline is not None:
            if raw or moves:
                raise ValueError("timeline 不可和 raw/moves 混发，请把动作放入 steps")
            frame, normalized = motion_timeline.compile_timeline(timeline, device_profile.load(),
                                               device_controls.get(device_id)["servo_speed"])
            raw = frame + "#"
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    if not raw and not text and not moves:
        return jsonify({"error": "raw / text / moves 至少填一个"}), 400

    cmd = {"time": now_str(), "device_id": device_id}
    if raw:
        cmd["raw"] = raw
    if timeline is not None:
        # Only send compact raw wire data, not a second large copy of steps.
        cmd["note"] = "motion_timeline"
    if text:
        cmd["text"] = text
    if moves is not None:
        cmd["moves"] = moves
    if note:
        cmd["note"] = note

    try:
        qlen = enqueue_command(device_id, cmd)
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 409
    append_line(DATA_HISTORY, json.dumps({
        "time": cmd["time"], "sensor": "", "user": "",
        "mode": "cmd", "device_id": device_id,
        "event": "enqueue", "command": cmd,
    }, ensure_ascii=False))
    try:
        store.record_command(cmd["time"], device_id, raw=cmd.get("raw", ""),
                             source="web", note=cmd.get("note", "") or "enqueue")
    except Exception as e:
        print("[STORE][ERROR] 命令落库失败（不影响入队）：%s: %s"
              % (type(e).__name__, e))
    print("[%s] 入队 device=%s queue=%d cmd=%s" % (
        cmd["time"], device_id, qlen, json.dumps(cmd, ensure_ascii=False)))
    return jsonify({"ok": True, "device_id": device_id, "queue_len": qlen, "command": cmd})


# === [03网页端] BEGIN === 投递账本只读端点（2026-09-23）
@app.route("/api/motion/delivery")
def api_motion_delivery():
    return jsonify(ok=True, delivery=motion_delivery.view(request.args.get("id", "")[:40]))


@app.route("/api/deliveries")
def api_deliveries():
    """网页据此给每条 AI 回复挂「已入队 / 设备已取走 / 未下发+原因」徽标。

    ★ 只复述可观测事实，**不是回执**：`taken` 只表示"被某次轮询领走"，
      之后 ESP 还可能超时 / JSON 解析失败 / F407 行长超限丢帧 —— 网页都看不到。
      ⚠️ 所以这里（以及用它的页面）**永远不出现"已上屏"三个字**。
    """
    device_id = str(request.args.get("device_id", "") or DEFAULT_DEVICE_ID).strip() \
        or DEFAULT_DEVICE_ID
    return jsonify({
        "device_id": device_id,
        "online": (lambda a: bool(a is not None and a <= DEVICE_ONLINE_SEC))(
            device_age_sec(device_id)),
        "queue_len": _queue_len(device_id),
        "deliveries": _delivery_view(request.args.get("limit") or 20),
        "text_transport": (_motion_status.get(device_id) or {}).get("text_transport", {}),
    })
# === [03网页端] END ===


@app.route("/api/devices")
def api_devices():
    """设备在线状态 + 队列长度 + 最近 ACK（供远程测试台刷新）。"""
    now = datetime.now()
    with _cmd_lock:
        ids = set(_device_last_seen) | set(_pending_commands) | set(_device_last_ack)
        rows = []
        for did in sorted(ids):
            seen = _device_last_seen.get(did)
            age = (now - seen).total_seconds() if seen else None
            rows.append({
                "device_id": did,
                "last_seen": seen.strftime("%Y-%m-%d %H:%M:%S") if seen else "",
                "online": bool(age is not None and age <= DEVICE_ONLINE_SEC),
                "poll_age_sec": round(max(0, age), 1) if age is not None else None,
                "poll_fresh": bool(age is not None and age <= 5),
                "queue_len": len(_pending_commands.get(did) or []),
                "last_ack": _device_last_ack.get(did, ""),
                "motion": _motion_status.get(did, {}),
                # === [03网页端] BEGIN === 2026-09-23 远程OTA页：纯增量字段，
                # 老前端忽略它；远程OTA页用它把 device_id 解析成可直连的地址。
                "ip": _device_ip.get(did, ""),
                # === [03网页端] END ===
            })
    return jsonify({"devices": rows, "server_time": now_str(),
                    "online_sec": DEVICE_ONLINE_SEC})


@app.route("/api/trend")
def api_trend():
    """给统计图用：最近 hours 小时内的温湿度数据（旧→新，最多 limit 条）。"""
    hours = max(1, request.args.get("hours", 3, type=int))
    limit = max(1, request.args.get("limit", 400, type=int))
    cutoff = datetime.now() - timedelta(hours=hours)
    points = []
    for line in read_tail(DATA_HISTORY, 5000):
        try:
            r = json.loads(line)
            t = str(r.get("time", ""))
            try:
                ts = datetime.strptime(t, "%Y-%m-%d %H:%M:%S")
            except Exception:
                continue
            if ts >= cutoff:
                points.append({"time": t, "sensor": r.get("sensor", "")})
        except Exception:
            pass
    return jsonify({"points": points[-limit:], "hours": hours})


# ===================== OTA（无线固件更新） =====================

@app.route("/api/ota/info")
def ota_info():
    """ESP 查询是否有新固件。带 token 放行（见 require_auth）。"""
    cfg = load_config()
    ver = str(cfg.get("firmware_version", "0.0.0"))
    fw_name = cfg.get("firmware_file", "sketch.ino.bin")
    # 返回下载地址（含 token，ESP 拿到直接能下载）
    return jsonify({
        "version": ver,
        "url": "http://%s/api/fw/%s?token=%s" % (
            request.host, fw_name, check_token()),
    })


@app.route("/api/f407/ota", methods=["POST"])
def api_f407_ota():
    """LAN 模拟公网的 F407 OTA 中转接口。

    表单字段：esp_ip、可选 device_id；文件字段 firmware。接口先让 F407
    在屏幕上等待 K-UP，确认后才把 BIN 流给 ESP，因此请求阶段不会擦写。
    """
    esp_ip = (request.form.get("esp_ip") or request.args.get("esp_ip") or "").strip()
    upload = request.files.get("firmware")
    if not esp_ip or upload is None:
        return jsonify({"ok": False, "error": "需要 esp_ip 和 firmware BIN 文件"}), 400
    data = upload.stream.read(f407_ota_relay.MAX_APP + 1)
    try:
        result = f407_ota_relay.perform_lan_ota(
            esp_ip, data, check_token(),
            approval_timeout=max(1, min(request.form.get("approval_timeout", 120, type=int), 120)))
    except f407_ota_relay.RelayError as exc:
        return jsonify({"ok": False, "error": str(exc), "esp_ip": esp_ip}), 409
    return jsonify(result)


@app.route("/api/fw/<path:filename>")
def ota_download(filename):
    """下载固件 .bin。带 token 放行。"""
    fp = os.path.join(FW_DIR, os.path.basename(filename))
    if not os.path.exists(fp):
        return jsonify({"error": "固件不存在: %s" % filename}), 404
    return send_file(fp, mimetype="application/octet-stream",
                     as_attachment=False, conditional=True)


# ===================== 「远程OTA」页（/api/otactl/*） =====================
# 网页端那一页（上传 F407 固件 + 按 IP 或名字点一下烧录）的服务端。
#
# ★ 前缀为什么**不是** `/api/ota/`：`require_auth()` 把 `/api/ota/` 与
#   `/api/fw/` 两个前缀做成「query 带 token 即免登录」，而上面的注释明令
#   「给网页控制台用的端点**必须走登录**」。放 `/api/otactl/` 才会落进
#   `/api/` 的 401 规则 ⇒ 天然要求登录 session。**别顺手把前缀改回去。**
# ★ 这 6 条都是**薄壳**：固件库 / 任务 / 目标解析全在 `ota_console.py`。
# ★ 既有的 `POST /api/f407/ota`（同步阻塞版）**原样保留** —— 交付包的
#   `无线更新F407_局域网中转.bat` 与 `Tools/ota/update_f407_via_relay.py`
#   依赖它；本次只是**另加**任务式接口，没有替换。
def _otactl_err(exc, code=400):
    return jsonify({"ok": False, "error": str(exc)}), code


@app.route("/api/otactl/fw", methods=["GET"])
def api_otactl_fw_list():
    """固件库列表。每个文件都当场体检 ⇒ 页面看到的就是**能烧的**。"""
    return jsonify({"ok": True, "items": ota_console.list_firmware(),
                    "max_bytes": ota_console.MAX_APP,
                    "dir": os.path.basename(ota_console.FW407_DIR)})


@app.route("/api/otactl/fw", methods=["POST"])
def api_otactl_fw_upload():
    """上传一个 F407 应用 BIN（multipart 字段名 `firmware`）。

    ★ 体检不过的镜像**不落盘**，但返回 200 + `valid:false` + 人话原因，
      好让页面把"为什么不行"直接摆给用户看（400 只留给空文件/超大/非法文件名）。
    """
    upload = request.files.get("firmware")
    if upload is None:
        return _otactl_err(ota_console.OtaError("需要 multipart 字段 firmware"))
    data = upload.stream.read(ota_console.MAX_UPLOAD + 1)
    try:
        return jsonify(ota_console.save_firmware(upload.filename, data))
    except ota_console.OtaError as exc:
        return _otactl_err(exc)


@app.route("/api/otactl/fw/<path:filename>", methods=["DELETE"])
def api_otactl_fw_delete(filename):
    """删库内固件。只认库内清单里的名字 ⇒ 不碰库外任何路径。"""
    try:
        return jsonify({"ok": True, "name": ota_console.delete_firmware(filename)})
    except ota_console.OtaError as exc:
        return _otactl_err(exc, 404)


@app.route("/api/otactl/flash", methods=["POST"])
def api_otactl_flash():
    """起一个后台烧录任务，**立即**返回 job_id（整条链路最长约 9 分钟）。

    ★ token 在这里取一次就交给线程 —— 后台线程里没有 request 上下文，
      `check_token()` / `request` 全部不能在线程里碰。
    """
    body = request.get_json(silent=True) or {}
    with _cmd_lock:
        ip_map = dict(_device_ip)
    try:
        snap = ota_console.start_flash_job(
            body.get("target"), body.get("name"), check_token(), ip_map)
    except ota_console.JobBusy as exc:
        return _otactl_err(exc, 409)
    except ota_console.OtaError as exc:
        return _otactl_err(exc)
    return jsonify({"ok": True, "job_id": snap["id"], "transport": snap["transport"],
                    "esp_ip": snap["esp_ip"], "known_device": snap["known_device"],
                    "bytes": snap["bytes"], "crc32": snap["crc32"]})


@app.route("/api/otactl/job/<job_id>")
def api_otactl_job(job_id):
    """任务快照（前端 1 秒轮询一次；`phase` 驱动页面文案）。"""
    try:
        return jsonify({"ok": True, "job": ota_console.get_job(job_id)})
    except ota_console.OtaError as exc:
        return _otactl_err(exc, 404)


@app.route("/api/otactl/jobs")
def api_otactl_jobs():
    """任务列表（新的在前）。刷新页面/切页后被页面用来把在跑的任务接回来。"""
    return jsonify({"ok": True, "jobs": ota_console.list_jobs()})


@app.route("/api/otactl/preflight")
def api_otactl_preflight():
    """烧之前先答一句"够得着吗"：目标解析 + 同网段判断 + 公网部署提示。

    ★ 这是给用户看的**诚实诊断**，不是假失败：云端部署时服务端学到的
      `remote_addr` 是反代地址，直刷必然连不上，这一步要提前说清楚。
    """
    target = (request.args.get("target") or "").strip()
    with _cmd_lock:
        ip_map = dict(_device_ip)
        seen = dict(_device_last_seen)
    try:
        esp_ip, known = ota_console.resolve_target(target, ip_map)
    except ota_console.OtaError as exc:
        return _otactl_err(exc)

    # 用内核选路问出"本机面对这个目标的那张网卡"（别随便挑一张，这台机器上
    # 还有 VMware 的 192.168.255.1 —— 见 _local_ip_for 的注释）。
    host_ip = _local_ip_for(esp_ip)
    try:
        same = bool(host_ip) and (
            ipaddress.ip_network(host_ip + "/24", strict=False) ==
            ipaddress.ip_network(esp_ip + "/24", strict=False))
    except ValueError:
        same = False

    last = seen.get(known) if known else None
    online = bool(last and (datetime.now() - last).total_seconds() <= DEVICE_ONLINE_SEC)
    if not host_ip:
        warn = ("没能确定本机到该设备的网卡 —— Flask 可能跑在一台没有到 %s 那段"
                "路由的机器上" % esp_ip)
    elif not same:
        warn = ("目标和 Flask 不在同一个 /24 网段（本机 %s）⇒ 局域网直刷多半到不了"
                "设备。若 Flask 已部署到公网，它无法直接访问设备所在的局域网（NAT），"
                "将来要靠一台**局域网内的代理**去领任务再转发 —— 那一步尚未实现。"
                % host_ip)
    elif known and not online:
        warn = "设备「%s」当前不在线（最后上报 %s）" % (
            known, last.strftime("%Y-%m-%d %H:%M:%S") if last else "从未")
    else:
        warn = ""
    return jsonify({"ok": True, "esp_ip": esp_ip, "known_device": known,
                    "host_ip": host_ip, "same_subnet": bool(same), "online": online,
                    "last_seen": last.strftime("%Y-%m-%d %H:%M:%S") if last else "",
                    "warn": warn})


# ============ 云端 OTA 下行「设备主动拉」（/api/iap/* 给设备、/api/iapl/* 给网页） ============
# === [01语音] BEGIN 2026-09-24 ===
# 为什么另起一套：ESP 在 NAT 后**没有公网 IP** ⇒ 云端**连不进** `ESP:8080/f407/*`，
# 所以上面那套 `/api/otactl/*`（服务端主动打设备 8080）**上云后物理上做不到**。
# 本块走 `合作表 §4.6` 登记的形态：**设备自己来拉**。逻辑全在 `iap_pull.py`，
# 这里只是薄壳；固件库**复用** `ota_console` 的 `firmware_f407/`，不另建一份。
#
# ★★ 两个前缀**故意分开**（照上面 `otactl` 那一课的同一个道理，见 `:696-699`）：
#   `/api/iap/…`   = **给设备**的（问活 / 拉块 / 报进度）⇒ query token 免登录
#   `/api/iapl/…`  = **给网页 / CLI** 的（建任务 / 看进度 / 取消）⇒ 落进 `/api/` 的 401
#                    ⇒ **必须登录**
#   ❌ 别把它们并成一个前缀：一并就必然要把「建任务」也做成免登录，
#      那等于把"谁能给板子刷固件"这件事交给任何一个拿到 token 的 URL。
# ★ 设备**怎么知道有活**：不新增轮询 —— 公告搭在它每秒一次的 `POST /api/data`
#   响应里（`iap_pull.announce()`），见下面 `api_data()` 里那三行。反应 ≤1 秒。

@app.route("/api/iap/chunk")
def api_iap_chunk():
    """设备拉第 ``seq`` 块固件。★ ``seq`` 是**块号（0 基）不是字节偏移**。

    返回**裸字节**（``application/octet-stream``），长度 ≤ ``iap_pull.BLOCK_BYTES``；
    **末块不补齐**（见 ``iap_pull`` 模块头的不变量）。
    已经拉完 / 越界 / 没认领 → 409 + ``{"eof":true}``，设备据此收手。
    """
    device_id = (request.args.get("device_id") or "").strip()
    seq = request.args.get("seq", type=int)
    if seq is None:
        return jsonify({"ok": False, "err": "缺少 seq（块号，0 基）"}), 400
    data, eof, err = iap_pull.read_chunk(device_id, seq)
    if err:
        return jsonify({"ok": False, "err": err, "eof": True}), 409
    if eof:
        return jsonify({"ok": True, "eof": True}), 409
    resp = app.response_class(data, mimetype="application/octet-stream")
    resp.headers["X-Iap-Seq"] = str(seq)
    resp.headers["X-Iap-Bytes"] = str(len(data))
    return resp


@app.route("/api/iap/report", methods=["POST"])
def api_iap_report():
    """设备报进度 / 报结果。``slot_crc32``（``@IAP:INFO`` 回来的 ``slot_crc``）
    等于任务的 ``slot_crc32_padded`` 才是 **PASS**。"""
    body = request.get_json(silent=True) or {}
    device_id = str(body.get("device_id")
                    or request.args.get("device_id") or "").strip()
    if not device_id:
        return jsonify({"ok": False, "err": "缺少 device_id"}), 400
    return jsonify(iap_pull.report(device_id, body))


@app.route("/api/iapl/job", methods=["POST"])
def api_iapl_job_create():
    """从**已有**的固件库里挑一个 BIN，建一个"设备主动拉"的下发任务。

    固件的"入库"仍然走 `/api/otactl/fw` 的上传（网页那一页），这里只挑不传。
    """
    body = request.get_json(silent=True) or {}
    try:
        snap = iap_pull.create_job(body.get("name"), body.get("device_id"))
    except iap_pull.IapBusy as exc:
        return jsonify({"ok": False, "error": str(exc)}), 409
    except iap_pull.IapError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    return jsonify({"ok": True, "job_id": snap["id"], "transport": snap["transport"],
                    "name": snap["name"], "target_device": snap["target_device"], "bytes": snap["bytes"],
                    "blocks": snap["blocks"], "crc32": snap["crc32"],
                    "slot_crc32_padded": snap["slot_crc32_padded"]})


@app.route("/api/iapl/jobs")
def api_iapl_jobs():
    """任务列表（含历史）。**登录才能看** —— 固件名和进度不该向未登录者公开。"""
    return jsonify({"ok": True, "jobs": iap_pull.list_jobs()})


@app.route("/api/iapl/job/<job_id>")
def api_iapl_job_get(job_id):
    """单个任务快照（``phase`` 驱动页面文案，与 `/api/otactl/job/<id>` 同形状）。"""
    try:
        return jsonify({"ok": True, "job": iap_pull.get_job(job_id)})
    except iap_pull.IapError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 404


@app.route("/api/iapl/job/<job_id>/abort", methods=["POST"])
def api_iapl_job_abort(job_id):
    """取消任务。**不碰设备** —— 没开始写 Flash 的任务直接作废。"""
    try:
        return jsonify({"ok": True, "job": iap_pull.abort_job(job_id)})
    except iap_pull.IapError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 404
# === [01语音] END ===


# ===================== 定时唤醒（类"定时器中断"） =====================
# 把嵌入式里的「定时器中断 → ISR 里干活」搬到服务器侧，结构一一对应：
#   config.auto_tick.enabled / interval_sec  ←→  定时器的 CR1/ARR（开关 + 周期）
#   auto_tick_loop()                          ←→  定时器本体（一直在跑，到点才动）
#   auto_tick_once()                          ←→  中断服务程序（干一次活就返回）
#   条件判断（在线/队列/静默期）               ←→  进 ISR 先看标志位，不满足就直接退出
#
# 和真定时器的唯一区别：服务器**不能主动连 ESP**（NAT 穿透：通信永远由 ESP 发起），
# 所以这里的"发送数据" = 把决策塞进 _pending_commands 队列，等 ESP 下次
# POST /api/data 时从响应里捎带回去执行。

def device_age_sec(device_id):
    """设备最近一次上报距今多少秒；从没上报过返回 None。"""
    with _cmd_lock:
        seen = _device_last_seen.get(device_id)
    if not seen:
        return None
    return (datetime.now() - seen).total_seconds()


def latest_sensor(limit=50):
    """取最近一条真实传感器数据（跳过空值和 "无数据" 占位）。

    F407 目前还没有周期性上报 @SENSOR，所以这里多半取不到 —— 那是硬件侧的事，
    取不到就交给 AI 当"无数据"处理，不影响定时器本身工作。
    """
    for line in reversed(read_tail(DATA_HISTORY, limit)):
        try:
            r = json.loads(line)
        except Exception:
            continue
        s = str(r.get("sensor", "") or "").strip()
        if s and s != "无数据":
            return s
    return ""


def _clean_frame_text(s):
    """@TEXT: 的内容不能带 @ # 和换行 —— ESP 就是靠这三个字符拆帧的。"""
    s = str(s or "").replace("@", "").replace("#", "")
    return " ".join(s.split())


# === [03网页端] BEGIN === 上屏文字闸门（2026-09-23）
def _text_cells(ch):
    """一个字符在屏上占几格：可打印 ASCII 走 8px=1 格，其余走 16px=2 格。
    ★ 口径必须跟 media.c::draw_text() 一致（:366 `cp >= 32 && cp <= 126` 判 8px），
      否则我算"放得下"、屏上却翻到第 7 行被 `truncated=1` 砍掉。"""
    cp = ord(ch)
    return 1 if 32 <= cp <= 126 else 2


def _text_bytes(s):
    return len(str(s or "").encode("utf-8"))


def _screen_text(s, max_bytes=None, max_cells=None):
    """把一句 AI 回复裁成"屏幕上真的能显示完"的样子 → (text, truncated)。

    ★ 为什么必须有这道闸：F407 对 `@TEXT:` 超长的行是**整行静默丢弃**（见顶部
      SCREEN_TEXT_MAX_BYTES 注释）⇒ 症状是"AI 说了、屏上什么都没有、零报错"。
    ★ 为什么按"字节 + 格数"两个维度卡：纯 ASCII 时字节先到顶（168 B = 168 格），
      纯汉字时字节先到顶（168 B = 56 字 = 112 格）—— 只看一个维度，另一边就是浪费。
    ★ 换行会被 _clean_frame_text 折成空格：ESP 那条 `@TEXT:` 是
      `Serial.print(帧 + "\\n")`，正文里带换行会被切成两行、第二行不是白名单帧。
    """
    max_bytes = SCREEN_TEXT_MAX_BYTES if max_bytes is None else max_bytes
    max_cells = SCREEN_TEXT_MAX_CELLS if max_cells is None else max_cells
    txt = _clean_frame_text(s)
    if not txt:
        return "", False
    if _text_bytes(txt) <= max_bytes and sum(_text_cells(c) for c in txt) <= max_cells:
        return txt, False

    out, nb, nc, cut = [], 0, 0, 0
    for ch in txt:
        b, c = _text_bytes(ch), _text_cells(ch)
        if nb + b > max_bytes or nc + c > max_cells:
            break
        out.append(ch)
        nb += b
        nc += c
        if ch in SCREEN_BREAK_CHARS:
            cut = len(out)
    # 能切在标点上就切（是句完整的话）；但别为了一个标点把大半句都扔了
    if cut >= 1 and cut * 2 >= len(out):
        out = out[:cut]
    return "".join(out).strip(), True


def _frame_int(v, default):
    """帧里的整数参数 → int；**有值但解不出来**返回 None（调用方跳过并告警）。

    ★ 这里有两个既存坑，2026-09-23 一起修掉：
      ① 原先写 `int(m.get("angle") or 90)` —— `or` 会把**合法的 0 当假值**改成 90，
         而 id2（左右转头）的行程就是 0~180，0 是合法值 ⇒ "转到最左"变成"转到中间"。
      ② 原先 `int("90度")` / `int(90.5)` 抛 ValueError，被 per-move 的 `except: continue`
         **静默吞掉整条 move** ⇒ 屏上既没动作也没提示。现在能解的解、解不出的存证。
    """
    if v is None or v == "":
        return default
    if isinstance(v, bool):
        return default
    try:
        return int(round(float(v)))      # "90" / "90.5" / 90.5 都接受
    except (TypeError, ValueError):
        return None


def _servo_limit_cfg():
    """管理员设备档案的软限位；保存后立即生效。"""
    out = {}
    for s in (device_profile.load().get("servos") or []):
        try:
            sid = int(s.get("id"))
        except (TypeError, ValueError):
            continue
        lo, hi = s.get("min"), s.get("max")
        try:
            out[sid] = (int(lo), int(hi), str(s.get("name") or s.get("role") or sid))
        except (TypeError, ValueError):
            continue
    return out


def _servo_clamp(sid, angle, clamped):
    """AI 给的舵机角度按 config 的软限位夹住，**并把夹取记下来**（不静默改）。

    ★ 为什么必须夹：`decision_to_raw` 原先不查 `hardware.servos`，F407 的 do_servo
      也只挡 0~180 ⇒ AI 说"低头"给个 30（id1 的软限位是 75~105）舵机会**真的往 30 转**，
      机械上可能顶死。⚠️ 只夹 **AI 决策**这条路 —— 网页遥控手打的帧一个字节都不动
      （测试页明说"测试期放开 0~180"）。
    """
    lim = _servo_limit_cfg().get(sid)
    if not lim:
        return angle
    lo, hi, name = lim
    if angle < lo or angle > hi:
        clamped.append("%s(id%d) %d→%d" % (name, sid, angle, hi if angle > hi else lo))
        return hi if angle > hi else lo
    return angle
# === [03网页端] END ===


def decision_to_raw(decision, clamped=None, device_id=None):
    """把 AI 决策翻译成 ESP 认识的 `@...#` 帧串（ESP 会拆成多行再发 F407）。

    统一由服务器生成 raw，完成限位、表情短文件名映射和时间序列校验；
    ESP 将文本帧逐行转发、将时间序列转换为一帧二进制。

    `clamped`：传一个 list 进来就会收到被软限位夹过的舵机（如 `['点头俯仰(id1) 30→75']`）。
    ★ 可选参数，老的 1 参调用**行为不变**。
    """
    frames = []
    profile = device_profile.load()
    default_speed = device_controls.get(device_id or DEFAULT_DEVICE_ID)["servo_speed"]
    servo_params = {s["id"]: s for s in profile["servos"]}
    clamped = [] if clamped is None else clamped
    timeline = decision.get("timeline")
    timed_moves = any(isinstance(m, dict) and "at_ms" in m for m in decision.get("moves") or [])
    if timeline is None and timed_moves:
        timeline = {"steps": [{"at_ms": m.get("at_ms", 0),
                               "move": {k: v for k, v in m.items() if k != "at_ms"}}
                              for m in decision.get("moves") or []]}
    if timeline is not None:
        try:
            frame, normalized = motion_timeline.compile_timeline(timeline, profile, default_speed)
            frames.append(frame)
        except ValueError as exc:
            decision["timeline_error"] = str(exc)
            print("[TIMELINE] refused: %s" % exc)
        # Invalid schedules never degrade into an immediate burst of moves.
        moves = []
    else:
        moves = decision.get("moves") or []
    for m in moves:
        if not isinstance(m, dict):
            continue
        try:
            t = str(m.get("type", "") or "").strip()
            if not t:
                continue
            if t == "servo":
                # [03网页端] 2026-09-23：`or 90` 会把合法的 0 变 90；浮点/带单位会抛异常被吞
                sid = _frame_int(m.get("id"), None)
                ang = _frame_int(m.get("angle"), None)
                if sid is None or ang is None:
                    print("[FRAME][WARN] servo 参数解不出来，跳过这条 move（原先会被静默吞掉）: %r"
                          % (m,))
                    continue
                ang = _servo_clamp(sid, ang, clamped)
                frame = "@MOVE:servo,%d,%d" % (sid, ang)
                if "speed" in m or "speed" in servo_params.get(sid, {}):
                    speed = m.get("speed", servo_params.get(sid, {}).get("speed", default_speed))
                    if isinstance(speed, bool) or not isinstance(speed, int) or not 0 <= speed <= 360:
                        continue
                    frame += ",%d" % speed
                frames.append(frame)
            elif t == "home":
                sid = _frame_int(m.get("id"), None) if "id" in m else None
                ids = set(servo_params)
                if "id" in m and sid not in ids:
                    continue
                if any("speed" in servo_params[x] for x in (ids if sid is None else [sid])):
                    frame, _ = motion_timeline.compile_timeline(
                        {"steps": [{"at_ms": 0, "move": m}]}, profile, default_speed)
                    frames.append(frame)
                    continue
                frame = "@MOVE:home,%s" % (sid if sid is not None else "all")
                if "speed" in m:
                    speed = m["speed"]
                    if isinstance(speed, bool) or not isinstance(speed, int) or not 0 <= speed <= 360:
                        continue
                    frame += ",%d" % speed
                frames.append(frame)
            elif t == "gesture":
                v = _clean_frame_text(m.get("name"))
                if v in ("nod", "hi", "look_left", "look_right", "shake"):
                    role = "pitch" if v in ("nod", "hi") else "yaw"
                    servo = next((s for s in profile["servos"] if s["role"] == role), {})
                    if "speed" in m or "speed" in servo:
                        frame, _ = motion_timeline.compile_timeline(
                            {"steps": [{"at_ms": 0, "move": m}]}, profile, default_speed)
                        frames.append(frame)
                    else:
                        frames.append("@MOVE:gesture,%s" % v)
                elif v:
                    print("[FRAME][WARN] 未登记的 F407 gesture，跳过: %s" % v)
            elif t == "expression":
                face_id = _clean_frame_text(m.get("face"))
                faces = {str(x.get("id", "")).lower(): str(x.get("file", "")).rsplit(".", 1)[0].lower()
                         for x in device_profile.load().get("expressions", [])}
                # UI/API 统一使用 eyesclose；FATFS 8.3 文件实际必须是 EYESCLOS.BMP。
                v = faces.get(face_id.lower()) if face_id else ""
                if v:
                    frames.append("@MOVE:expression,%s" % v)
                elif face_id:
                    print("[FRAME][WARN] 未登记的表情，跳过: %s" % face_id)
            else:
                frames.append("@MOVE:" + t)   # 新动作类型：类型名本身就是帧
        except Exception as e:
            print("[FRAME][WARN] 这条 move 畸形、跳过（不再静默）: %r (%s: %s)"
                  % (m, type(e).__name__, e))
            continue   # 单条畸形 move 不该拖垮整次决策
    # [03网页端] 2026-09-23：上屏文字走闸门（超长的整行会被 F407 静默丢掉）
    text, _cut = _screen_text(decision.get("text") or decision.get("message"))
    if text:
        frames.append("@TEXT:" + text)
    return "#".join(frames)


def auto_tick_once(cfg, tk):
    """一次定时触发（= 中断服务程序）：先查条件，再调 AI，最后决策入队。

    返回 (fired, reason)：fired=True 表示真的调了 AI 并入了队。
    """
    device_id = str(tk.get("device_id") or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    cfg = device_profile.apply_to_config(cfg, device_controls.get(device_id)["servo_speed"])
    ts = now_str()

    # ① 设备不在线就别自己说话 —— 否则命令会堆在队列里，等它重连后一次性全放出来
    if tk.get("require_online", True):
        age = device_age_sec(device_id)
        if age is None or age > DEVICE_ONLINE_SEC:
            return False, "skip:设备不在线"
    # ② 队列里还有没执行完的命令 → 让路（避免定时器把队列刷爆）
    try:
        max_q = int(tk.get("max_queue", 1) or 1)
    except (TypeError, ValueError):
        max_q = 1
    if _queue_len(device_id) >= max_q:
        return False, "skip:队列未清空"
    # ③ 静默期：用户刚说过话就别抢话（APIdata 里记的 _device_last_user）
    try:
        quiet = int(tk.get("quiet_sec", 60) or 0)
    except (TypeError, ValueError):
        quiet = 60
    if quiet > 0:
        with _cmd_lock:
            last_user = _device_last_user.get(device_id)
        if last_user and (datetime.now() - last_user).total_seconds() < quiet:
            return False, "skip:用户刚说过话"

    # ④ 组装上下文（和 /api/data 走同一条 AI 通路，行为完全一致）
    sensor = latest_sensor()
    mode = str(tk.get("mode") or "").strip().lower()
    if mode not in ("idle", "full"):
        mode = str((cfg.get("modes") or {}).get("default", "full"))
    prompt = str(tk.get("prompt") or DEFAULT_TICK_PROMPT)

    history, decisions = load_memory_context(cfg)
    content, meta = ai_relay.get_ai_decision(
        cfg, ai_relay.build_messages(cfg, history, decisions, sensor, prompt, mode=mode))
    decision = ai_relay.parse_decision(content, cfg.get("actions") or [])

    # ⑤ 写决策日志 —— 结构照抄 api_data，这样下一轮 load_memory_context 会把它
    #    当成一轮"AI 主动说过的话"记进多轮记忆
    log_entry = {
        "time": ts,
        "provider": meta.get("provider", ""),
        "model": meta.get("model", ""),
        "key": meta.get("key", ""),
        "sensor": sensor,
        "user": prompt,
        "mode": mode,
        "device_id": device_id,
        "tick": True,
    }
    for k in ("message", "text", "moves", "timeline", "action", "value"):
        if k in decision and decision[k] not in (None, ""):
            log_entry[k] = decision[k]
    log_entry.update({k: v for k, v in decision.items()
                      if k not in log_entry and k != "raw"})
    append_line(DECISION_LOG, json.dumps(log_entry, ensure_ascii=False))

    # 定时唤醒也是一轮"互动"（AI 主动开口），要进用户需求统计 ——
    # 但它不是用户说的话，所以用 is_auto_tick 标出来，前端好分开算。
    # 放在 raw 检查**之前**：AI 说了话但没给动作，那次开口也该被记上。
    try:
        store.record_interaction(
            ts, device_id, prompt,
            ai_message=str(decision.get("message", "")
                           or decision.get("text", "") or ""),
            mode=mode, provider=str(meta.get("provider", "") or ""),
            model=str(meta.get("model", "") or ""),
            is_auto_tick=True, moves=analytics.normalize_moves(decision))
    except Exception as e:
        print("[STORE][ERROR] 定时唤醒互动落库失败：%s: %s" % (type(e).__name__, e))

    raw = decision_to_raw(decision, device_id=device_id)
    if not raw:
        return False, "skip:AI 没给出可执行内容"

    cmd = {"time": ts, "device_id": device_id, "raw": raw, "note": "auto_tick"}
    qlen = enqueue_command(device_id, cmd)
    append_line(DATA_HISTORY, json.dumps({
        "time": ts, "sensor": sensor, "user": "", "mode": mode,
        "device_id": device_id, "event": "auto_tick", "command": cmd,
    }, ensure_ascii=False))
    try:
        store.record_command(ts, device_id, raw=raw, source="auto_tick",
                             note="auto_tick")
    except Exception as e:
        print("[STORE][ERROR] 定时唤醒命令落库失败：%s: %s" % (type(e).__name__, e))
    print("[%s] [TICK] 定时唤醒 → 入队 device=%s queue=%d raw=%s" % (
        ts, device_id, qlen, raw))
    return True, "ok:" + str(decision.get("message", ""))


def auto_tick_loop():
    """定时器本体（后台线程一直跑）。

    每 0.5s 醒一次看表，而不是 sleep(interval_sec) —— 这样网页改间隔能立刻生效、
    关掉开关马上停、进程退出（daemon）也不会卡住。
    """
    next_at = None
    armed = None
    while True:
        try:
            cfg = load_config()
            tk = cfg.get("auto_tick") or {}
            interval = max(5, int(tk.get("interval_sec", 60) or 60))
            if not tk.get("enabled", False):
                next_at, armed = None, None          # 关掉 → 解除武装
                time.sleep(1.0)
                continue
            now = time.time()
            if next_at is None or armed != interval:
                next_at, armed = now + interval, interval   # 首次 / 改了间隔 → 重新计时
            with _tick_lock:
                _tick_stat["next_at"] = next_at
            if now < next_at:
                time.sleep(0.5)
                continue
            next_at = now + interval                 # 先把表拨好，别让 AI 的耗时把周期拖歪
            fired, reason = auto_tick_once(cfg, tk)
            with _tick_lock:
                _tick_stat["count"] += 1
                _tick_stat["last_tick"] = now_str()
                _tick_stat["last_reason"] = reason
                if fired:
                    _tick_stat["last_message"] = reason[3:]
            if not fired:
                print("[TICK] %s" % reason)
        except Exception as e:
            with _tick_lock:
                _tick_stat["last_error"] = "%s: %s" % (type(e).__name__, e)
            print("[TICK][ERROR] %s" % e)
            time.sleep(5)   # 出错别空转刷屏（config.json 改坏了就是这个分支）


_presence_started = False
_presence_lock = threading.Lock()


def presence_loop():
    """60 秒没声音观望，120 秒休眠；声音/命令都会重新计时。"""
    while True:
        try:
            cfg = load_config()
            settings = cfg.get('presence') or {}
            if settings.get('enabled', True):
                idle = max(5, min(3600, int(settings.get('idle_seconds', 60))))
                sleep = max(idle + 5, min(7200, int(settings.get('sleep_seconds', 120))))
                now = datetime.now()
                for device, seen in list(_device_last_seen.items()):
                    if not seen or (now - seen).total_seconds() > DEVICE_ONLINE_SEC:
                        continue
                    busy = (any(camera_blockers(device).values()) or
                            sampled_player.busy(device) or _queue_len(device) > 0)
                    kind = presence.TRACKER.tick(device, idle, sleep, busy)
                    if kind:
                        _presence_enqueue(device, kind)
        except Exception as exc:
            print('[PRESENCE] loop error: %s' % exc)
        time.sleep(1)


def start_presence():
    global _presence_started
    with _presence_lock:
        if _presence_started:
            return False
        _presence_started = True
    threading.Thread(target=presence_loop, name='presence', daemon=True).start()
    print('[PRESENCE] 上位机待机线程已启动：60s 观望 / 120s 休眠 / 声音回正')
    return True


def start_auto_tick():
    """启动定时线程，只启动一次。返回 True 表示这次真的起了线程。"""
    global _auto_tick_started
    with _tick_lock:
        if _auto_tick_started:
            return False
        _auto_tick_started = True
    t = threading.Thread(target=auto_tick_loop, name="auto-tick", daemon=True)
    t.start()
    print("[TICK] 定时唤醒线程已启动（间隔/开关见 config.json 的 auto_tick）")
    start_presence()
    return True


def start_store():
    """建表 + 从两个 .txt 回填一次。只跑一次，返回 True 表示这次真的跑了。

    ⚠️ 回填失败**不能**让服务器起不来：统计页没数据只是少个功能，
      而整个中转站挂了就是 ESP 上报也收不到了。所以这里吞掉异常只打日志。
    """
    global _store_started
    with _tick_lock:
        if _store_started:
            return False
        _store_started = True
    try:
        store.init_db()
        store.backfill()
    except Exception as e:
        print("[STORE][ERROR] 建表/回填失败：%s: %s" % (type(e).__name__, e))
        print("[STORE] 统计页可能没数据；原始 .txt 记录不受影响，上报链路正常")
    return True


# ------------------------------------------------- ESP 服务器 IP 自动发现（UDP 应答）
# ★ 2026-09-20 新增。ESP 侧 sketch_aug28a.ino::discoverServer() 会往 UDP BEACON_PORT
#   广播 BEACON_MAGIC，这里收到就回一句 "ARDP_IP:<本机在 ESP 那侧的 IP>:<HTTP端口>"。
#
# 为什么要有这个：手机热点是 DHCP，PC 每次重连 IP 都可能变（本项目已换过 5 次）。
# 旧做法是把这个 IP 写死在 ESP 的 SERVER_IPS[0] 里，一旦对不上，ESP 会**静默回落到
# 公网** —— 网页在本机 Flask 入队、ESP 却跑去云端取令，板子永远收不到，**两端都不报错**，
# 是全项目最难查的一类故障。有了应答线程，换网再也不用改 IP、也不用重烧 ESP。
#
# ⚠️ 端口/口令必须与 sketch_aug28a.ino 的 DISCOVER_PORT / DISCOVER_MAGIC 一致；
#    改一处不改另一处 = 两边都收不到、而且同样不报错。
BEACON_PORT = 5001
BEACON_MAGIC = b"ARDP_DISCOVER"

_beacon_started = False
_beacon_lock = threading.Lock()


def _local_ip_for(peer_ip):
    """按真实接口掩码选ESP所在网段；默认路由漂到有线网时也不报错网卡。"""
    return lan_discovery.local_ip_for(peer_ip)


def beacon_loop():
    """UDP 应答线程：ESP 来问路就告诉它本机当前 IP。出错只打日志，不拖垮服务器。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.bind(("0.0.0.0", BEACON_PORT))
    except OSError as e:
        # ⚠️ 最常见的原因：已经有一个 app.py 在跑（旧的 Flask 没关干净 —— 本项目
        #    验收脚本复用旧进程是常事）。Windows 上 SO_REUSEADDR 允许重复 bind，
        #    但收包只给后绑的那个 ⇒ 表现是"问路时灵时不灵"。看到这行先查 5000 端口。
        print("[BEACON][ERROR] UDP %d 绑定失败：%s" % (BEACON_PORT, e))
        print("[BEACON] 多半是已经有一个 app.py 在跑 ⇒ ESP 问路会时灵时不灵")
        sock.close()
        return

    print("[BEACON] 服务器 IP 自动发现已就绪（UDP %d，等 ESP 来问路）" % BEACON_PORT)

    while True:
        try:
            data, addr = sock.recvfrom(256)
        except OSError as e:
            print("[BEACON][ERROR] 收包失败：%s" % e)
            time.sleep(1)
            continue
        if data.strip() != BEACON_MAGIC:
            continue
        ip = _local_ip_for(addr[0])
        if not ip:
            print("[BEACON] %s 来问路，但没能确定本机 IP，跳过" % addr[0])
            continue
        # 每次现读 config，端口改了不用重启线程
        port = int(load_config().get("port", 5000))
        try:
            lan_discovery.send_reply(sock, ("ARDP_IP:%s:%d" % (ip, port)).encode("ascii"), addr, ip)
            print("[BEACON] %s 来问路 → 告诉他 %s:%d" % (addr[0], ip, port))
        except OSError as e:
            print("[BEACON][ERROR] 应答失败：%s" % e)


def start_beacon():
    """起自动发现线程，只起一次。返回 True 表示这次真的起了线程。"""
    global _beacon_started
    with _beacon_lock:
        if _beacon_started:
            return False
        _beacon_started = True
    threading.Thread(target=beacon_loop, name="ip-beacon", daemon=True).start()
    return True


@app.before_request
def ensure_background():
    """任何请求进来都确保后台的一次性初始化已做完。

    这样 `python app.py` / `flask run` / waitress 三种起法都能工作。
    ⚠️ 多进程部署（gunicorn -w N）时每个 worker 各有一个线程，会重复调 AI ——
    本项目线上是 `nohup python app.py` 单进程，没这个问题。
    """
    start_auto_tick()
    start_store()
    # ⚠️ 自动发现线程在 `python app.py` 时由 __main__ 抢先起（那是常规起法）；
    #    这里是给 `flask run` / waitress 兜底 —— 但那条路要等第一个请求才生效，
    #    而 ESP 得先知道 IP 才发得出请求 ⇒ 兜底起法下多等一次刷新即可。
    start_beacon()
    # ★ [01语音] 顺手把识别模型预热一下（asr.warmup 是幂等的，只会真起一次线程）。
    #   放在这里是因为三种起法（python app.py / flask run / waitress）都会走到它。
    try:
        if os.environ.get('RELAY_SKIP_ASR_WARMUP') != '1':
            asr.warmup()
    except Exception as e:
        print("[ASR][WARN] 预热没起来（%s）—— 不影响其余功能" % type(e).__name__)
    return None


@app.route("/api/auto_tick", methods=["GET", "POST"])
def api_auto_tick():
    """定时唤醒的查看 / 开关：GET 看状态，POST 改 enabled / interval_sec / quiet_sec / mode。

    改的是 config.json，跑着的线程下一轮就读到，**不用重启服务器**。
    """
    cfg = load_config()
    tk = cfg.setdefault("auto_tick", {})
    if request.method == "POST":
        body = request.get_json(silent=True) or {}
        changed = {}
        if "enabled" in body:
            tk["enabled"] = bool(body.get("enabled"))
            changed["enabled"] = tk["enabled"]
        for k in ("interval_sec", "quiet_sec", "max_queue"):
            if k in body:
                try:
                    v = int(body.get(k))
                except (TypeError, ValueError):
                    return jsonify({"error": "%s 必须是整数" % k}), 400
                if k == "interval_sec":
                    v = max(5, v)          # 下限 5s，再快就是拿钱换串口噪声了
                if v < 0:
                    return jsonify({"error": "%s 不能为负" % k}), 400
                tk[k] = v
                changed[k] = v
        if "mode" in body:
            m = str(body.get("mode") or "").strip().lower()
            if m not in ("", "idle", "full"):
                return jsonify({"error": "mode 只能是 idle / full / 空（空=跟默认模式）"}), 400
            tk["mode"] = m
            changed["mode"] = m
        cfg["auto_tick"] = tk
        save_config(cfg)
        print("[TICK] 配置已更新：%s" % json.dumps(changed, ensure_ascii=False))

    with _tick_lock:
        stat = dict(_tick_stat)
    nxt = stat.pop("next_at", None)
    return jsonify({
        "running": bool(_auto_tick_started),
        "config": tk,
        "count": stat["count"],
        "last_tick": stat["last_tick"],
        "last_reason": stat["last_reason"],
        "last_message": stat["last_message"],
        "last_error": stat["last_error"],
        "next_tick": (datetime.fromtimestamp(nxt).strftime("%Y-%m-%d %H:%M:%S")
                      if nxt else ""),
        "server_time": now_str(),
    })


# ===================== 统计（分页各页面用） =====================
# 全部走 /api/ 前缀，所以自动受 require_auth 保护，不用单独写鉴权。
#
# ★ 这些接口的数据来源是 relay.db（见 store.py），**不是** data_history.txt。
#   原始 .txt 里绝大多数行是 sensor:"无数据"，统计只看 valid=1 的行。
#   两边数据不一致时以 .txt 为准（它是主链路），库里缺了跑一次 store.backfill() 补。

_TH_FIELDS = ("temp", "humi", "distance", "presence")
_TH_OPS = (">", ">=", "<", "<=", "==", "!=")
_TH_SEV = ("info", "warn", "error")


def _hours_arg(default=24):
    """hours 查询参数：夹在 1 小时 ~ 1 年之间，防止一句 ?hours=99999999 拖垮查询。"""
    try:
        h = int(request.args.get("hours", default))
    except (TypeError, ValueError):
        h = default
    return max(1, min(24 * 365, h))


def _limit_arg(default=50):
    """列表类接口返回多少条明细。上限 500，防止一页把整库拉出来。"""
    try:
        n = int(request.args.get("limit", default))
    except (TypeError, ValueError):
        n = default
    return max(1, min(500, n))


@app.route("/api/stats/summary")
def api_stats_summary():
    """总览页的数字卡片。

    设备在线数不在库里（那是进程内存的 _device_last_seen），所以在这里补上。
    """
    try:
        s = store.summary()
    except Exception as e:
        return jsonify({"error": "统计不可用：%s" % e}), 500
    now = datetime.now()
    with _cmd_lock:
        ids = set(_device_last_seen) | set(_pending_commands) | set(_device_last_ack)
        online = sum(1 for did in ids
                     if _device_last_seen.get(did)
                     and (now - _device_last_seen[did]).total_seconds() <= DEVICE_ONLINE_SEC)
    s.update({"devices_online": online, "devices_total": len(ids),
              "online_sec": DEVICE_ONLINE_SEC, "server_time": now_str()})
    return jsonify(s)


@app.route("/api/stats/sensor")
def api_stats_sensor():
    """温湿度序列 + 区间统计（环境页）。"""
    try:
        return jsonify(store.sensor_series(hours=_hours_arg(), limit=400))
    except Exception as e:
        return jsonify({"error": "统计不可用：%s" % e}), 500


@app.route("/api/stats/presence")
def api_stats_presence():
    """人员在离区间（人员页）。"""
    try:
        return jsonify(store.presence_spans(hours=_hours_arg(24)))
    except Exception as e:
        return jsonify({"error": "统计不可用：%s" % e}), 500


@app.route("/api/stats/anomaly")
def api_stats_anomaly():
    """异常统计（异常页）。可按 code / severity 过滤。"""
    try:
        return jsonify(store.anomaly_stats(
            hours=_hours_arg(24),
            code=(request.args.get("code") or "").strip() or None,
            severity=(request.args.get("severity") or "").strip() or None,
            recent_limit=_limit_arg(50)))
    except Exception as e:
        return jsonify({"error": "统计不可用：%s" % e}), 500


@app.route("/api/stats/interaction")
def api_stats_interaction():
    """用户需求统计（互动页）。"""
    try:
        return jsonify(store.interaction_stats(hours=_hours_arg(24),
                                               recent_limit=_limit_arg(50)))
    except Exception as e:
        return jsonify({"error": "统计不可用：%s" % e}), 500


@app.route("/api/thresholds", methods=["GET", "POST"])
def api_thresholds():
    """读写异常判定规则（存在 config.json 的 thresholds 段，改完不用重启）。

    POST 只认下面这三个键，别的键一律忽略 —— 和 /api/settings 一样，
    网页只能改"策略"，改不了硬件接口和决策帧格式。
    """
    cfg = load_config()
    if request.method == "POST":
        body = request.get_json(silent=True) or {}
        th = cfg.setdefault("thresholds", {})
        if "enabled" in body:
            th["enabled"] = bool(body.get("enabled"))
        if "cooldown_sec" in body:
            try:
                v = int(body.get("cooldown_sec"))
            except (TypeError, ValueError):
                return jsonify({"error": "cooldown_sec 必须是整数"}), 400
            if v < 0:
                return jsonify({"error": "cooldown_sec 不能为负"}), 400
            th["cooldown_sec"] = v
        if "rules" in body:
            rules = body.get("rules")
            if not isinstance(rules, list):
                return jsonify({"error": "rules 必须是数组"}), 400
            if len(rules) > 50:
                return jsonify({"error": "规则最多 50 条"}), 400
            clean = []
            for r in rules:
                if not isinstance(r, dict):
                    return jsonify({"error": "每条规则必须是对象"}), 400
                code = str(r.get("code", "") or "").strip()
                field = str(r.get("field", "") or "").strip()
                op = str(r.get("op", "") or "").strip()
                if not code:
                    return jsonify({"error": "规则的 code 不能为空"}), 400
                if field not in _TH_FIELDS:
                    return jsonify({"error": "规则 %s 的 field 必须是 %s"
                                    % (code, " / ".join(_TH_FIELDS))}), 400
                if op not in _TH_OPS:
                    return jsonify({"error": "规则 %s 的 op 必须是 %s"
                                    % (code, " ".join(_TH_OPS))}), 400
                try:
                    value = float(r.get("value"))
                except (TypeError, ValueError):
                    return jsonify({"error": "规则 %s 的 value 必须是数字" % code}), 400
                sev = str(r.get("severity", "info") or "info")
                if sev not in _TH_SEV:
                    sev = "info"
                clean.append({
                    "code": code,
                    "label": str(r.get("label", "") or code),
                    "field": field, "op": op, "value": value,
                    "severity": sev,
                    "on_change": bool(r.get("on_change")),
                })
            th["rules"] = clean
        cfg["thresholds"] = th
        save_config(cfg)
        print("[TH] 阈值已更新：enabled=%s cooldown=%s 规则=%d 条" % (
            th.get("enabled"), th.get("cooldown_sec"), len(th.get("rules") or [])))
    return jsonify({
        "thresholds": cfg.get("thresholds") or {},
        "fields": list(_TH_FIELDS), "ops": list(_TH_OPS),
        "severities": list(_TH_SEV),
        "field_labels": {"temp": "温度(°C)", "humi": "湿度(%)",
                         "distance": "距离(cm)", "presence": "人员(1有/0无)"},
    })


# ===================== 主接口 =====================

# ===================== 调试模式（开发伙伴对话）=====================
# ★ 本段与 /api/data 那条**故意完全隔离**，别再合并：
#   · 记忆走 debug_chat_log.txt，**一个字都不碰 decision_log.txt**
#   · 不写 relay.db、不进互动统计、不入命令队列、不下发任何帧
#   · 必须关掉 json_object，模型才能正常写文字（见 ai_relay._call_chat）
#   · 鉴权不用另加：/api/* 已被上面的 require_auth 兜住

def _read_debug_turns(limit=DEBUG_HISTORY_LINES):
    """读调试会话历史（旧→新）。文件不存在 = 空会话（read_tail 已兜住）。"""
    turns = []
    for line in read_tail(DEBUG_LOG, limit):
        try:
            r = json.loads(line)
        except Exception:
            continue
        if isinstance(r, dict) and r.get("user"):
            turns.append(r)
    return turns


@app.route("/api/debug_chat/history")
def api_debug_chat_history():
    """回放当前调试会话（前端刷新用）。"""
    dc = load_config().get("debug_chat") or {}
    turns = _read_debug_turns()
    return jsonify({
        "enabled": bool(dc.get("enabled", True)),
        "turns": [{"time": t.get("time", ""),
                   "user": t.get("user", ""),
                   "reply": t.get("reply", ""),
                   "provider": t.get("provider", "")} for t in turns],
        "total": len(turns),
    })


@app.route("/api/debug_chat", methods=["POST"])
def api_debug_chat():
    """调试模式对话：纯文字，不返回决策帧、不入队、不下发。"""
    cfg = load_config()
    dc = cfg.get("debug_chat") or {}
    if not dc.get("enabled", True):
        return jsonify({"error": "调试模式已在 config.json 里关掉了"
                                 "（debug_chat.enabled = false）"}), 400
    body = request.get_json(silent=True) or {}
    msg = str(body.get("message", "") or "").strip()
    if not msg:
        return jsonify({"error": "message 不能为空"}), 400
    if len(msg) > 4000:
        return jsonify({"error": "message 太长（最多 4000 字）"}), 400

    ts = now_str()
    try:
        content, meta = ai_relay.get_ai_decision(
            cfg,
            ai_relay.build_debug_messages(cfg, _read_debug_turns(), msg),
            json_mode=False,          # ★ 必须传 False，理由见 _call_chat 的注释
            max_tokens=ai_relay.debug_max_tokens(cfg),
        )                             # ★ 大额度：512 会把长回答截断在半句话上
        reply = ai_relay.parse_debug_reply(content)
    except Exception as e:
        print("[DEBUG_CHAT][ERROR] %s" % e)
        return jsonify({"error": "调用 AI 失败：%s" % e}), 502

    append_line(DEBUG_LOG, json.dumps({
        "time": ts, "user": msg, "reply": reply,
        "provider": meta.get("provider", ""), "model": meta.get("model", ""),
    }, ensure_ascii=False))
    print("[%s] [调试] provider=%s 问: %s" % (
        ts, meta.get("provider", "?"), msg[:40].replace("\n", " ")))
    return jsonify({"reply": reply, "time": ts,
                    "provider": meta.get("provider", ""),
                    "model": meta.get("model", "")})


@app.route("/api/debug_chat/new", methods=["POST"])
def api_debug_chat_new():
    """「新对话」：把当前调试会话**归档**（不删），下次提问自动重建空文件。

    归档而不是删 —— 用户说了以后要做长期记忆，这些归档是将来的原料，
    而且在 _debug_archive\\ 里随时能翻。
    """
    archived = None
    if os.path.exists(DEBUG_LOG) and os.path.getsize(DEBUG_LOG) > 0:
        try:
            os.makedirs(DEBUG_ARCHIVE_DIR, exist_ok=True)
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            archived = os.path.join(DEBUG_ARCHIVE_DIR, "debug_%s.txt" % stamp)
            n = 1
            while os.path.exists(archived):     # 同一秒连点两次也不互相覆盖
                n += 1
                archived = os.path.join(DEBUG_ARCHIVE_DIR,
                                        "debug_%s_%d.txt" % (stamp, n))
            shutil.move(DEBUG_LOG, archived)
        except Exception as e:
            print("[DEBUG_CHAT][ERROR] 归档失败：%s" % e)
            return jsonify({"error": "归档失败：%s" % e}), 500
        print("[%s] [调试] 新对话，已归档 -> %s"
              % (now_str(), os.path.basename(archived)))
    return jsonify({"ok": True,
                    "archived": os.path.basename(archived) if archived else ""})


@app.route("/api/memory/new", methods=["POST"])
def api_memory_new():
    """「新对话（机器人）」：给 decision_log.txt 追加一条"记忆起点"标记。

    ★ 为什么是标记而不是删文件：
      删了会连统计一起丢（relay.db 的回填游标 store.py 是按行数走的），
      而且不可逆。标记法只影响"AI 记得什么"，日志与统计一条不少。
      生效点在 load_memory_context()。
    """
    ts = now_str()
    append_line(DECISION_LOG, json.dumps({
        "time": ts, "event": MEMORY_RESET_EVENT,
        "note": "网页点了「新对话」",
    }, ensure_ascii=False))
    print("[%s] [记忆] 已打新对话标记：AI 从此不再记得之前的话（日志不删）" % ts)
    return jsonify({"ok": True, "time": ts})


@app.route("/api/data", methods=["POST"])
def api_data():
    # 合入目标设备默认速度与管理员档案，不使用固定90。
    cfg = load_config()
    body = request.get_json(silent=True) or {}
    sensor = str(body.get("sensor", "")).strip()
    user = str(body.get("user", "")).strip()
    ack = str(body.get("ack", "") or "").strip()
    device_id = str(body.get("device_id", "") or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    if user:
        presence.TRACKER.note(device_id)
    cfg = device_profile.apply_to_config(cfg, device_controls.get(device_id)["servo_speed"])
    # PC 端语音识别的回灌也复用 /api/data，但它不是 ESP 的一次轮询。
    # 这类请求只能把 AI 决策入队，绝不能领取/消费已有网页命令，更不能把
    # 设备在线时间伪造成刚刚由 ESP 上报。
    enqueue_decision = body.get("enqueue_decision") is True
    # === [03网页端] BEGIN === 2026-09-23：网页发起的请求都不是"ESP 的一次轮询"
    # `deliver`    ：网页对话/互动（互动页打字）—— AI 决策要**真的下发到设备**，见下面 ③‴
    # `no_pop`     ：测试页两个"模拟上报"入口 —— 只要决策，**不领命令**（下同）
    # ★ 为什么三个标志都要挡 `pop_command`：浏览器把命令 `pop` 走之后，那条命令就
    #   **永久消失**了（ESP 若超时/JSON 解析失败不会重发，见 问题日志_AI速查.md 抢单条）
    #   ⇒ 症状是"点了遥控按钮、再打字聊天 → 遥控那一下静默没发生"。
    deliver = body.get("deliver") is True
    no_pop = body.get("no_pop") is True
    from_web = deliver or enqueue_decision or no_pop
    # 误触发可产生几十段录音。设备录音/上传时暂停取任务，语音回灌不能无限
    # 调AI再塞队列；先拒绝满队列，ASR历史保留为auto_failed，供用户重发。
    if enqueue_decision and _queue_len(device_id) >= DELIVER_MAX_QUEUE:
        err = "回复队列已满，请等设备处理后在录音历史重新发送"
        _note_delivery(device_id, now_str(), user, "", "refused_queue_full", err)
        return jsonify({"ok": False, "queued_to_device": False,
                        "deliver_state": "refused_queue_full", "err": err}), 429
    # === [03网页端] END ===
    if not from_web:
        device_controls.confirm(device_id, body.get("control_ack"))
        idle_pet.confirm(device_id, body.get("idle_ack"))
        board_pose.ingest(device_id, body.get("board_pose"))
        with _cmd_lock:
            status = _motion_status.setdefault(device_id, {})
            motion_delivery.report(device_id, body.get("motion_delivery"))
            value = body.get("motion_state")
            if isinstance(value, str) and len(value) < 100:
                status["state"] = value
                status["at"] = now_str()
            text_report = body.get("text_transport")
            if isinstance(text_report, dict):
                status["text_transport"] = {
                    "phase": str(text_report.get("phase", ""))[:24],
                    "sequence": text_report.get("sequence"),
                    "attempts": text_report.get("attempts"), "at": now_str()}
            status["limits_applied"] = device_profile.motion_snapshot(body.get("motion_ack"))["applied"]
            if isinstance(body.get("motion_cache"), bool):
                status["profile_cached"] = body["motion_cache"]
            if ack.startswith("home,"):
                status["home"] = ack[5:]
        audio_report = body.get("audio_transport")
        if isinstance(audio_report, dict):
            report = {}
            for key in ["accepted", "stored", "sent", "total", "uart_overruns", "retries"]:
                value = audio_report.get(key)
                if isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= 0xFFFFFFFF:
                    report[key] = value
            for key in ["version", "phase", "error"]:
                if isinstance(audio_report.get(key), str):
                    report[key] = audio_report[key][:100]
            report["at"] = now_str()
            with _io_lock:
                _audio_transport_status[device_id] = report
    mock = bool(body.get("mock", False))   # 仪表盘测试模式：跳过AI，返回模拟决策
    need_ai = bool(body.get("need_ai", False))  # 显式要求AI决策（如F407异常上报）
    # ESP 运行模式：idle=待机(仅异常调AI) / full=全功率(每次用户输入都决策)
    mode = str(body.get("mode", "") or "").strip().lower()
    if mode not in ("idle", "full"):
        mode = str((cfg.get("modes") or {}).get("default", "full"))
    ts = now_str()

    if not from_web:
        touch_device(device_id, ack=ack or None)
        # 分步动作能力只看网关自己的轮询：有这个对象才算支持本地计时播放。
        _gateway_step[device_id] = dict(
            seen=time.monotonic(),
            capable=isinstance(body.get('step_playback'), dict))
        # === [03网页端] BEGIN === 2026-09-23 远程OTA页：记设备来源 IP
        # ★ 只在"非浏览器请求"这一支里记 —— 用户 PC 上打开的网页也是从
        #   192.168.x.x/127.0.0.1 打到 Flask 的，记进来会把映射写成
        #   "设备的 IP = 电脑的 IP"，远程OTA页就会往电脑自己身上发固件。
        _dev_ip = (request.remote_addr or "").strip()
        if _dev_ip:
            with _cmd_lock:
                _device_ip[device_id] = _dev_ip
        # === [03网页端] END ===
    # 记下"用户最后说话的时刻" —— 定时唤醒的静默期（quiet_sec）靠它判断该不该抢话
    if user:
        with _cmd_lock:
            _device_last_user[device_id] = datetime.now()

    # ② 追加到 data_history.txt（含 device_id / ack，供远程测试台日志）
    hist = {"time": ts, "sensor": sensor, "user": user, "mode": mode,
            "device_id": device_id}
    if ack:
        hist["ack"] = ack
    append_line(DATA_HISTORY, json.dumps(hist, ensure_ascii=False))
    print("[%s] 收到数据 device=%s mode=%s sensor=%s user=%s ack=%s%s" % (
        ts, device_id, mode, sensor, user, ack or "-",
        " [测试模式]" if mock else ""))

    # ③ 是否要AI决策：只有“用户主动请求”(user非空) 或 显式 need_ai=True
    #    （如F407异常上报/控制页指令）才调AI；
    #    周期上报的空数据只记录不调AI —— 省AI费用、不影响硬件效率。
    run_ai = (not mock) and (need_ai or len(user) > 0)

    # ③′ 结构化落库 + 异常判定（SQLite，只服务统计页；上面的 .txt 才是主链路）
    #    ★ 整段包 try：这里失败绝不能连累上报 —— ESP 拿不到响应会重试堆积，
    #      而"统计页少几条数据"只是少个功能。两个严重性不是一个量级。
    fired_alerts = []
    try:
        parsed = analytics.parse_sensor(sensor) if sensor else None
        if parsed is not None:
            store.record_sensor(ts, device_id, sensor, parsed)

        # 设备自己报的异常：将来 F407 发 @ALERT 时走这个字段；网页测试页也用它注入
        alert_raw = body.get("alert")
        if alert_raw:
            a = alert_raw if isinstance(alert_raw, dict) else {"message": alert_raw}
            code = str(a.get("code", "") or "DEVICE_ALERT")
            sev = str(a.get("severity", "") or "warn")
            msg = str(a.get("message", "") or a.get("label", "") or code)
            store.record_event(ts, device_id, "device_alert", code, sev, msg,
                               source="device", need_ai=need_ai, ai_called=run_ai)
            fired_alerts.append({"code": code, "label": msg, "severity": sev})

        # 阈值判定：按 config.json 的 thresholds.rules 判，**服务端判、F407 不用自己判**
        if parsed is not None:
            hits = analytics.evaluate_thresholds(
                parsed, cfg.get("thresholds") or {}, _threshold_last_fired,
                int(time.time()), prev=_prev_reading.get(device_id))
            _prev_reading[device_id] = parsed
            for h in hits:
                msg = analytics.format_alert_message(h, parsed)
                store.record_event(ts, device_id, "sensor_alert", h["code"],
                                   h["severity"], msg, source="threshold")
                fired_alerts.append({"code": h["code"], "label": h["label"],
                                     "severity": h["severity"], "message": msg})

        # ESP 回执挂到对应的那条命令上
        if ack:
            store.attach_ack(device_id, ack, ts)
    except Exception as e:
        print("[STORE][ERROR] 落库/异常判定失败（不影响上报）：%s: %s"
              % (type(e).__name__, e))

    decision = {"message": "中转站内部错误", "moves": [], "raw": ""}
    meta = {}
    skip_decision_log = False
    relay_failed = False
    try:
        if mock:
            # 测试模式：不调用AI，返回新信封格式的模拟决策，让界面先跑通
            decision = {
                "message": "[测试模式] 看到你啦，你好呀",
                "text": "你好呀",
                "moves": [
                    {"type": "servo", "id": 1, "angle": 100},
                    {"type": "servo", "id": 2, "angle": 90},
                    {"type": "gesture", "name": "nod"},
                    {"type": "expression", "face": "happy"},
                ],
                "raw": "",
            }
            meta = {"provider": "mock", "model": "", "key": ""}
        elif not run_ai:
            # 仅记录数据，不调用AI —— 但仍要走末尾 pop command，不能提前 return
            decision = {"action": "none", "value": 0,
                        "message": "已记录（未调用AI）", "ai": False,
                        "moves": []}
            skip_decision_log = True
        else:
            # ④ 读最近N条记录当记忆上下文
            history, decisions = load_memory_context(cfg)
            # ⑤ 调用AI（带上当前运行模式，让决策符合待机/全功率语境）
            content, meta = ai_relay.get_ai_decision(
                cfg, ai_relay.build_messages(
                    cfg, history, decisions, sensor, user, mode=mode))
            # ⑥ 规范化成统一JSON
            decision = ai_relay.parse_decision(content, cfg.get("actions") or [])
            if decision.get("message") == "AI响应无法解析":
                decision["ai_status"] = "parse_error"
                decision["ai_error"] = "模型没有返回可识别的 JSON 决策"
            else:
                decision["ai_status"] = "ok"
    except Exception as e:
        print("[ERROR] %s" % e)
        decision = {"message": "中转站出错: %s" % e, "moves": [], "raw": ""}
        relay_failed = True
        decision["ai_status"] = "relay_error"
        decision["ai_error"] = "%s: %s" % (type(e).__name__, e)

    # === [03网页端] BEGIN === 2026-09-23
    # ① 响应带上 `time`：前端刚 POST 完要先把气泡画出来（本地回显），过 5 秒
    #    `/api/history` 又会把同一轮送回来 —— 两边靠 `time`+`user`+`message` 认重，
    #    没有 `time` 就认不出，用户会看到自己每句话都被画两遍。
    #    `decision_log` 那条 `"time"` 是 log_entry 自己写的，这里不会覆盖它。
    # ② 给"语音轮"打个来源标记（**精确，不靠猜**）：PC 音频回灌
    #    （`audio_rx.inject()`）是**唯一**会带 `enqueue_decision` 的调用方
    #    （与 01语音 的契约见 合作表 §4.6）⇒ 拿它当判据是精确的。
    #    写进 `decision` 会被下面 log_entry 的"其余字段"那段带进 decision_log.txt
    #    ⇒ 互动页读 /api/history 就能把这一轮标成 🎤。
    #    ❌ 别改成"user 非空就是语音" —— 网页打字也是 user 非空，那样标反。
    decision["time"] = ts
    if enqueue_decision:
        decision["voice"] = True
    # === [03网页端] END ===

    if not skip_decision_log:
        # ⑤ 存到 decision_log.txt（新决策：message+moves[]+可选text；旧决策仍带 action/value）
        log_entry = {
            "time": ts,
            "provider": meta.get("provider", ""),
            "model": meta.get("model", ""),
            "key": meta.get("key", ""),
            "sensor": sensor,
            "user": user,
            "mode": mode,
            "device_id": device_id,
        }
        for k in ("message", "text", "moves", "timeline", "action", "value"):
            if k in decision and decision[k] not in (None, ""):
                log_entry[k] = decision[k]
        log_entry.update({k: v for k, v in decision.items()
                          if k not in log_entry and k != "raw"})
        append_line(DECISION_LOG, json.dumps(log_entry, ensure_ascii=False))
        n_moves = len(decision.get("moves") or [])
        print("[%s] 决策 provider=%s moves=%d message=%s" % (
            ts, meta.get("provider", "?"), n_moves, decision.get("message", "")))

    # ③″ 互动记录 —— "用户需求统计"的数据来源。
    #     只在"确实有一轮用户↔AI 往来"时记（user 非空 + 不是纯记录）。
    #     空数据的周期上报不算互动，否则统计页的数字全是噪声。
    if user and not skip_decision_log:
        try:
            store.record_interaction(
                ts, device_id, user,
                ai_message=str(decision.get("message", "")
                               or decision.get("text", "") or ""),
                mode=mode, provider=str(meta.get("provider", "") or ""),
                model=str(meta.get("model", "") or ""),
                is_mock=mock, moves=analytics.normalize_moves(decision))
        except Exception as e:
            print("[STORE][ERROR] 互动落库失败（不影响回复）：%s: %s"
                  % (type(e).__name__, e))

    # === [03网页端] BEGIN === 2026-09-23：上屏文字闸门 + 网页对话/互动正式下发
    # ③‴ 把随响应返回的 `text` 换成"屏上真能显示完"的版本。
    #   ★ 位置很关键 —— 必须排在 append_line(DECISION_LOG) 与 record_interaction **之后**：
    #     `message` 是决策协议 v2 的必填字段，也是**记忆源**（decision_log → load_memory_context）
    #     与互动统计的 ai_message，截断它 = AI 以为自己在说短句、统计页跟着失真。
    #     所以 **message 保持全文，只换 text**（按协议 text 就是"屏幕那一行"）。
    #   ★ 顺带兜住 ESP 自己那条路：ESP 轮询时读的正是响应里的 `text`（sketch:666），
    #     那条路不经过云端、但**这个 text 是云端生成的** ⇒ 在这里换掉就一并护住了，固件零改动。
    if decision.get("text") or decision.get("message"):
        _st, _cut = _screen_text(decision.get("text") or decision.get("message"))
        if _st:
            decision["text"] = _st
            if _cut:
                decision["screen_truncated"] = True
    # === [03网页端] END ===

    # PC 音频识别回灌：AI 的动作必须留给下一次 ESP 轮询领取。这里不能走
    # pop_command，否则 PC 会把原有网页命令或刚生成的音频命令提前消费掉。
    if enqueue_decision:
        if relay_failed:
            return jsonify({"ok": False, "err": "AI 决策失败"}), 502
        raw = decision_to_raw(decision, device_id=device_id)
        if not raw:
            return jsonify({"ok": False, "err": "AI 未产生可下发命令"}), 502
        # === [03网页端] BEGIN === 2026-09-23：语音轮也记进投递账本
        #   互动页要能说出"这一轮语音到底下发没下发、设备取走没有"。这里**只加一本
        #   账、不动响应契约**（`queued_to_device` 等字段原样，见 合作表 §4.6）。
        #   ❌ 别把这段搬到 return 之后 —— 那样永远不执行。
        did = _note_delivery(device_id, ts, user, raw, "queued")
        decision["deliver_id"] = did
        decision["deliver_state"] = "queued"
        # === [03网页端] END ===
        queue_len = enqueue_reply(device_id, {
            "time": ts,
            "device_id": device_id,
            "raw": raw,
            "note": "audio_recognition",
            "_did": did,
        })
        if queue_len is None:
            # AI调用期间别的请求也可能入队；锁内再核验，不靠前置检查防并发。
            with _deliver_lock:
                for rec in _deliveries:
                    if rec["id"] == did:
                        rec.update(state="refused_queue_full", err="回复队列已满，录音历史可重发")
            return jsonify({"ok": False, "queued_to_device": False,
                            "deliver_state": "refused_queue_full", "err": "回复队列已满，录音历史可重发"}), 429
        decision["queued_to_device"] = True
        decision["queue_len"] = queue_len
        decision["device_id"] = device_id
        return jsonify(decision)

    # === [03网页端] BEGIN === 2026-09-23：网页对话/互动正式下发到设备
    # 用户 2026-09-23：「所有的网页端的对话、互动都要正式发送到屏幕上面」。
    # ★ 为什么必须走命令队列：浏览器 POST 的响应体**只有浏览器看得见**，ESP 看不见 ⇒
    #   "云端调过 AI" 和 "帧到了屏上" 是两件事，中间只有一条路（NAT 穿透就是它）：
    #   enqueue_command → ESP 每秒轮询取走（响应里的 `command`）→ forwardCommandToF407 → F407。
    # ★ 为什么不在前端"拿到 AI 的帧再 POST /api/command"：`decision["raw"]` 是**大模型的
    #   原始回复文本**（ai_relay.py:150/206/228），**不是 `@...#` 帧串** ⇒ 前端手上没有帧，
    #   那样做等于在 JS 里再写一份 decision_to_raw()（两份实现必然漂移）。
    # ❌ 绝不无脑在每次 api_data 里 enqueue —— ESP 每 1 秒一次空轮询（user=""），
    #   那样会每秒灌一条 `@TEXT:已记录（未调用AI）`。下面 `decision.get("ai") is False`
    #   那道闸就是钉这个的（占位决策不带 ai 字段，未调 AI 的那条显式 ai=False）。
    if deliver:
        raw = ""
        clamp_note = []
        dstate, derr = "", ""
        if relay_failed:
            dstate, derr = "refused_no_ai", "AI 调用失败"
        elif decision.get("ai") is False:
            dstate, derr = "refused_no_ai", "未调用 AI"
        else:
            raw = decision_to_raw(decision, clamp_note, device_id=device_id)
            age = device_age_sec(device_id)
            if not raw:
                dstate, derr = "refused_empty_frame", "AI 没产生可下发内容"
            elif age is not None and age > DEVICE_ONLINE_SEC:
                # 只在有"过期的正面证据"时拒发；age is None（服务刚重启/从没上报过）
                # 照常入队 —— 那种情况下设备 1 秒内就会来取，拒发反而是把正常的丢掉。
                dstate = "refused_offline"
                derr = "设备离线（最后一次上报在 %d 秒前）" % int(age)
            elif _queue_len(device_id) >= DELIVER_MAX_QUEUE:
                dstate = "refused_queue_full"
                derr = "前面还有 %d 条没被设备取走" % _queue_len(device_id)
            else:
                did = _note_delivery(device_id, ts, user, raw, "queued")
                qlen = enqueue_reply(device_id, {
                    "time": ts,
                    "device_id": device_id,
                    "raw": raw,
                    "note": "web_chat_mock" if mock else "web_chat",
                    "_did": did,
                })
                if qlen is None:
                    with _deliver_lock:
                        for rec in _deliveries:
                            if rec["id"] == did:
                                rec.update(state="refused_queue_full", err="回复队列已满")
                    return jsonify({"ok": False, "deliver_state": "refused_queue_full",
                                    "deliver_err": "回复队列已满，请稍后再试"}), 429
                try:
                    store.record_command(ts, device_id, raw=raw, source="web_chat",
                                         note="web_chat_mock" if mock else "web_chat")
                except Exception as e:
                    print("[STORE][ERROR] 命令落库失败（不影响下发）：%s: %s"
                          % (type(e).__name__, e))
                dstate = "queued"
                decision["queue_len"] = qlen
                # 投递单号：互动页拿它去 /api/deliveries 对账，把"已入队"升级成
                # "设备已取走"。给的 id 是**精确**的，不靠 time+user 去猜。
                decision["deliver_id"] = did
                print("[%s] 网页对话下发 device=%s queue=%d raw=%s" % (
                    ts, device_id, qlen, raw))
        if not dstate:
            dstate, derr = "refused_unknown", "未知原因"
        if dstate != "queued":
            _note_delivery(device_id, ts, user, raw, dstate, derr)
            print("[%s] 网页对话未下发 device=%s reason=%s" % (ts, device_id, dstate))
        decision["deliver_state"] = dstate
        decision["deliver_err"] = derr
        decision["device_frames"] = raw      # ★ 字段名不叫 raw —— `raw` 在本工程已是"AI 原始输出"
        if clamp_note:
            decision["servo_clamped"] = clamp_note
    # === [03网页端] END ===

    # ⑥ 无论是否调 AI，都尝试捎带一条待执行 command（网页遥控队列）
    #    ★ [03网页端] 2026-09-23：**只有"设备自己的轮询"才有资格领**。
    #      浏览器/PC 领了就是**丢**（`pop` 之后 ESP 若超时/解析失败不会重发），
    #      而浏览器也不转发它 —— 症状是"点了遥控按钮、再打字聊天，遥控那一下没了"。
    sampled_was_busy = sampled_player.busy(device_id)
    if not from_web and isinstance(body.get('step_playback'), dict):
        packet = sampled_player.exchange(device_id, body['step_playback'],
                    device_profile.effective(device_profile.load(), device_controls.get(device_id)['servo_speed']))
        if packet:
            decision['step_playback'] = packet
    cmd = pop_command(device_id, timeline_busy=body.get('motion_busy') is True) if not from_web and not sampled_was_busy else None
    if not from_web:
        # 普通心跳也会生成“已记录”占位文字，但 ai=False；它不属于用户交互。
        # 若把占位文字当成AI回复，每次轮询都会清空偏差，跟随将永远无法启动。
        if sampled_was_busy:
            # 逐步动作持有控制权，整条AI回复留在普通队列，完成后继续领取。
            decision['ai'] = False
            if companion_runtime.LOCATOR.pending(device_id):
                companion_runtime.LOCATOR.cancel(device_id, '其它动作优先，请重新定位')
            follow.stop(device_id, '录制动作正在执行')
        elif cmd is not None or run_ai or mock:
            if companion_runtime.LOCATOR.pending(device_id):
                companion_runtime.LOCATOR.cancel(device_id, '手动或AI指令优先，请重新定位')
            follow.stop(device_id, '手动或AI交互优先', hold=5)
        else:
            follow_settings = camera.current_settings(device_id)
            follow_blockers = dict(camera_blockers(device_id))
            if camera.gate(device_id, follow_settings) and not companion_runtime.LOCATOR.pending(device_id):
                follow_blockers['policy'] = True
            if companion_runtime.LOCATOR.pending(device_id):
                auto_move = companion_runtime.LOCATOR.take(device_id, follow_settings, board_pose.get(device_id),
                        device_profile.effective(device_profile.load(), device_controls.get(device_id)['servo_speed']),
                        camera_blockers(device_id))
            else:
                auto_move = follow.take(device_id, follow_settings, board_pose.get(device_id),
                        device_profile.effective(device_profile.load(), device_controls.get(device_id)['servo_speed']),
                        follow_blockers)
            if auto_move:
                auto_move['raw'] = decision_to_raw(auto_move, device_id=device_id)
                cmd = auto_move
                print("[FOLLOW] device=%s job=%s servo_params=%s raw=%s" % (
                    device_id, auto_move.get('camera_job', ''),
                    [(p['id'], p['angle'], p['speed']) for p in auto_move.get('servo_params', [])],
                    auto_move['raw']))
    if cmd is not None:
        decision["command"] = cmd
        if not from_web:
            # Manual queued actions take priority over an unsolicited AI burst.
            # One packet per response keeps the entire UART burst within RX ring.
            decision["ai"] = False
        print("[%s] 下发 command → %s : %s" % (
            ts, device_id, json.dumps(cmd, ensure_ascii=False)))

    # === [01语音] BEGIN 2026-09-24 云端 OTA：公告搭在这条**既有的**设备轮询上 ===
    # ★ 不新增"有活吗"的端点、不新增连接 —— 设备本来就每秒 POST 这里一次，
    #   反应时间 ≤1 秒，而且白捡已验收的自动发现/重试/穿 NAT 那一整套。
    # ★ 认领规则与上面那条 `command` **同一条**：只有"设备自己的轮询"才有资格领
    #   （`from_web` 的请求领走了 = 设备的活被浏览器抢了，详见 `iap_pull.announce`）。
    iap = iap_pull.announce(device_id, from_web)
    if iap is not None:
        decision["iap"] = iap
        print("[%s] 下发 iap 任务 → %s : %s" % (
            ts, device_id, json.dumps(iap, ensure_ascii=False)))
    # === [01语音] END ===

    if not from_web:
        decision["controls"] = device_controls.get(device_id)
        decision["motion_profile"] = device_profile.motion_snapshot(body.get("motion_ack"))
        decision["idle_profile"] = idle_pet.snapshot(device_id, device_profile.load(), device_controls.get(device_id)["servo_speed"])
        if decision.get("ai") is not False and not relay_failed:
            decision["device_frames"] = decision_to_raw(decision, device_id=device_id)
            # The ESP HTTP receiver is bounded at 4KB. Send one compact wire
            # representation, retaining rich schedules in browser/log responses.
            if "timeline" in decision or any(isinstance(m, dict) and "at_ms" in m for m in decision.get("moves") or []):
                decision.pop("timeline", None)
                decision.pop("raw", None)
                decision["moves"] = []
    decision["device_id"] = device_id
    # 本次触发的异常（测试页/网页可以直接看到"我这条注入被判成了什么"）
    if fired_alerts:
        decision["alerts"] = fired_alerts
    return jsonify(decision)


# ==================================================================
# 语音链路（P2）—— [01语音] 独占新增，见 合作表 §4.6
# ==================================================================
def _audio_inject_cfg():
    """注入 /api/data 要的 url/token/device_id —— 从 config 现读，**只传不打印**。"""
    cfg = load_config()
    host = str(cfg.get("host", "127.0.0.1"))
    if host in ("0.0.0.0", ""):
        host = "127.0.0.1"
    return {"url": "http://%s:%d/api/data" % (host, int(cfg.get("port", 5000))),
            "token": str(cfg.get("token", "")),
            "device_id": DEFAULT_DEVICE_ID}


@app.route("/api/audio/chunk", methods=["POST"])
def api_audio_chunk():
    """ESP 上行：一块裸 PCM。★ 正文是**二进制**，不是 hex（合作表 §4.6）。

    seq=0 那一次必须带 `total`（总字节数），后面几块只要 seq。
    ★ 有空洞时返回 409 + `gap`，让 ESP 补发，别静默收下 —— 静默丢块最难查。
    """
    sid = str(request.args.get("sess", "") or "")
    try:
        seq = int(request.args.get("seq", "0"))
    except ValueError:
        return jsonify({"ok": False, "err": "seq 不是整数"}), 400
    data = request.get_data(cache=False) or b""
    r = audio_rx.feed_chunk(sid, seq, data,
                            rate=request.args.get("rate"),
                            bits=request.args.get("bits"),
                            ch=request.args.get("ch"),
                            total=request.args.get("total"))
    if seq == 0 and r.get('ok'):
        _note_sound(str(request.args.get('device_id') or DEFAULT_DEVICE_ID), sid)
    return jsonify(r), (200 if r.get("ok") else 409)


@app.route("/api/audio/stream", methods=["POST"])
def api_audio_stream():
    """ESP 上行：**一次 POST 把整段 PCM 流完**（`Content-Length` 裸 PCM）。

    ★ P3 选定的传输方式（原来那套"每块一次 POST"退为对照组）。理由实测过：
      werkzeug 的开发服务器**不支持 keep-alive**（每个响应都带 `Connection: close`），
      ⇒ 每块一次 POST = 每块一次全新 TCP 握手。10 秒录音 156 块 = 156 次握手，
      而 ESP8266 走 WiFi，握手远比传 2048 B 贵。⇒ **一次录音 = 一个请求 = 一条连接**。
      （实测按 32000 B/s 真实节拍灌 9.984 秒：服务端 9.920 秒收齐、边收边写。）

    契约：
      `POST /api/audio/stream?sess=<新id>&token=<t>&rate=16000&bits=16&ch=1&total=319488`
      正文 = **裸 PCM 字节流**（不按块、没有 seq）；`total` 必须等于会发出去的字节数。
      ⇒ 200 `{"ok":true,"complete":true,"received":..,"dur":..,"queued":true,"saved":".."}`
      ⇒ 收不齐：`complete:false` + err（**不送识别**，残件留在盘上）
      ⇒ 409 `{"ok":false,"err":".."}`：total 不合法 / 会话超限 / **sess 复用**

    ★ `total` **不能写 `seconds*32000`**：数据颗粒度是 32ms 半块，10 秒 = 312.5 半块
      ⇒ 取整成 312×1024 = **319488**（9.984 秒）。写 320000 会永远收不齐、
      零报错、**永远不出字**（`audio_rx.py` 判 `received == total` 才送识别）。

    ★ 收尾在**这一条请求里**做完（内部调 `audio_rx.finish`）⇒ ESP **不要再发 `/end`**。
      另：响应要等正文发完才出，所以 ESP 那边的"等响应超时"必须从**正文发完之后**开始算。
    """
    sid = str(request.args.get("sess") or "")
    vad_short = str(request.args.get("vad") or "").strip() == "1"
    s, err = audio_rx.open_stream(sid, request.args.get("rate"), request.args.get("bits"),
                                  request.args.get("ch"), request.args.get("total"), vad_short=vad_short)
    if s is None:
        return jsonify({"ok": False, "err": err}), 409
    _note_sound(str(request.args.get('device_id') or DEFAULT_DEVICE_ID), sid)
    wrote, stop = 0, None
    while True:
        try:
            b = request.stream.read(4096)
        except Exception as e:                 # 对方断开 / 正文没发完
            stop = "读正文失败 %s: %s" % (type(e).__name__, e)
            s.mark_err(stop)
            break
        if not b:
            break
        r = s.append_raw(b)
        wrote += len(b)
        if not r.get("ok"):
            stop = r.get("err")
            break
        if s.received >= s.total:
            break
    if stop:
        print("[AUDIO] 流式 %s 中断：%s（已收 %d/%d）" % (sid, stop, s.received, s.total))
    out = audio_rx.finish(sid, _audio_inject_cfg())
    out["streamed"] = True
    out["wrote"] = wrote
    return jsonify(out), (200 if out.get("ok") else 404)


@app.route("/api/audio/end", methods=["POST"])
def api_audio_end():
    """ESP 上行：本段结束。

    ★ **立刻返回**，识别在后台线程 —— ESP 的 `HTTP_TIMEOUT_MS` 只有 3000ms，
    而实测模型加载 4.3 秒。结果去 `GET /api/audio/status` 取。
    """
    sid = str(request.args.get("sess", "") or "")
    r = audio_rx.finish(sid, _audio_inject_cfg(), samples=request.args.get("samples"))
    return jsonify(r), (200 if r.get("ok") else 404)


@app.route("/api/tts/speech", methods=["POST"])
def api_tts_speech():
    """旧客户端的停用响应：不合成、不访问模型、不发送音频。"""
    return jsonify({"ok": False, "err": "语音输出已停用，请使用文字和动作回复"}), 410


@app.route("/api/audio/status")
def api_audio_status():
    """网页读状态：模式 / 模型状况 / 最近一次结果 / 待确认的文字。"""
    status = audio_rx.status()
    with _io_lock:
        status["transport"] = {key: dict(value) for key, value in _audio_transport_status.items()}
    # Presentation freshness must not depend on the browser/server timezone.
    for report in status["transport"].values():
        try:
            report["age_seconds"] = max(0, (datetime.now() - datetime.strptime(report["at"], "%Y-%m-%d %H:%M:%S")).total_seconds())
        except (KeyError, TypeError, ValueError):
            report["age_seconds"] = None
    return jsonify(status)


@app.route("/api/audio/history")
def api_audio_history():
    try:
        return jsonify(audio_rx.history(int(request.args.get("offset", 0))))
    except (TypeError, ValueError):
        return jsonify({"err": "offset 必须是整数"}), 400


@app.route("/api/device/controls", methods=["GET", "POST"])
def api_device_controls():
    body = request.get_json(silent=True) or {}
    device = str(body.get("device_id") or request.args.get("device_id") or DEFAULT_DEVICE_ID)
    try:
        controls = device_controls.save(device, body.get("controls")) if request.method == "POST" else device_controls.get(device)
        return jsonify({"ok": True, "device_id": device, "controls": controls})
    except ValueError as exc:
        return jsonify({"ok": False, "err": str(exc)}), 400


@app.route("/api/audio/retention", methods=["POST"])
def api_audio_retention():
    body = request.get_json(silent=True) or {}
    try:
        return jsonify({"ok": True, "retention": audio_rx.set_retention(body.get("limit"))})
    except ValueError as exc:
        return jsonify({"ok": False, "err": str(exc)}), 400


@app.route("/api/audio/latest.wav")
def api_audio_latest_wav():
    """Logged-in users may play a selected archived recording."""
    wanted = request.args.get("id") or request.args.get("sess")
    last = audio_rx.recording(wanted) if wanted else audio_rx.status().get("last")
    last = last or {}
    if not (last.get("complete") or last.get("playable") or last.get("received", 0) > 0):
        return jsonify({"err": "没有匹配的完整录音，请刷新结果"}), 404
    path = os.path.realpath(last.get("saved") or "")
    base = os.path.realpath(audio_rx.WAV_DIR)
    try:
        valid = os.path.commonpath([base, path]) == base
    except ValueError:
        valid = False
    if not valid or not path.lower().endswith(".wav") or not os.path.isfile(path):
        return jsonify({"err": "录音文件不存在"}), 404
    response = send_file(path, mimetype="audio/wav", conditional=True,
                        download_name=os.path.basename(path))
    response.headers["Cache-Control"] = "no-store"
    return response


@app.route("/api/audio/send", methods=["POST"])
def api_audio_send():
    """手动模式：网页点了"确认发送" ⇒ 把挂起的文字真的灌回 /api/data。"""
    body = request.get_json(silent=True) or {}
    r = audio_rx.queue_pending(_audio_inject_cfg(), body.get("id") or body.get("sess"))
    return jsonify(r), (202 if r.get("ok") else 409)


@app.route("/api/audio/drop", methods=["POST"])
def api_audio_drop():
    """手动模式：网页点了"这条不要" ⇒ 丢掉挂起的文字。"""
    body = request.get_json(silent=True) or {}
    return jsonify(audio_rx.drop_pending(body.get("id") or body.get("sess")))


@app.route("/api/audio/mode", methods=["POST"])
def api_audio_mode():
    """切 manual / auto。body `{"mode":"manual"|"auto"}`，也认 `?mode=`。"""
    body = request.get_json(silent=True) or {}
    got = audio_rx.set_mode(body.get("mode") or request.args.get("mode"))
    if got is None:
        return jsonify({"ok": False, "err": "mode 只能是 manual 或 auto",
                        "mode": audio_rx.get_mode()}), 400
    return jsonify({"ok": True, "mode": got})



@app.route("/api/device/pose")
def device_pose():
    device = request.args.get("device_id") or DEFAULT_DEVICE_ID
    return jsonify({"ok": True, "device_id": device, "pose": board_pose.get(device)})

@app.route("/api/admin/idle-pet", methods=["GET", "POST"])
def admin_idle_pet():
    body = request.get_json(silent=True) or {}
    device = str(body.get("device_id") or request.args.get("device_id") or DEFAULT_DEVICE_ID)
    try:
        profile = device_profile.load()
        speed = device_controls.get(device)["servo_speed"]
        if request.method == "POST":
            idle_pet.save(device, body.get("settings"), profile, speed)
        return jsonify({"ok": True, "settings": idle_pet.get(device),
                        "sync": idle_pet.snapshot(device, profile, speed), "pose": board_pose.get(device)})
    except (ValueError, TypeError) as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

@app.route("/api/admin/action-preview", methods=["POST"])
def admin_action_preview():
    body = request.get_json(silent=True) or {}
    device = str(body.get("device_id") or DEFAULT_DEVICE_ID)
    try:
        raw, plan = motion_timeline.compile_timeline(body.get("timeline"), device_profile.load(),
                            device_controls.get(device)["servo_speed"])
        return jsonify({"ok": True, "plan": plan, "note": "只预览，不下发"})
    except (ValueError, TypeError) as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400




@app.route("/api/admin/actions", methods=["POST"])
def admin_action_save():
    body = request.get_json(silent=True) or {}
    try:
        profile = device_profile.save_action(body.get("action"))
        return jsonify({"ok": True, "profile": profile})
    except (ValueError, TypeError) as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

import companion_runtime
companion_runtime.install(__import__(__name__))

if __name__ == "__main__":
    companion_runtime.prepare(__import__(__name__))
    cfg = load_config()
    print("=" * 50)
    print("  智能家居 AI 中转站启动")
    print("  监听 http://%s:%s" % (cfg.get("host", "0.0.0.0"), cfg.get("port", 5000)))
    print("  active_provider = %s" % cfg.get("active_provider", "auto"))
    _tk = cfg.get("auto_tick") or {}
    print("  定时唤醒 = %s（每 %ss，静默期 %ss）" % (
        "开" if _tk.get("enabled") else "关",
        _tk.get("interval_sec", 60), _tk.get("quiet_sec", 60)))
    print("  自动发现 = UDP %d（ESP 来问路就告诉它本机 IP）" % BEACON_PORT)
    print("=" * 50)
    start_auto_tick()
    start_store()
    start_beacon()
    app.run(host=cfg.get("host", "0.0.0.0"),
            port=int(cfg.get("port", 5000)),
            threaded=True)

