"""device_profile.py：三轴舵机限位、回正、速度、表情和管理员动作库的规范档案。

使用：load 合并与迁移默认值，save/validate 验证后保存；AI 和时间线均使用当前档案。
时序与边界：servo-contract.json 是物理默认参数源；保留用户速度及自定义动作，禁止把上下轴默认角改回 90。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
# -*- coding: utf-8 -*-
import copy
import json
import os
import re
import tempfile
import struct
import zlib
import threading
import servo_contract

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PROFILE_PATH = os.path.join(BASE_DIR, "admin_device_profile.json")
_profile_lock = threading.RLock()
# 协议里的易读 ID 可以比 FATFS 短文件名长（eyesclose → EYESCLOS.BMP）。
FACE_RE = re.compile(r"^[a-z0-9_]{1,24}$")
BMP_RE = re.compile(r"^[A-Z0-9_]{1,8}\.BMP$")


def default_profile():
    return {
        "schema": 1,
        "servo_contract_revision": servo_contract.REVISION,
        "servos": [
            *copy.deepcopy(servo_contract.CONTRACT['servos']),
        ],
        "expressions": [
            {"id": "doubtful", "file": "DOUBTFUL.BMP", "name": "疑惑"},
            {"id": "eyesclose", "file": "EYESCLOS.BMP", "name": "闭眼"},
            {"id": "happy", "file": "HAPPY.BMP", "name": "开心"},
            {"id": "lookleft", "file": "LOOKLEFT.BMP", "name": "看左"},
            {"id": "lookup1", "file": "LOOKUP1.BMP", "name": "抬头 1"},
            {"id": "lookup2", "file": "LOOKUP2.BMP", "name": "抬头 2"},
            {"id": "sleep", "file": "SLEEP.BMP", "name": "睡觉"},
            {"id": "smell", "file": "SMELL.BMP", "name": "闻一闻"},
            {"id": "surprise", "file": "SURPRISE.BMP", "name": "惊讶"},
            {"id": "think", "file": "THINK.BMP", "name": "思考"},
            {"id": "wink", "file": "WINK.BMP", "name": "眨眼"},
        ],
        "actions": [
            {"id": "nod", "name": "点头", "meaning": "认同、回应、鼓励或轻微致意", "ready": True,
             "timeline": {"steps": [{"at_ms": 0, "move": {"type": "gesture", "name": "nod"}}]}},
            {"id": "shake", "name": "摇头", "meaning": "否定、困惑、无奈或提醒", "ready": True,
             "timeline": {"steps": [{"at_ms": 0, "move": {"type": "gesture", "name": "shake"}}]}},
            {"id": "wave", "name": "打招呼", "meaning": "问候、欢迎、道别或吸引注意", "ready": True,
             "timeline": {"steps": [{"at_ms": 0, "move": {"type": "gesture", "name": "hi"}}]}},
            {"id": "look_left", "name": "看左边", "meaning": "看向左侧、寻找或表现好奇", "ready": True,
             "timeline": {"steps": [{"at_ms": 0, "move": {"type": "gesture", "name": "look_left"}}]}},
            {"id": "look_right", "name": "看右边", "meaning": "看向右侧、回应右方的人或物", "ready": True,
             "timeline": {"steps": [{"at_ms": 0, "move": {"type": "gesture", "name": "look_right"}}]}},
            {"id": "home", "name": "回正", "summary": "全部登记舵机回到管理员保存的正面角度", "meaning": "面对用户、恢复自然姿态", "ready": True,
             "moves": [{"type": "home"}]},
            {"id": "happy", "name": "开心", "summary": "切换开心表情", "meaning": "开心、认可、庆祝或友好", "ready": True,
             "moves": [{"type": "expression", "face": "happy"}]},
            {"id": "think", "name": "思考", "summary": "切换思考表情", "meaning": "思考、分析、认真倾听或暂时不确定", "ready": True,
             "moves": [{"type": "expression", "face": "think"}]},
            {"id": "surprise", "name": "惊讶", "summary": "切换惊讶表情", "meaning": "意外、惊叹、听到新消息", "ready": True,
             "moves": [{"type": "expression", "face": "surprise"}]},
            {"id": "wink", "name": "眨眼", "summary": "切换眨眼表情", "meaning": "俏皮、默契、轻松的回应", "ready": True,
             "moves": [{"type": "expression", "face": "wink"}]},
            {"id": "idle_blink", "name": "待机眨眼", "meaning": "自然眨眼，短暂放松", "custom": True, "confirmed": False,
             "timeline": {"duration_ms": 4000, "steps": [{"at_ms": 0, "move": {"type": "expression", "face": "blink"}}]}},
            {"id": "idle_look", "name": "待机左右看", "meaning": "空闲时好奇地左右看，结束回正", "custom": True, "confirmed": False,
             "timeline": {"steps": [{"at_ms": 0, "move": {"type": "expression", "face": "look"}},
                                     {"at_ms": 0, "move": {"type": "gesture", "name": "shake"}}]}},
            {"id": "idle_sleep", "name": "待机睡觉", "meaning": "长时间无人交互时休息；听到人声仍会唤醒", "custom": True, "confirmed": False,
             "timeline": {"duration_ms": 4000, "steps": [{"at_ms": 0, "move": {"type": "expression", "face": "sleep"}}]}},
            {"id": "nod_twice_timeline", "name": "连续点头两次", "summary": "按当前安全限位自动编排", "ready": True,
             "timeline": {"repeat": 2,
                          "steps": [{"at_ms": 0, "move": {"type": "gesture", "name": "nod"}}]}},
        ],
    }


def _text(value, field, limit=80):
    value = str(value if value is not None else "").strip()
    if len(value) > limit:
        raise ValueError("%s 最多 %d 个字符" % (field, limit))
    return value


def validate(profile):
    if not isinstance(profile, dict):
        raise ValueError("设备档案必须是对象")
    result = {"schema": 1, "servo_contract_revision": servo_contract.REVISION,
              "servos": [], "expressions": [], "actions": []}
    seen = set()
    servos = profile.get("servos")
    if not isinstance(servos, list) or not servos:
        raise ValueError("至少要有一条舵机配置")
    for s in servos:
        if not isinstance(s, dict):
            raise ValueError("舵机配置格式错误")
        try:
            sid, lo, mid, hi, front = (int(s.get(k)) for k in ("id", "min", "mid", "max", "front"))
        except (TypeError, ValueError):
            raise ValueError("舵机编号和角度必须是整数")
        if sid < 1 or sid > 16 or sid in seen:
            raise ValueError("舵机编号必须为 1~16 且不能重复")
        if not (0 <= lo <= mid <= hi <= 180 and lo <= front <= hi):
            raise ValueError("舵机%d 要满足 0 ≤ 最小 ≤ 中值 ≤ 最大 ≤ 180，正面角度须在最小~最大之间" % sid)
        seen.add(sid)
        result["servos"].append({"id": sid, "name": _text(s.get("name"), "舵机名称") or "舵机%d" % sid,
                                 "role": _text(s.get("role"), "舵机角色") or "user_defined",
                                 "min": lo, "mid": mid, "max": hi, "front": front,
                                 "direction": _text(s.get("direction"), "方向说明", 160),
                                 "note": _text(s.get("note"), "备注", 240)})
        speed = s.get("speed")
        if speed is not None:
            if isinstance(speed, bool) or not isinstance(speed, int) or not 0 <= speed <= 360:
                raise ValueError("舵机%d 默认速度必须是0~360的整数（度/秒）" % sid)
            result["servos"][-1]["speed"] = speed
    servo_contract.check(result['servos'])
    expressions = profile.get("expressions") or []
    face_seen = set()
    for x in expressions:
        if not isinstance(x, dict):
            raise ValueError("表情配置格式错误")
        face = _text(x.get("id"), "表情 ID").lower()
        if not FACE_RE.match(face) or face in face_seen:
            raise ValueError("表情 ID 只能是 1~24 位小写字母、数字或下划线，且不能重复")
        face_seen.add(face)
        filename = _text(x.get("file"), "BMP 文件名", 12).upper()
        if not BMP_RE.match(filename):
            raise ValueError("BMP 文件名必须是 FATFS 8.3 格式，例如 EYESCLOS.BMP")
        result["expressions"].append({"id": face, "file": filename,
                                      "name": _text(x.get("name"), "表情名称") or face})
    for face, name in [("blink","眨眼"),("shy","害羞"),("glasses","戴眼镜"),("angry","生气"),
                       ("funny","搞怪"),("pout","嘟嘴"),("laugh","大笑"),("starry","星星眼"),
                       ("right","向右看"),("doubt","疑惑"),("look","左右看"),("smile","微笑")]:
        if face not in face_seen:
            result["expressions"].append({"id":face,"name":name,"file":face.upper()+".BMP"})
    # Custom recipes are data, never executable code or arbitrary raw frames.
    import motion_timeline
    defaults = {x["id"]: x for x in default_profile()["actions"]}
    action_ids = set()
    for x in profile.get("actions") or default_profile()["actions"]:
        if not isinstance(x, dict):
            raise ValueError("动作必须是对象")
        aid = str(x.get("id", ""))
        if not re.fullmatch(r"[a-z0-9_]{1,32}", aid) or aid in action_ids:
            raise ValueError("动作ID需为1~32位小写字母数字下划线且不重复")
        action_ids.add(aid)
        if x.get("custom") is True and aid != "home":
            if not isinstance(x.get("confirmed", False), bool):
                raise ValueError("动作确认开关必须是布尔值")
            recipe = copy.deepcopy(x.get("timeline"))
            recipe_error = ""
            try: motion_timeline.compile_timeline(recipe, result, 90)
            except (ValueError, TypeError) as exc: recipe_error = str(exc)
            result["actions"].append({"id": aid, "name": _text(x.get("name"), "动作名称") or aid,
                "meaning": _text(x.get("meaning"), "动作含义", 240), "custom": True,
                "confirmed": x.get("confirmed", False), "ready": x.get("confirmed", False) and not recipe_error,
                "unavailable_reason": recipe_error or ("管理员尚未确认启用此动作" if not x.get("confirmed") else ""),
                "timeline": recipe})
        elif aid in defaults:
            result["actions"].append(copy.deepcopy(defaults[aid]))
        else:
            raise ValueError("新动作需使用custom=true和时间序列配方")
    for aid in ["home", "idle_blink", "idle_look", "idle_sleep"]:
        if aid not in action_ids:
            result["actions"].append(copy.deepcopy(defaults[aid]))
    for action in result["actions"]:
        if action.get("custom"):
            action["ready"] = action.get("confirmed", False) and not action.get("unavailable_reason")
            if not action["ready"] and not action.get("unavailable_reason"):action["unavailable_reason"] = "管理员尚未确认启用此动作"
    roles = [s["role"] for s in result["servos"] if s["role"] in ("pitch", "yaw")]
    if len(roles) != len(set(roles)):
        raise ValueError("点头 pitch 和摇头 yaw 各只能绑定一个舵机")
    gesture_roles = {"nod": "pitch", "wave": "pitch", "nod_twice_timeline": "pitch",
                     "shake": "yaw", "look_left": "yaw", "look_right": "yaw"}
    for action in result["actions"]:
        if action.get("custom"):continue
        role = gesture_roles.get(action["id"])
        if role:
            servo = next((s for s in result["servos"] if s["role"] == role), None)
            if servo is None:
                action.update(ready=False, unavailable_reason="请先在管理员页面登记%s舵机" % role)
            else:
                end = ("目标%d°" % servo["min"] if action["id"] == "look_left" else
                       "目标%d°" % servo["max"] if action["id"] == "look_right" else
                       "收尾回正%d°" % servo["front"])
                action["summary"] = "舵机%d，安全范围%d~%d°，%s" % (servo["id"], servo["min"], servo["max"], end)
        elif action["id"] == "home":
            action["summary"] = "回正：" + "、".join("舵机%d=%d°" % (s["id"], s["front"]) for s in result["servos"])
        elif action["id"] not in face_seen:
            action.update(ready=False, unavailable_reason="管理员表情库尚未登记这个表情")
    return result


def motion_snapshot(ack=0):
    """Repeat an atomic limits packet until F407 acknowledges the exact CRC."""
    servos = load()["servos"]
    entries = b"".join(struct.pack("6B", s["id"], s["min"], s["max"], s["front"],
                                   {"pitch": 1, "yaw": 2}.get(s["role"], 0), 0) for s in servos)
    revision = zlib.crc32(entries) or 1
    data = struct.pack("<BBI", 1, len(servos), revision) + entries
    return {"revision": revision, "wire": data.hex() if ack != revision else "",
            "applied": ack == revision}


def load():
    try:
        with _profile_lock:
            with open(PROFILE_PATH, "r", encoding="utf-8") as f:
                raw = json.load(f)
            upgraded = servo_contract.migrate(raw)
            result = validate(upgraded)
            if raw.get('servo_contract_revision') != servo_contract.REVISION:
                backup = PROFILE_PATH + '.before_servo_contract.json'
                if not os.path.exists(backup):
                    with open(backup, 'x', encoding='utf-8') as f:
                        json.dump(raw, f, ensure_ascii=False, indent=2)
                return _save(result)
            return result
    except (OSError, ValueError, json.JSONDecodeError):
        profile = validate(default_profile())
        save(profile)
        return profile


def _save(profile):
    profile = validate(profile)
    # 临时文件必须和目标在同一卷；否则 Windows 的 os.replace 会报跨盘错误。
    parent = os.path.dirname(os.path.abspath(PROFILE_PATH))
    os.makedirs(parent, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix="device_profile_", suffix=".json", dir=parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(profile, f, ensure_ascii=False, indent=2)
            f.write("\n")
        os.replace(temp, PROFILE_PATH)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)
    return profile


def save(profile):
    with _profile_lock:
        return _save(profile)


def save_speeds(speeds):
    """Merge only speeds; preserve angles, role IDs and expression files."""
    if not isinstance(speeds, dict) or not speeds:
        raise ValueError("请提供要保存的舵机速度")
    with _profile_lock:
        profile = load()
        servos = {str(s["id"]): s for s in profile["servos"]}
        for sid, speed in speeds.items():
            if sid not in servos:
                raise ValueError("舵机%s 未登记" % sid)
            if isinstance(speed, bool) or not isinstance(speed, int) or not 0 <= speed <= 360:
                raise ValueError("速度必须是0~360的整数（度/秒）")
            servos[sid]["speed"] = speed
        return _save(profile)


def effective(profile, default_speed=90):
    result = copy.deepcopy(profile)
    for servo in result["servos"]:
        if servo.get("speed") is None:
            servo["speed"] = default_speed
    return result


def apply_to_config(config, default_speed=90):
    """返回只用于本次决策的配置副本，不写 config.json。"""
    config = copy.deepcopy(config)
    profile = effective(load(), default_speed)
    config.setdefault("hardware", {})["servos"] = copy.deepcopy(profile["servos"])
    config["device_profile"] = profile
    return config


def action(action_id):
    for item in load()["actions"]:
        if item["id"] == action_id:
            return copy.deepcopy(item)
    return None


def save_action(action):
    """Merge one recipe into the latest profile, preserving concurrent calibration."""
    if not isinstance(action, dict) or action.get("id") == "home" or action.get("custom") is not True:
        raise ValueError("自定义动作必须带custom=true，不能修改系统home")
    with _profile_lock:
        profile = load()
        profile["actions"] = [a for a in profile["actions"] if a["id"] != action.get("id")]
        profile["actions"].append(copy.deepcopy(action))
        validated = validate(profile)
        saved = next(a for a in validated["actions"] if a["id"] == action.get("id"))
        if saved.get("confirmed") and not saved.get("ready"):
            raise ValueError(saved.get("unavailable_reason", "动作不能执行"))
        return _save(validated)

def save_parameters(profile):
    with _profile_lock:
        if not isinstance(profile, dict):raise ValueError("设备档案必须为对象")
        profile = copy.deepcopy(profile)
        profile["actions"] = load()["actions"]
        return _save(profile)
