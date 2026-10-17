"""analytics.py：解析温湿度等传感器记录，计算统计并评估阈值规则。

使用：由 app 在收到设备数据或查询图表时调用；输入为原始记录/规则，输出为结构化读数和事件。
时序与边界：采样与图表时间聚合分离；缺失/无效读数不应被当成零值样本。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
# -*- coding: utf-8 -*-

import re

# ---- 与 app.js::parseSensor 逐字一致的两个正则（改这里必须同步改 app.js） ----
RE_TEMP = re.compile(r"温度:\s*([\d.+-]+)")
RE_HUMI = re.compile(r"湿度:\s*([\d.+-]+)")
# ---- 本次新增的字段 ----
RE_DIST = re.compile(r"距离:\s*([\d.+-]+)")
RE_PRESENCE = re.compile(r"(?:人|雷达):\s*(有|无)")

# 表示"没测到"的占位串，一律 valid=0
NULL_WORDS = ("", "无数据", "无", "null", "None", "-")

# 兼容写法：有的地方把 key 写成英文
RE_TEMP_EN = re.compile(r"(?:temp|t):\s*([\d.+-]+)", re.I)
RE_HUMI_EN = re.compile(r"(?:humi|hum|h):\s*([\d.+-]+)", re.I)


def _f(m):
    if not m:
        return None
    try:
        return float(m.group(1))
    except (TypeError, ValueError):
        return None


def parse_sensor(s):
    """把一行传感器文本解析成结构化字段。

    返回 {'temp','humi','distance','presence','valid','raw'}
      - temp/humi/distance 是 float 或 None
      - presence 是 1(有)/0(无)/None(没这个字段)
      - valid 为 True 表示"至少解析出一个物理量"，**统计只看 valid=1**
    """
    raw = str(s or "").strip()
    # 允许带 @SENSOR: 前缀（ESP/F407 发来时带，网页输入框里不带）
    body = raw
    if body.upper().startswith("@SENSOR:"):
        body = body[len("@SENSOR:"):].strip()

    if body in NULL_WORDS:
        return {"temp": None, "humi": None, "distance": None, "presence": None,
                "valid": False, "raw": raw}

    temp = _f(RE_TEMP.search(body))
    humi = _f(RE_HUMI.search(body))
    if temp is None:
        temp = _f(RE_TEMP_EN.search(body))
    if humi is None:
        humi = _f(RE_HUMI_EN.search(body))

    dist = _f(RE_DIST.search(body))

    pm = RE_PRESENCE.search(body)
    presence = None
    if pm:
        presence = 1 if pm.group(1) == "有" else 0

    valid = any(v is not None for v in (temp, humi, dist, presence))
    return {"temp": temp, "humi": humi, "distance": dist,
            "presence": presence, "valid": valid, "raw": raw}


# ---------------------------------------------------------------- 阈值判定

_OPS = {
    ">":  lambda a, b: a > b,
    ">=": lambda a, b: a >= b,
    "<":  lambda a, b: a < b,
    "<=": lambda a, b: a <= b,
    "==": lambda a, b: a == b,
    "!=": lambda a, b: a != b,
}

SEVERITY_ORDER = {"info": 0, "warn": 1, "error": 2}


def evaluate_thresholds(parsed, th_cfg, last_fired, now_epoch, prev=None):
    """按 config.json 的 thresholds.rules 判断这条读数触发了哪些异常。

    参数
      parsed      —— parse_sensor 的返回值
      th_cfg      —— config['thresholds'] 整段
      last_fired  —— {code: epoch} 的**可变字典**，调用方持有；命中时会就地更新
      now_epoch   —— 当前 unix 秒
      prev        —— **上一条**读数（parse_sensor 的返回值），给 on_change 规则用；
                     没有上一条就传 None

    返回触发的规则列表 [{'code','label','severity','field','op','value','actual'}]

     两个降噪机制，缺一个都会把异常页变成一堵墙：
      1) cooldown_sec —— 同一条规则多少秒内只报一次。温度持续偏高时，
         每来一条上报都报一次的话，几分钟就能灌满 events 表。
      2) on_change —— 规则带这个字段时，只在**状态翻转**的那一次触发。
         人员判定必须用它：`人:有` 是个持续状态，不由它的话人在屋里待一小时
         就是 60 条"检测到人"，全是噪声。
    """
    if not th_cfg or not th_cfg.get("enabled", False):
        return []
    if not parsed or not parsed.get("valid"):
        return []

    try:
        cooldown = int(th_cfg.get("cooldown_sec", 60) or 0)
    except (TypeError, ValueError):
        cooldown = 60

    fired = []
    for rule in (th_cfg.get("rules") or []):
        if not isinstance(rule, dict):
            continue
        field = str(rule.get("field", "") or "").strip()
        code = str(rule.get("code", "") or "").strip()
        if not field or not code:
            continue
        actual = parsed.get(field)
        if actual is None:
            continue
        # on_change：和上一条一样就跳过（没翻转）
        if rule.get("on_change") and prev is not None \
                and prev.get(field) == actual:
            continue
        op = str(rule.get("op", "") or "").strip()
        fn = _OPS.get(op)
        if fn is None:
            continue
        try:
            target = float(rule.get("value"))
        except (TypeError, ValueError):
            continue
        try:
            hit = fn(float(actual), target)
        except (TypeError, ValueError):
            continue
        if not hit:
            continue
        # 冷却：同一条规则在冷却期内不再重复报
        prev_at = last_fired.get(code)
        if prev_at is not None and cooldown > 0 and (now_epoch - prev_at) < cooldown:
            continue
        last_fired[code] = now_epoch
        fired.append({
            "code": code,
            "label": str(rule.get("label", "") or code),
            "severity": str(rule.get("severity", "info") or "info"),
            "field": field,
            "op": op,
            "value": target,
            "actual": actual,
        })
    return fired


def _tidy_num(v):
    """32.0 → '32'，28.5 → '28.5'。给人看的数字不该拖着 .0。"""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return str(v)
    return str(int(f)) if f == int(f) else str(round(f, 2))


def format_alert_message(rule, parsed):
    """把一条触发的规则变成给人看的一句话。"""
    field = rule.get("field")
    if field == "presence":
        # 布尔量显示成 1/0 没人看得懂，直接说"有人/无人"
        return "%s（%s）" % (
            rule.get("label", rule.get("code", "")),
            "有人" if rule.get("actual") == 1 else "无人")
    unit = {"temp": "°C", "humi": "%", "distance": "cm"}.get(field, "")
    return "%s：当前 %s%s（阈值 %s %s%s）" % (
        rule.get("label", rule.get("code", "")),
        _tidy_num(rule.get("actual")), unit,
        rule.get("op", ""), _tidy_num(rule.get("value")), unit)


# ---------------------------------------------------------------- 老数据归类

# 回填 decision_log.txt 时用：老数据没有 code/severity 字段，只能靠关键词归类。
#  这是**补历史**用的启发式，不是协议的一部分。新数据请走 thresholds + alert 字段。
_LEGACY_RULES = (
    (("人回来了", "有人", "人来了"), "PRESENCE_ON", "检测到人（历史数据）"),
    (("人出门", "人走了", "无人", "没人"), "PRESENCE_OFF", "人离开（历史数据）"),
    (("不是目标用户", "陌生人", "不认识"), "FACE_UNKNOWN", "非目标用户（历史数据）"),
    (("温度", "过热", "太热", "热了"), "TEMP_HIGH", "温度相关（历史数据）"),
    (("冷", "温度低"), "TEMP_LOW", "温度偏低（历史数据）"),
    (("湿度", "太潮", "干燥"), "HUMI_ANOMALY", "湿度相关（历史数据）"),
    (("[ESP]", "F407 ready"), "DEVICE_NOTE", "设备侧消息（历史数据）"),
    (("[定时唤醒]",), "AUTO_TICK", "定时唤醒（历史数据）"),
)


def match_legacy_alert(user_text):
    """老 decision_log 里的 user 文本 → [(code, label)]。没有匹配返回 []。"""
    s = str(user_text or "")
    if not s:
        return []
    if s.strip().startswith("[定时唤醒]"):
        return []      # 定时唤醒是"AI 主动说话"，算互动不算异常
    out = []
    for keys, code, label in _LEGACY_RULES:
        if any(k in s for k in keys):
            out.append((code, label))
    return out


# ---------------------------------------------------------------- moves

def normalize_moves(decision):
    """从一条 AI 决策里取出规整的 moves 列表（畸形项直接丢，不抛异常）。"""
    if not isinstance(decision, dict):
        return []
    moves = decision.get("moves")
    if not isinstance(moves, list):
        return []
    out = []
    for m in moves:
        if isinstance(m, dict) and m.get("type"):
            out.append(m)
    return out


def move_type_counts(decisions):
    """统计一批决策里各 move 类型出现次数，给"用户需求统计"用。"""
    counts = {}
    for d in decisions or []:
        for m in normalize_moves(d):
            t = str(m.get("type", "") or "?")
            counts[t] = counts.get(t, 0) + 1
    return counts
