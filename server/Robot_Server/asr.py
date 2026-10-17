"""asr.py：常驻 CPU faster-whisper 语音识别与可观测加载状态。

使用：transcribe(wav_path) 返回 text/error；warmup 幂等预热；模型目录默认本服务 models/asr，可用 ASR_MODELS_DIR 覆盖。
时序与边界：首次使用前单独下载模型；模型和推理共用串行锁，状态锁仅保护计数。VAD 后为空就报告无人声，不关闭 VAD 强行重试。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
# -*- coding: utf-8 -*-

import os
import sys
import threading
import time

# ----  必须赶在 import faster_whisper 之前设好 ----
os.environ.setdefault("HF_ENDPOINT", "https://hf-mirror.com")
os.environ["HF_HUB_DISABLE_SYMLINKS_WARNING"] = "1"

try:                       # 控制台是 GBK 时，中文 print 不至于把进程异常退出
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# ---- 路径 / 参数 ----
_HERE = os.path.dirname(os.path.abspath(__file__))
MODELS_DIR = os.environ.get("ASR_MODELS_DIR") or os.path.join(_HERE, "models", "asr")
WAV_DIR = os.path.join(_HERE, "recordings")

MODEL_NAME = "small"        # 默认本地识别模型，下载器与检查器使用同一配置
COMPUTE_TYPE = "int8"       # CPU 上 int8 最省，实测够用
DEVICE = "cpu"
#  为什么要有这句提示词：Whisper 识别中文常吐繁体，一段简体 initial_prompt 就能引导为简体。
#   空录音不取消 VAD 重试，以免提示词被误识别为用户语音。
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
    """双重检查后只加载一次本地模型。
    
    状态锁不覆盖模型加载；加载锁串行创建及推理。失败写 error_code，不在请求线程下载权重。"""
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
                                 download_root=MODELS_DIR, local_files_only=True)
        except Exception as e:
            #  只记类型 + 一句话，不要将整条栈或路径里的敏感东西打进日志
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
    """幂等地启动一次后台预热。
    
    每个请求都可能调用此函数，必须在状态锁内检查/置位，避免重复起线程和争抢模型。"""
    global _warmup_started
    with _state_lock:
        if _warmup_started:
            return None
        _warmup_started = True
    t = threading.Thread(target=_get_model, name="asr-warmup", daemon=True)
    t.start()
    return t


def transcribe(wav_path):
    """识别完整 WAV，返回 text/dur/lang/vad/sec/error/error_code。
    
    锁覆盖惰性 segments 的实际求值；无清晰人声返回 no_speech，不靠取消 VAD 强造文本。"""
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
        # 未指定文件时，仅查本服务录音目录中的最新 WAV
        import glob
        cands = []
        for d in (WAV_DIR,):
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
