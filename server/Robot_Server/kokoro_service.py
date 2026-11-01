"""复用现有 Kokoro 工具的本机服务，不改它的源码或下载模型。

使用已有 kokoro-tts/.venv/Scripts/python.exe -B 运行本文件，传入
--config 本网页config.json。先预热模型，再监听127.0.0.1:8766；
网页以原设备token认证调用/speech。输出硬件标准PCM WAV。
--self-test 文件.wav 可只合成测试音频，不启动HTTP或播放电脑扬声器。
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import sys
import threading
import time

from tts_audio import pcm_wav


def load_engine(config):
    """只接受完整已有安装；禁用在线模型下载和外部源码的pycache写入。"""
    settings = config.get("tts") or {}
    root = Path(settings["kokoro_dir"]).resolve()
    for relative in ("tts_api.py", "tts_core.py", "models/kokoro-v1.0.onnx", "models/voices-v1.0.bin"):
        if not (root / relative).is_file():
            raise RuntimeError("已有Kokoro安装缺少：" + relative)
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(root))
    import tts_api
    # 原工具会在目录缺失时复制espeak数据；本桥不在外部目录创建数据。
    if not Path(tts_api.ESPEAK_DATA).is_dir():
        raise RuntimeError("原工具的espeak数据目录缺失，请先用原工具完成初始化")
    tts_api.get_engine()
    return tts_api


def synthesize(engine, config, text):
    """始终走原工具的断句接口，避免长句音素被模型截断。"""
    settings = config.get("tts") or {}
    speed = float(settings.get("speed", 1.0))
    if not 0.5 <= speed <= 2.0:
        raise ValueError("语速需在0.5~2.0之间")
    audio, rate = engine.speak_long(text, voice=settings.get("voice", "zf_xiaoxiao"), speed=speed)
    return pcm_wav(audio, rate)


def make_server(engine, config, port=8766):
    token = str(config.get("token") or "")
    if not token:
        raise ValueError("config.json缺少设备token")
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass  # 不将认证信息或用户文本写入HTTP访问日志。

        def respond(self, status, data, kind="application/json; charset=utf-8"):
            self.send_response(status)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
            self.close_connection = True

        def do_GET(self):
            if self.path == "/health" and secrets.compare_digest(
                    self.headers.get("Authorization", "").encode(), ("Bearer " + token).encode()):
                self.respond(200, b'{"ok":true,"service":"kokoro-f407-v1","rate":16000}')
            else:
                self.respond(401, b'{"ok":false}')

        def do_POST(self):
            self.connection.settimeout(10)
            if self.path != "/speech":
                self.respond(404, b'{"ok":false}')
                return
            if not secrets.compare_digest(self.headers.get("Authorization", "").encode(),
                                          ("Bearer " + token).encode()):
                self.respond(401, b'{"ok":false,"err":"unauthorized"}')
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 4096:
                    raise ValueError("请求长度需为1~4096字节")
                body = self.rfile.read(length)
                if len(body) != length:
                    raise ValueError("请求不完整")
                value = json.loads(body)
                text = str(value.get("text") or "").strip()
                if not text or len(text) > 240:
                    raise ValueError("文字长度需为1~240字符")
            except (ValueError, AttributeError, OSError) as exc:
                self.respond(400, json.dumps({"ok": False, "err": str(exc)}, ensure_ascii=False).encode())
                return
            # 不让大量回复排队撑爆CPU；繁忙时明确拒绝，由调用者保留文字。
            if not lock.acquire(blocking=False):
                self.respond(503, b'{"ok":false,"err":"Kokoro busy"}')
                return
            try:
                started = time.monotonic()
                wav = synthesize(engine, config, text)
                print("[KOKORO] WAV %d bytes, %.2fs" % (len(wav), time.monotonic() - started), flush=True)
                self.respond(200, wav, "audio/wav")
            except Exception as exc:
                self.respond(503, json.dumps({"ok": False, "err": str(exc)}, ensure_ascii=False).encode())
            finally:
                lock.release()

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--self-test", type=Path)
    args = parser.parse_args()
    config = json.loads(Path(args.config).read_text(encoding="utf-8-sig"))
    started = time.monotonic()
    engine = load_engine(config)
    test_wav = synthesize(engine, config, "你好，我是小D。现在使用本地语音合成。")
    print("[KOKORO] model warmed in %.2fs; PCM 16000Hz/16bit/mono" % (time.monotonic() - started), flush=True)
    if args.self_test:
        args.self_test.parent.mkdir(parents=True, exist_ok=True)
        args.self_test.write_bytes(test_wav)
        print("KOKORO_WAV_READY: " + str(args.self_test), flush=True)
        return
    with make_server(engine, config, args.port) as server:
        print("KOKORO_READY http://127.0.0.1:%d/speech" % args.port, flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
