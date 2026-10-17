"""device_controls.py：麦克风 VAD 与舵机全局速度的参数保存、修订计算和应用确认。

使用：save 校验并原子替换配置；get 提供 revision，ESP 重发直到 F407 回报匹配修订。
时序与边界：保存到网页不等于板端应用；applied 需要新鲜 ACK。VAD 毫秒值按 DMA 半块粒度量化。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
import copy
import json
import os
import tempfile
import threading
import time
import zlib

PATH = os.path.join(os.path.dirname(__file__), "device_controls.json")
_lock = threading.Lock()
_acks = {}
DEFAULT = {"enabled": True, "minimum": 900, "margin": 800,
           "onset_ms": 160, "silence_ms": 800, "servo_speed": 90}


def _load():
    try:
        with open(PATH, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def validate(data):
    if not isinstance(data, dict) or not isinstance(data.get("enabled"), bool):
        raise ValueError("自动录音开关必须是布尔值")
    result = {"enabled": data["enabled"]}
    for key, lo, hi in [("minimum", 50, 8000), ("margin", 0, 8000),
                        ("onset_ms", 64, 640), ("silence_ms", 200, 2000),
                        ("servo_speed", 0, 360)]:
        value = data.get(key)
        if isinstance(value, bool) or not isinstance(value, int) or not lo <= value <= hi:
            raise ValueError("%s 必须是 %d~%d 的整数" % (key, lo, hi))
        # A DMA half is approximately 32ms; show the actual quantized setting.
        result[key] = min((hi // 32) * 32, ((value + 31) // 32) * 32) if key.endswith("_ms") else value
    return result


def get(device):
    with _lock:
        data = _load().get(device, copy.deepcopy(DEFAULT))
        try:
            data = validate(data)
        except ValueError:
            data = copy.deepcopy(DEFAULT)
        revision = zlib.crc32(json.dumps(data, sort_keys=True).encode()) or 1
        ack, seen = _acks.get(device, (0, 0))
    return dict(data, revision=revision, applied=ack == revision and time.time() - seen < 15)


def save(device, data):
    data = validate(data)
    with _lock:
        settings = _load()
        settings[device] = data
        parent = os.path.dirname(os.path.abspath(PATH))
        fd, temporary = tempfile.mkstemp(prefix="controls-", suffix=".json", dir=parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(settings, f, ensure_ascii=False, indent=2)
            os.replace(temporary, PATH)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    return get(device)


def confirm(device, revision):
    if isinstance(revision, int) and not isinstance(revision, bool) and 0 <= revision <= 0xFFFFFFFF:
        with _lock:
            _acks[device] = (revision, time.time())
