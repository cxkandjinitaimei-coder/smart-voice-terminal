"""Deployment checks: prepare the CPU model, then verify the running service.

No device commands or AI calls. The decoding check uses temporary silent PCM.
"""
import argparse
import http.cookiejar
import importlib.metadata
import json
import os
from pathlib import Path
import tempfile
import time
import urllib.parse
import urllib.request
import wave


def dependencies():
    """Exercise the decoder, not only imports; no model/network is needed.

    faster-whisper 1.2.1 can import and load a model with PyAV 19, but its
    first filename transcription fails at av.open(metadata_errors=...).
    """
    try:
        import av
        from faster_whisper.audio import decode_audio
        version = importlib.metadata.version("faster-whisper")
        print("ASR_DEPENDENCIES: faster-whisper=%s PyAV=%s" % (version, av.__version__), flush=True)
        with tempfile.TemporaryDirectory(prefix="asr-decoder-check-") as temporary:
            filename = os.path.join(temporary, "silence.wav")
            with wave.open(filename, "wb") as out:
                out.setnchannels(1)
                out.setsampwidth(2)
                out.setframerate(16000)
                out.writeframes(b"\0\0" * 16000)
            audio = decode_audio(filename, sampling_rate=16000)
            if len(audio) != 16000 or audio.ndim != 1 or not (audio == 0).all():
                raise RuntimeError("Decoded silent WAV has invalid length or samples")
        print("ASR_DECODER_READY: real WAV decoded at 16000 Hz.", flush=True)
        return 0
    except Exception as exc:
        print("ASR_DECODER_FAILED: %s: %s" % (type(exc).__name__, str(exc)[:200]), flush=True)
        return 1


def prepare(app_dir):
    if dependencies() != 0:
        return 1
    import asr
    # The uploaded script lives in /tmp. Cache weights at the real app's
    # existing model directory, so the restarted service reuses them.
    asr.MODELS_DIR = os.environ.get("ASR_MODELS_DIR") or os.path.join(
        os.path.dirname(os.path.realpath(app_dir)), "语音识别", "models")
    print("Preparing ASR small / CPU / int8; reusing the existing model cache.", flush=True)
    if asr._get_model() is None:
        print("ASR_NOT_READY: " + (asr.model_status().get("error") or "Model unavailable"), flush=True)
        return 1
    with tempfile.TemporaryDirectory(prefix="asr-check-") as temporary:
        filename = os.path.join(temporary, "silence.wav")
        with wave.open(filename, "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(16000)
            out.writeframes(b"\0\0" * 16000)
        result = asr.transcribe(filename)
        if result.get("error") and result.get("error_code") != "no_speech":
            print("ASR_NOT_READY: " + result["error"], flush=True)
            return 1
    print("ASR_PREPARED: model loaded, WAV decoding and VAD checked.", flush=True)
    return 0


def service(app_dir, url, wait_seconds=120):
    config = json.loads((Path(app_dir) / "config.json").read_text(encoding="utf-8-sig"))
    token = config.get("token", "")
    if not token:
        raise RuntimeError("Cloud config has no login token")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
        urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
    login = urllib.request.Request(url.rstrip("/") + "/login",
        data=urllib.parse.urlencode({"token": token}).encode(), method="POST")
    with opener.open(login, timeout=5) as response:
        response.read()
    deadline = time.monotonic() + wait_seconds
    print("Checking running service ASR readiness...", flush=True)
    while True:
        with opener.open(url.rstrip("/") + "/api/audio/status", timeout=5) as response:
            model = json.load(response).get("model") or {}
        if model.get("loaded") and model.get("name") == "small":
            print("ASR_READY: running service has loaded small / CPU / int8.", flush=True)
            return 0
        if model.get("error"):
            print("ASR_NOT_READY: " + str(model["error"])[:200], flush=True)
            return 1
        if time.monotonic() >= deadline:
            print("ASR_NOT_READY: service model load timed out.", flush=True)
            return 1
        time.sleep(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app-dir", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dependencies", action="store_true", help="Decode a WAV without loading/downloading a model")
    mode.add_argument("--prepare", action="store_true")
    mode.add_argument("--service-url")
    args = parser.parse_args()
    try:
        if args.dependencies:
            return dependencies()
        return prepare(args.app_dir) if args.prepare else service(args.app_dir, args.service_url)
    except Exception as exc:
        # Never dump config, cookies or the login request into deployment logs.
        print("ASR_CHECK_FAILED: " + type(exc).__name__, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
