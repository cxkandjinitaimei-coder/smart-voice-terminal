"""ASR dependency/model/service readiness regression; no downloads or real audio."""
import importlib
import json
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import asr
import asr_check


class ASRChecks(unittest.TestCase):
    def setUp(self):
        self.module = importlib.reload(asr)
        self.temporary = tempfile.TemporaryDirectory(prefix="asr-ready-test-")
        self.directory = Path(self.temporary.name)
        self.wav = self.directory / "fixture.wav"
        self.wav.write_bytes(b"fixture")

    def tearDown(self):
        self.temporary.cleanup()

    def test_missing_dependency_is_not_no_speech(self):
        with patch.dict(sys.modules, {"faster_whisper": None}):
            result = self.module.transcribe(str(self.wav))
        self.assertEqual(result["error_code"], "dependency_missing")
        self.assertEqual(self.module.model_status()["state"], "error")
        self.assertFalse(self.module.model_status()["loaded"])

    def test_model_download_failure(self):
        def broken(*args, **kwargs):
            raise OSError("model download failed")
        with patch.dict(sys.modules, {"faster_whisper": SimpleNamespace(WhisperModel=broken)}):
            result = self.module.transcribe(str(self.wav))
        self.assertEqual(result["error_code"], "model_load_failed")

    def test_model_reused_and_silence_stays_empty(self):
        calls = []
        class Model:
            def __init__(inner, name, **kwargs):
                calls.append((name, kwargs))
            def transcribe(inner, filename, **kwargs):
                self.assertTrue(kwargs["vad_filter"])
                return [], SimpleNamespace(duration=1)
        with patch.dict(sys.modules, {"faster_whisper": SimpleNamespace(WhisperModel=Model)}):
            first = self.module.transcribe(str(self.wav))
            second = self.module.transcribe(str(self.wav))
        self.assertEqual(len(calls), 1)
        self.assertEqual(first["error_code"], "no_speech")
        self.assertEqual(second["text"], "")
        self.assertEqual(self.module.model_status()["state"], "ready")

    def test_prepare_reuses_cloud_model_directory_and_decoder(self):
        expected = self.directory / "语音识别" / "models"
        calls = []
        class Model:
            def __init__(inner, name, **kwargs):
                self.assertEqual(Path(kwargs["download_root"]), expected)
            def transcribe(inner, filename, **kwargs):
                import wave
                with wave.open(filename) as wav:
                    self.assertEqual((wav.getframerate(), wav.getnchannels(), wav.getsampwidth()), (16000, 1, 2))
                    self.assertEqual(wav.getnframes(), 16000)
                calls.append(filename)
                return [], SimpleNamespace(duration=1)
        with patch.dict(sys.modules, {"faster_whisper": SimpleNamespace(WhisperModel=Model)}), \
             patch.object(asr_check, "dependencies", return_value=0) as decoder:
            self.assertEqual(asr_check.prepare(str(self.directory / "flask_relay")), 0)
        decoder.assert_called_once()
        self.assertEqual(len(calls), 1)
        self.assertFalse(Path(calls[0]).exists(), "Temporary probe must be removed")

    def test_broken_decoder_never_loads_or_downloads_model(self):
        with patch.object(asr_check, "dependencies", return_value=1), \
             patch.object(self.module, "_get_model") as get_model:
            self.assertEqual(asr_check.prepare(str(self.directory / "flask_relay")), 1)
        get_model.assert_not_called()

    def test_service_requires_loaded_model(self):
        (self.directory / "config.json").write_text(json.dumps({"token": "isolated-only"}))
        class Response:
            def __init__(inner, body): inner.body = json.dumps(body).encode()
            def __enter__(inner): return inner
            def __exit__(inner, *args): pass
            def read(inner, *args): return inner.body
        class Opener:
            def __init__(inner, model): inner.model = model; inner.calls = []
            def open(inner, request, **kwargs):
                inner.calls.append(request)
                return Response({} if not isinstance(request, str) else {"model": inner.model})
        for model, result in [({"loaded": False, "error": "missing component"}, 1),
                              ({"loaded": True, "name": "small"}, 0),
                              ({"loaded": False}, 1)]:
            opener = Opener(model)
            with patch("urllib.request.build_opener", return_value=opener):
                self.assertEqual(asr_check.service(str(self.directory), "http://127.0.0.1:5000", wait_seconds=0), result)
            self.assertEqual(len(opener.calls), 2)
            self.assertTrue(opener.calls[1].endswith("/api/audio/status"))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    unittest.main()
