"""test_audio_queue.py：使用真实服务模块的离线回归。

使用：由 tools/run_checks.py 在临时服务副本中单独运行；不连接硬件、不调用真实 AI。
范围：断言涵盖本文件列出的状态/边界；测试通过不代表飞线信号、供电和公网时延已验证。
依赖：server/Robot_Server/requirements.txt；数据由测试临时创建并清理。
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

import app as A
import audio_archive
import audio_rx as R
import device_controls as C

sys.stdout.reconfigure(encoding="utf-8")


def wait_for(predicate, timeout=5):
    end = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < end, "condition timed out"
        time.sleep(.02)


def main():
    with tempfile.TemporaryDirectory(prefix="audio-queue-", ignore_cleanup_errors=True) as temporary:
        directory = Path(temporary)
        R.WAV_DIR = str(directory / "wav")
        R._STATE_FILE = str(directory / "audio_state.json")
        R.audio_retention.PATH = str(directory / "audio_retention.json")
        R._mode = "manual"
        C.PATH = str(directory / "controls.json")
        A.start_auto_tick = A.start_store = A.start_beacon = lambda: None
        A.asr.warmup = lambda: None
        A.store.record_command = lambda *args, **kwargs: None
        A.load_config = lambda: {"token": "queue-test-only", "auto_tick": {"enabled": False}}
        started, release = threading.Event(), threading.Event()
        calls, sent = [], []
        def transcribe(path):
            calls.append(path)
            if len(calls) == 1:
                started.set()
                assert release.wait(5)
            return {"text": Path(path).stem, "sec": 0, "vad": True}
        R.asr.transcribe = transcribe
        R.inject = lambda text, cfg: (sent.append(text) is None, "fixture")
        ids, files = [], []
        for sid in ("repeat", "second", "repeat"):
            session, error = R.open_stream(sid, total=4096)
            assert not error
            assert session.append_raw(b"\x01\x00" * 2048)["ok"]
            assert R.finish(sid)["ok"]
            ids.append(session.record_id)
            files.append(session.path)
            assert started.wait(1)
        assert len(calls) == 1 and R.status()["processing_queue"] == 2
        assert len(set(files)) == 3, "ESP reboot/session reuse must not overwrite WAVs"
        release.set()
        wait_for(lambda: R.status()["pending_count"] == 3)
        assert calls == files, "ASR must run in capture order"
        assert R.status()["last"]["id"] == ids[-1], "Older worker must not overwrite newest result"
        assert R.status()["history_total"] == 3, "New recording must not be imported twice"
        assert [r["id"] for r in audio_archive.pending(R.WAV_DIR)] == ids
        # A fresh process can read history and pending FIFO without private globals.
        code = "import sys;import audio_rx as R;R.WAV_DIR=sys.argv[1];s=R.status();assert s['history_total']==3 and s['pending_count']==3;print('restart retained 3')"
        result = subprocess.run([sys.executable, "-c", code, R.WAV_DIR], cwd=Path(__file__).parent,
                                capture_output=True, text=True, check=True, timeout=10)
        assert "restart retained 3" in result.stdout
        assert R.queue_pending(None, ids[0])["ok"]
        wait_for(lambda: not R.status()["sending"])
        assert sent == [Path(files[0]).stem] and R.status()["pending_count"] == 2
        assert R.drop_pending(ids[1])["dropped"] and os.path.isfile(files[1])
        assert R.status()["pending_count"] == 1
        client = A.app.test_client()
        assert client.get("/api/audio/history").status_code == 401
        assert client.post("/api/device/controls", json={}).status_code == 401
        with client.session_transaction() as session:
            session["authed"] = True
        for record_id in ids:
            assert client.get("/api/audio/latest.wav?id=" + record_id).status_code == 200
        assert client.get("/api/audio/latest.wav?id=does-not-exist").status_code == 404
        assert client.get("/api/audio/history").json["total"] == 3
        controls = dict(C.DEFAULT, minimum=700)
        assert client.post("/api/device/controls", json={"controls": controls}).status_code == 200
        revision = C.get("esp-bridge")["revision"]
        assert not C.get("esp-bridge")["applied"]
        C.confirm("esp-bridge", revision)
        assert C.get("esp-bridge")["applied"]
        assert client.post("/api/device/controls", json={"controls": dict(controls, minimum=-1)}).status_code == 400
        assert A.decision_to_raw({"moves": [{"type": "servo", "id": 2, "angle": 90, "speed": 30}]}) == "@MOVE:servo,2,90,30"
        print("PASS: persistent FIFO, 3 recordings/reused sess, old result retained, selected send/drop/play, auth, controls persistence/ACK/range, speed frame")


if __name__ == '__main__':
    main()
