# -*- coding: utf-8 -*-
"""常驻语音识别（faster-whisper）—— 给 Flask 中转站用的那一个识别器。

为什么要常驻：
    `语音识别/语音识别.py` 是给人手工双击用的，每跑一次都要重新加载模型。
    接进自动链路后，每条语音都白等那一次加载（首次十几秒）。这里把模型缓存在
    进程里，**只加载一次**，之后每次识别只有识别本身的开销。

★ 四条约束都是实测踩出来的，改之前先看清楚：
  1. `HF_ENDPOINT` 必须在 `import faster_whisper` **之前**设好 —— huggingface.co
     在本机不通，走 hf-mirror。抄自 `语音识别/语音识别.py:37-38`。
  2. `download_root` 必须指向 `语音识别/models` —— 那里已经有下好的 small（约 464 MB）。
     **换一个目录 = 它会去 huggingface.co 重新下，而下不动。**
  3. 使用 small / int8 / zh / beam5 / VAD。空结果直接报告无人声；不关 VAD
     强制重试，避免把安静录音识别成提示词（2026-09-21 实际录音回归）。
  4. 识别是 CPU 重活，用锁串行化 —— 别让两个会话同时挤进来抢。

单独自测（不启服务器，双击不了就跑这条）：
    D:\\Python\\python.exe flask_relay\\asr.py "语音识别\\wav\\测试_你好我是机器人.wav"
"""

import os
import sys
import threading
import time

# ---- ★ 必须赶在 import faster_whisper 之前设好 ----
os.environ.setdefault("HF_ENDPOINT", "https://hf-mirror.com")
os.environ["HF_HUB_DISABLE_SYMLINKS_WARNING"] = "1"

try:                       # 控制台是 GBK 时，中文 print 不至于把进程炸掉
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# ---- 路径 / 参数 ----
_HERE = os.path.dirname(os.path.abspath(__file__))
from pathlib import Path as _Path
_PROJ = str(next((p for p in _Path(_HERE).parents if (p / 'AGENTS.md').is_file()), _Path(_HERE).parent))
MODELS_DIR = os.environ.get("ASR_MODELS_DIR") or os.path.join(_PROJ, "语音识别", "models")
WAV_DIR = os.path.join(_PROJ, "语音识别", "wav")

MODEL_NAME = "small"        # 与 语音识别.py::DEFAULT_MODEL 一致
COMPUTE_TYPE = "int8"       # CPU 上 int8 最省，实测够用
DEVICE = "cpu"
# ★ 为什么要有这句提示词：Whisper 识别中文常吐繁体，一段简体 initial_prompt 就能掰回来。
#   原文与理由见 语音识别/语音识别.py:47-52 —— 别删。
ZH_PROMPT = "以下是普通话的句子，请用简体中文转写。"

# ---- 进程内的单例 ----
_lock = threading.Lock()        # 串行化 transcribe（CPU 重活）
_state_lock = threading.Lock()  # 只保护下面这几个计数器的读
_model = None
_load_error = None
_load_sec = None
_loaded_at = None
_calls = 0
_last_sec = None
_warmup_started = False
_loading = False
_error_code = None


def model_status():
    """不加载模型，只报当前状态。给 /api/audio/status 用。"""
    with _state_lock:
        return {
            "loaded": _model is not None,
            "name": MODEL_NAME,
            "compute": COMPUTE_TYPE,
            "device": DEVICE,
            "load_sec": _load_sec,
            "loaded_at": _loaded_at,
            "error": _load_error,
            "error_code": _error_code,
            "state": "ready" if _model is not None else "error" if _load_error else "loading" if _loading else "idle",
            "calls": _calls,
            "last_sec": _last_sec,
            "models_dir": MODELS_DIR,
            "models_dir_exists": os.path.isdir(MODELS_DIR),
        }


