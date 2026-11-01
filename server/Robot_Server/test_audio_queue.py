"""Real archive/FIFO/routes; optional headless browser. All data stays temporary."""
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


def browser_checks(directory):
    from werkzeug.serving import make_server, WSGIRequestHandler
    import requests
    from cdp_min import Browser, wait_port
    A._audio_transport_status["esp-bridge"] = {
        "version": "1.1.6", "phase": "failed", "accepted": 24576,
        "stored": 24576, "sent": 20480, "total": 319488,
        "error": "MCU reported lost microphone samples", "uart_overruns": 1,
    }
    class Quiet(WSGIRequestHandler):
        def log_request(self, *args):
            pass
    server = make_server("127.0.0.1", 0, A.app, threaded=True, request_handler=Quiet)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % server.server_port
    port = 9358
    edge = subprocess.Popen([r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
                             "--headless=new", "--disable-gpu", "--no-first-run",
                             "--remote-debugging-port=%d" % port,
                             "--user-data-dir=" + str(directory / "edge"), "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        assert wait_port(port, seconds=10)
        br = Browser(port)
        sid, _ = br.new_page()
        for name in ("Page", "Runtime", "Network"):
            br.call(name + ".enable", {}, session=sid)
        session = requests.Session()
        response = session.post(base + "/login", data={"token": "queue-test-only"})
        assert response.status_code == 200
        br.call("Network.setCookie", {"name": "session", "value": session.cookies.get("session"),
                                      "url": base, "path": "/"}, session=sid)
        br.call("Emulation.setDeviceMetricsOverride", {"width": 1100, "height": 1500, "deviceScaleFactor": 1, "mobile": False}, session=sid)
        br.call("Page.navigate", {"url": base + "/#test"}, session=sid)
        def js(code):
            return br.evaluate(code, sid)
        wait_for(lambda: js("!!document.querySelector('#audio-history button')"))
        wait_for(lambda: "ESP 1.1.6" in js("document.querySelector('#audio-transport-status').textContent"))
        assert "20480" in js("document.querySelector('#audio-transport-status').textContent")
        assert "MCU 报告麦克风数据丢失" in js("document.querySelector('#audio-transport-status').textContent")
        assert js("document.querySelectorAll('#audio-history button').length") >= 3
        js("document.querySelectorAll('#audio-history button')[1].click()")
        wait_for(lambda: js("document.querySelector('#audio-player').src.includes('id=')"))
        selected = js("document.querySelector('#audio-player').src")
        assert requests.get(selected, cookies=session.cookies).status_code == 200
        js("document.querySelector('#control-minimum').value=650; document.querySelector('#control-minimum').dispatchEvent(new Event('input',{bubbles:true})); document.querySelector('#controls-save').click()")
        wait_for(lambda: C.get("esp-bridge")["minimum"] == 650)
        assert js("document.querySelector('#controls-status').textContent").find("等待") >= 0
        C.confirm("esp-bridge", C.get("esp-bridge")["revision"])
        wait_for(lambda: "已确认" in js("document.querySelector('#controls-status').textContent"))
        js("document.querySelector('#servo-speed-1').value=30; document.querySelector('#servo-test-panel .servo-group-row button:nth-of-type(3)').click()")
        wait_for(lambda: A._queue_len("esp-bridge") > 0)
        command = A.pop_command("esp-bridge")
        assert command["raw"] == "@MOVE:servo,1,90,30#", command
        assert not br.console_errors(), br.console_errors()
        js("document.querySelector('#audio-retention-limit').value=2; document.querySelector('#audio-retention-limit').dispatchEvent(new Event('input',{bubbles:true})); document.querySelector('#audio-retention-save').click()")
        wait_for(lambda: R.audio_retention.get()["enabled"])
        wait_for(lambda: "最近 2 条" in js("document.querySelector('#audio-retention-status').textContent"))
        assert R.history()["total"] == 2
        assert js("document.querySelector('#audio-asr-status').textContent")
        js("document.querySelector('#device-controls').scrollIntoView()")
        br.screenshot(sid, str(directory / "audio_controls.png"))
        print("PASS browser: actual queue selection/WAV, saved threshold and MCU ACK label, 30deg/s command")
        return directory / "audio_controls.png"
    finally:
        try:
            br.call("Browser.close")
        except Exception:
            pass
        try:
            edge.wait(timeout=10)
        except subprocess.TimeoutExpired:
            edge.terminate()
            edge.wait(timeout=10)
        server.shutdown()


def main(browser=False, screenshot=None):
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
        if browser:
            image = browser_checks(directory)
            if screenshot:
                import shutil
                shutil.copy2(image, screenshot)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser", action="store_true")
    parser.add_argument("--screenshot")
    args = parser.parse_args()
    main(args.browser, args.screenshot)
