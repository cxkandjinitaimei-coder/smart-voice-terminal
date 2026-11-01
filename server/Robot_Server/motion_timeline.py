"""F407 时间轴动作的校验、编码和二进制 type 0x20 帧。

网页提交原始录制后，本模块先检查 schema、绝对角、采样时间、三轴映射和当前
舵机限位，再生成一个能放入 F407 1024 字节接收环的原子包。板端收到并校验后，
时间从 MCU 接受时刻开始计算；浏览器和云端时钟不能直接驱动舵机。编码阶段必须
保持“起始/版本/步数/总时长 -> 每步数据 -> CRC -> 提交”的顺序，任何校验失败
都不能让半包进入 UART 队列。
"""
import re
import struct

MAX_STEPS = 32
MAX_DURATION = 60000
MAX_TOTAL = 120000
HEADER = struct.Struct("<BBBBI")
STEP = struct.Struct("<IBBHH8s")


def integer(value, label, lo, hi):
    if isinstance(value, bool) or not isinstance(value, int) or not lo <= value <= hi:
        raise ValueError("%s 必须是 %d~%d 的整数" % (label, lo, hi))
    return value


def compile_timeline(value, profile, default_speed=90):
    if isinstance(value, list):
        value = {"steps": value}
    if not isinstance(value, dict):
        raise ValueError("timeline 必须包含 steps、duration_ms、repeat")
    if value.get("stop") is True:
        return "@TIMELINE:STOP", {"stop": True}
    steps = value.get("steps")
    if not isinstance(steps, list) or not 1 <= len(steps) <= MAX_STEPS:
        raise ValueError("时间序列需要 1~32 个步骤")
    servos = {s["id"]: s for s in profile["servos"]}
    faces = {s["id"]: s["file"].split(".")[0].lower() for s in profile["expressions"]}
    output = []
    occupied = []
    horizon = 0

    def add(at, move):
        kind = move.get("type")
        if kind == "home":
            sid = move.get("id")
            targets = list(servos) if sid is None else [integer(sid, "舵机编号", 1, 16)]
            for target in targets:
                if target not in servos:
                    raise ValueError("舵机 %d 未登记" % target)
                add(at, {"type": "servo", "id": target, "angle": servos[target]["front"],
                         **({"speed": move["speed"]} if "speed" in move else {})})
        elif kind == "servo":
            sid = integer(move.get("id"), "舵机编号", 1, 16)
            if sid not in servos:
                raise ValueError("舵机 %d 未登记" % sid)
            s = servos[sid]
            angle = integer(move.get("angle"), "舵机%d角度" % sid, s["min"], s["max"])
            speed = integer(move.get("speed", s.get("speed", default_speed)), "速度", 0, 360)
            output.append({"at_ms": at, "move": {"type": kind, "id": sid, "angle": angle, "speed": speed}})
        elif kind == "expression":
            face = move.get("face")
            if face not in faces:
                raise ValueError("表情 %s 未登记" % face)
            output.append({"at_ms": at, "move": {"type": kind, "face": face}})
        else:
            raise ValueError("时间序列只支持 servo、home、expression、gesture")

    for item in steps:
        if not isinstance(item, dict) or not isinstance(item.get("move"), dict):
            raise ValueError("每一步需要 at_ms 和 move")
        at = integer(item.get("at_ms"), "at_ms", 0, MAX_DURATION - 1)
        move = item["move"]
        if move.get("type") != "gesture":
            add(at, move)
            continue
        name = move.get("name")
        if name not in ("nod", "hi", "shake", "look_left", "look_right"):
            raise ValueError("未知手势 %s" % name)
        role = "pitch" if name in ("nod", "hi") else "yaw"
        s = next((s for s in servos.values() if s.get("role") == role), None)
        if s is None:
            raise ValueError("手势 %s 需要登记 %s 舵机" % (name, role))
        speed = integer(move.get("speed", s.get("speed", default_speed)), "手势速度", 0, 360)
        front = s["front"]
        if not s["min"] <= front <= s["max"]:
            raise ValueError("手势正面角度不在安全范围")
        lo, hi = s["min"], s["max"]
        angles = {"nod": [lo, hi, lo, front], "hi": [hi, lo, hi, front],
                  "shake": [lo, hi, front], "look_left": [lo], "look_right": [hi]}[name]
        begin = at
        previous = None
        for angle in angles:
            add(at, {"type": "servo", "id": s["id"], "angle": angle, "speed": speed})
            # 没有舵轴反馈时，第一段允许从任意已校验的安全起始角开始。
            distance = max(angle - s["min"], s["max"] - angle) if previous is None else abs(angle - previous)
            # speed=0 表示立即写入 PWM 目标；带时间轴的保持段仍按动作腿分隔。
            # 保存值为 0 时必须保留 0，不能为了“看起来正常”偷偷改成 90。
            at += ((distance * 1000 + speed - 1) // speed if speed else 0) + 200
            previous = angle
        occupied.append((s["id"], begin, at))
        horizon = max(horizon, at)

    output.sort(key=lambda x: x["at_ms"])
    if len(output) > MAX_STEPS:
        raise ValueError("手势展开后超过 32 步，请减少步骤或使用 repeat")
    last_face = None
    seen = set()
    for item in output:
        at, move = item["at_ms"], item["move"]
        key = (at, move.get("id", "face"))
        if key in seen:
            raise ValueError("同一时刻不能重复设置同一舵机或表情")
        seen.add(key)
        if move["type"] == "expression":
            if last_face is not None and at - last_face < 500:
                raise ValueError("表情切换至少间隔 500ms，留出屏幕绘制时间")
            last_face = at
    # 阶段：拒绝重叠的手势时间段，也拒绝插入到手势占用区间内的显式动作。
    for i, (sid, begin, end) in enumerate(occupied):
        for osid, obegin, oend in occupied[i + 1:]:
            if sid == osid and begin < oend and obegin < end:
                raise ValueError("同一舵机的手势时间重叠，请延后下一手势")
        for item in steps:
            m = item["move"]
            same_axis = m.get("type") == "servo" and m.get("id") == sid
            same_axis = same_axis or (m.get("type") == "home" and m.get("id", sid) == sid)
            if same_axis and begin <= item["at_ms"] < end:
                raise ValueError("舵机定点与手势时间重叠")
    duration = integer(value.get("duration_ms", max(output[-1]["at_ms"] + 1000, horizon)),
                       "duration_ms", 1, MAX_DURATION)
    repeat = integer(value.get("repeat", 1), "repeat", 1, 20)
    if output[-1]["at_ms"] >= duration or horizon > duration or duration * repeat > MAX_TOTAL:
        raise ValueError("周期需覆盖所有步骤和手势；总时长不能超过 120 秒")
    face_times = [s["at_ms"] for s in output if s["move"]["type"] == "expression"]
    if repeat > 1 and face_times and duration - face_times[-1] + face_times[0] < 500:
        raise ValueError("循环交界的表情间隔也需要至少 500ms")
    packet = HEADER.pack(1, len(output), repeat, 0, duration)
    for item in output:
        m = item["move"]
        packet += STEP.pack(item["at_ms"], 1 if m["type"] == "servo" else 2,
                            m.get("id", 0), m.get("angle", 0), m.get("speed", 0),
                            faces[m["face"]].encode("ascii") if m["type"] == "expression" else b"")
    return "@TIMELINE:" + packet.hex(), {"steps": output, "repeat": repeat, "duration_ms": duration}


def validate_raw(raw, profile, default_speed=90):
    """Raw servo commands obey the same physical limits as structured plans."""
    if re.search(r"@TIMELINE:(?!STOP(?:#|$))", raw, re.I):
        raise ValueError("请用 timeline 结构发送时间序列，不能直接发送编码数据")
    servos = {s['id']:s for s in profile['servos']}
    for frame in re.finditer(r'@MOVE:servo([^@#\r\n]*)', raw, re.I):
        match = re.fullmatch(r',([0-9]+),([0-9]+)(?:,([0-9]+))?', frame[1])
        if not match:
            raise ValueError('舵机指令格式应为 @MOVE:servo,编号,角度[,速度]#')
        sid,angle = int(match[1]),int(match[2])
        if sid not in servos:
            raise ValueError('舵机%d 未登记' % sid)
        s = servos[sid]
        integer(angle, '舵机%d角度' % sid, s['min'], s['max'])
        if match[3] is not None:integer(int(match[3]), '速度', 0, 360)