def _get_model():
    """加载一次、之后复用。失败不抛给调用方 —— 记在 _load_error 里，由状态接口暴露。"""
    global _model, _load_error, _load_sec, _loaded_at, _loading, _error_code
    with _state_lock:
        if _model is not None:
            return _model
        if _load_error is not None:
            return None
    with _lock:                     # 加载也串行，避免两个线程同时下/同时读同一份权重
        with _state_lock:
            if _model is not None:
                return _model
            if _load_error is not None:
                return None
        t0 = time.time()
        with _state_lock:
            _loading = True
        try:
            from faster_whisper import WhisperModel
            model = WhisperModel(MODEL_NAME, device=DEVICE,
                                 compute_type=COMPUTE_TYPE,
                                 download_root=MODELS_DIR)
        except Exception as e:
            # ★ 只记类型 + 一句话，别把整条栈或路径里的敏感东西打进日志
            err = "%s: %s" % (type(e).__name__, str(e)[:200])
            with _state_lock:
                _load_error = err
                _error_code = "dependency_missing" if isinstance(e, ImportError) else "model_load_failed"
                _loading = False
            print("[ASR][ERROR] 模型加载失败：%s" % err)
            return None
        sec = round(time.time() - t0, 2)
        with _state_lock:
            _model = model
            _load_sec = sec
            _loaded_at = time.strftime("%Y-%m-%d %H:%M:%S")
            _load_error = None
            _error_code = None
            _loading = False
        print("[ASR] 模型 %s 就绪（%.1f 秒）—— 之后每次识别不再重复加载" % (MODEL_NAME, sec))
        return _model


def warmup():
    """可选：起个后台线程先把模型加载好，第一条语音不用等。

    ★ **必须幂等** —— app.py 是在 `@app.before_request` 里调它的（每个请求都走一遍），
    少了这个 guard 就会变成"每个请求起一个线程"。
    """
    global _warmup_started
    with _state_lock:
        if _warmup_started:
            return None
        _warmup_started = True
    t = threading.Thread(target=_get_model, name="asr-warmup", daemon=True)
    t.start()
    return t


def transcribe(wav_path):
    """识别一个 wav。返回 dict；失败时 'error' 非空、'text' 为空。

    返回：{text, dur, lang, lang_prob, vad, sec, error}
    """
    global _calls, _last_sec
    if not wav_path or not os.path.isfile(wav_path):
        return {"text": "", "error": "文件不存在: %s" % wav_path, "error_code": "file_missing"}

    model = _get_model()
    if model is None:
        with _state_lock:
            err = _load_error or "模型不可用"
            code = _error_code or "model_unavailable"
        return {"text": "", "error": err, "error_code": code}

    t0 = time.time()
    try:
        with _lock:
            # 自动过滤静音段。空结果保留为空，交由页面提示试听、靠近麦克风重录。
            segs, info = model.transcribe(wav_path, language="zh", beam_size=5,
                                          vad_filter=True, initial_prompt=ZH_PROMPT)
            text = "".join(s.text for s in segs).strip()
            used_vad = True
            # No forced retry without VAD: on the actual quiet microphone files
            # that produced the prompt itself as a fabricated transcription.
    except Exception as e:
        return {"text": "", "error": "识别失败 %s: %s" % (type(e).__name__, str(e)[:200]),
                "error_code": "transcription_failed"}

    sec = round(time.time() - t0, 2)
    with _state_lock:
        _calls += 1
        _last_sec = sec
    return {
        "text": text,
        "dur": round(getattr(info, "duration", 0.0) or 0.0, 2),
        "lang": getattr(info, "language", ""),
        "lang_prob": round(getattr(info, "language_probability", 0.0) or 0.0, 3),
        "vad": used_vad,
        "sec": sec,
        "error": None if text else "未检测到清晰人声。请试听录音，靠近麦克风后重录。",
        "error_code": None if text else "no_speech",
    }


if __name__ == "__main__":
    wav = sys.argv[1] if len(sys.argv) > 1 else None
    if not wav:
        # 没给参数就挑个最新的 wav 试（跟 语音识别.py 的 newest_wav 同一个思路）
        import glob
        cands = []
        for d in (WAV_DIR, os.path.join(_PROJ, "ST32", "AIAssistant", "Tools", "micrec")):
            if os.path.isdir(d):
                cands += glob.glob(os.path.join(d, "*.wav"))
        if not cands:
            print("没找到 wav，用法：asr.py <某个.wav>")
            sys.exit(2)
        wav = max(cands, key=os.path.getmtime)
        print("（没给参数，用最新的那个）")

    print("模型目录: %s" % MODELS_DIR)
    print("wav     : %s" % wav)
    r = transcribe(wav)
    print("状态    : %s" % model_status())
    if r.get("error"):
        print("★ 失败：%s" % r["error"])
        sys.exit(1)
    print("识别耗时: %s 秒   音频时长: %s 秒   VAD: %s" % (r["sec"], r["dur"], r["vad"]))
    print("文字    : %s" % r["text"])
