"""Actual SQLite/WAV retention and authenticated routes; isolated temp data."""
import json
from pathlib import Path
import sys
import tempfile
import wave
from unittest.mock import patch

import app as A
import audio_archive as archive
import audio_retention as retention
import audio_rx as R


def main():
    with tempfile.TemporaryDirectory(prefix="audio-retention-test-") as temporary:
        directory = Path(temporary)
        R.WAV_DIR = str(directory / "wav")
        Path(R.WAV_DIR).mkdir()
        retention.PATH = str(directory / "audio_retention.json")
        R._mode = "manual"
        R._last = None
        A.start_auto_tick = A.start_store = A.start_beacon = A.asr.warmup = lambda: None
        A.load_config = lambda: {"token": "retention-fixture-only"}
        def recording(name, phase="skipped", complete=True):
            filename = Path(R.WAV_DIR) / ("rec_" + name + ".wav")
            with wave.open(str(filename), "wb") as out:
                out.setnchannels(1); out.setsampwidth(2); out.setframerate(16000)
                out.writeframes(b"\x01\x00" * 1024)
            rec = {"id": name, "sess": name, "saved": str(filename), "at": "fixture",
                   "received": 2048, "total": 2048 if complete else 4096, "complete": complete,
                   "text": "hello" if phase == "pending" else "", "inject": phase}
            archive.save(R.WAV_DIR, rec)
            return filename
        oldest = recording("oldest")
        pending = recording("pending", "pending")
        latest = [recording("recent%d" % i, complete=i != 2) for i in range(3)]
        # Merely deploying, reading status or importing history must not delete.
        assert not retention.get()["enabled"] and R.status()["history_total"] == 5
        assert oldest.exists()
        client = A.app.test_client()
        assert client.post("/api/audio/retention", json={"limit": 3}).status_code == 401
        with client.session_transaction() as session: session["authed"] = True
        for value in (0, -1, 5001, True, 3.5, "3", None):
            assert client.post("/api/audio/retention", json={"limit": value}).status_code == 400
        result = client.post("/api/audio/retention", json={"limit": 3}).json
        assert result["retention"]["deleted"] == 1 and result["retention"]["protected"] == 1
        assert not oldest.exists() and pending.exists() and all(p.exists() for p in latest)
        assert archive.page(R.WAV_DIR)["total"] == 4
        assert json.loads(Path(retention.PATH).read_text())["limit"] == 3
        # Selecting a saved partial WAV must permit listening, never grant ASR completeness.
        response = client.get("/api/audio/latest.wav?id=recent2")
        assert response.status_code == 200 and response.data[:4] == b"RIFF"
        response.close()
        assert not archive.get(R.WAV_DIR, "recent2")["complete"]
        assert client.get("/api/audio/latest.wav?id=oldest").status_code == 404
        # Completing a formerly protected old item applies the saved cap.
        rec = archive.get(R.WAV_DIR, "pending"); rec["inject"] = "dropped"
        R._set_last_if_current(rec)
        assert not pending.exists() and archive.page(R.WAV_DIR)["total"] == 3
        # An archive row pointing outside WAV_DIR must never delete that file.
        external = directory / "rec_external.wav"; external.write_bytes(b"keep")
        rec = archive.get(R.WAV_DIR, "recent0"); rec["saved"] = str(external)
        archive.save(R.WAV_DIR, rec)
        output = archive.prune(R.WAV_DIR, 2)
        assert output["failed"] == 1 and external.exists()
        # If the OS refuses deletion, leave the matching archive row accessible.
        with patch("audio_archive.os.unlink", side_effect=PermissionError("locked")):
            output = archive.prune(R.WAV_DIR, 1)
        assert output["failed"] == 2 and archive.get(R.WAV_DIR, "recent1")
        print("PASS: opt-in persisted cap, oldest cleanup, protected pending/newest, selected full/partial WAV, auth/ranges, boundary and locked-file safety")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
