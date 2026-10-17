"""test_audio_transport_status.py：使用真实服务模块的离线回归。

使用：由 tools/run_checks.py 在临时服务副本中单独运行；不连接硬件、不调用真实 AI。
范围：断言涵盖本文件列出的状态/边界；测试通过不代表飞线信号、供电和公网时延已验证。
依赖：server/Robot_Server/requirements.txt；数据由测试临时创建并清理。
"""
import json
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

import app as A

sys.stdout.reconfigure(encoding="utf-8")
with tempfile.TemporaryDirectory(prefix="upload-status-") as temporary:
    root = Path(temporary)
    with patch.object(A, "start_auto_tick"), patch.object(A, "start_store"), patch.object(A, "start_beacon"), \
         patch.object(A.asr, "warmup"), patch.object(A, "load_config", return_value={"token": "upload-test-only"}), \
         patch.object(A, "append_line"), patch.object(A.store, "record_sensor"), \
         patch.object(A.device_controls, "confirm"), patch.object(A.device_controls, "get", return_value=dict(A.device_controls.DEFAULT, revision=1)), \
         patch.object(A, "_audio_transport_status", {}), patch.object(A.audio_rx, "WAV_DIR", str(root)):
        client = A.app.test_client()
        assert client.get("/api/audio/status").status_code == 401
        payload = {"token": "upload-test-only", "device_id": "test-esp", "need_ai": False,
                   "audio_transport": {"version": "1.1.6", "phase": "failed", "accepted": 24576,
                                       "stored": 24576, "sent": 20480, "total": 319488,
                                       "error": "MCU reported lost microphone samples", "uart_overruns": 1,
                                       "private": "not-to-be-reflected"}}
        response = client.post("/api/data", json=payload)
        assert response.status_code == 200
        with client.session_transaction() as session: session["authed"] = True
        report = client.get("/api/audio/status").json["transport"]["test-esp"]
        assert report["accepted"] == 24576 and report["sent"] == 20480 and report["uart_overruns"] == 1
        assert report["error"] == "MCU reported lost microphone samples" and "private" not in report
        # Browser-originated simulation must not overwrite real hardware reports.
        payload["no_pop"] = True; payload["audio_transport"]["accepted"] = 0
        assert client.post("/api/data", json=payload).status_code == 200
        assert client.get("/api/audio/status").json["transport"]["test-esp"]["accepted"] == 24576
        payload["no_pop"] = False; payload["token"] = "invalid"
        assert A.app.test_client().post("/api/data", json=payload).status_code == 401
print("PASS: authenticated no-UART ESP failure diagnostics, byte counts, browser isolation, safe fields and auth")
