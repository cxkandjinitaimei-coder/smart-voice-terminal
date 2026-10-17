"""download_asr_model.py：显式下载并验证 faster-whisper small 模型，可断点续传。

使用：python download_asr_model.py；可用 --models-dir/--endpoint 指定目录或镜像，成功输出 MODEL_READY。
时序与边界：网络重试有上限，文件和缓存保留；验证真正解码与 VAD，不仅 import。不会部署网页或重启服务。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
import argparse
import os
from pathlib import Path
import tempfile
import time
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models-dir", default=str(Path(__file__).resolve().parent / "models" / "asr"))
    parser.add_argument("--endpoint", default="https://hf-mirror.com")
    parser.add_argument("--attempts", type=int, default=6)
    args = parser.parse_args()
    if not 1 <= args.attempts <= 20:
        parser.error("--attempts must be 1..20")
    os.environ["HF_ENDPOINT"] = args.endpoint
    # Use ordinary resumable HTTP downloads, one file at a time. The deployment
    # may otherwise use the hub's concurrent or Xet transport on an unstable link.
    os.environ["HF_HUB_DISABLE_XET"] = "1"
    os.environ.setdefault("HF_HUB_DOWNLOAD_TIMEOUT", "120")
    os.environ.setdefault("HF_HUB_ETAG_TIMEOUT", "30")
    from huggingface_hub import snapshot_download
    from faster_whisper import WhisperModel

    model_dir = Path(args.models_dir).expanduser().resolve()
    model_dir.mkdir(parents=True, exist_ok=True)
    print(f"Cache: {model_dir}; endpoint: {args.endpoint}", flush=True)
    print("Reusing cached files and partial downloads; SSH can be closed when run under nohup.", flush=True)
    for attempt in range(1, args.attempts + 1):
        print(f"DOWNLOAD_ATTEMPT {attempt}/{args.attempts}", flush=True)
        try:
            local_path = snapshot_download(
                "Systran/faster-whisper-small", cache_dir=str(model_dir), max_workers=1,
                allow_patterns=["config.json", "model.bin", "tokenizer.json",
                                "vocabulary.*", "preprocessor_config.json"],
            )
            break
        except Exception as exc:
            # Keep the useful network reason but do not dump environment or credentials.
            message = str(exc).replace("\n", " ")[:400]
            print(f"DOWNLOAD_RETRY: {type(exc).__name__}: {message}", flush=True)
            if attempt == args.attempts:
                print("MODEL_NOT_READY: downloads are kept; rerun to resume.", flush=True)
                return 1
            time.sleep(min(10 * attempt, 60))

    print("DOWNLOAD_FINISHED; loading small / CPU / int8 locally...", flush=True)
    model = WhisperModel(local_path, device="cpu", compute_type="int8", local_files_only=True)
    with tempfile.TemporaryDirectory(prefix="asr-model-check-") as temp:
        silent = Path(temp) / "silence.wav"
        with wave.open(str(silent), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(b"\0\0" * 16000)
        segments, _ = model.transcribe(str(silent), language="zh", vad_filter=True)
        list(segments)  # Evaluation is lazy: actually decode, not just create a generator.
    print("MODEL_READY: small / CPU / int8 loaded; WAV/VAD check passed.", flush=True)
    print("Model is cached. Start/restart your Robot_Server and check /api/audio/status.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
