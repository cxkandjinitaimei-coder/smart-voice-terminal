"""Cloud downloader retry/cache/local validation regression without network."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import types
from unittest.mock import patch
import wave

spec = importlib.util.spec_from_file_location("download_asr_model", Path(__file__).with_name("download_asr_model.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

with tempfile.TemporaryDirectory(prefix="asr-download-test-") as temp:
    calls = []
    consumed = []

    def download(repo, **kwargs):
        assert os.environ["HF_HUB_DISABLE_XET"] == "1"
        assert os.environ["HF_ENDPOINT"] == "https://hf-mirror.com"
        assert repo == "Systran/faster-whisper-small"
        assert Path(kwargs["cache_dir"]) == Path(temp).resolve()
        assert kwargs["max_workers"] == 1
        calls.append(kwargs)
        if len(calls) == 1:
            raise ConnectionError("socket test interruption")
        return temp

    class Model:
        def __init__(self, path, **kwargs):
            assert path == temp
            assert kwargs == dict(device="cpu", compute_type="int8", local_files_only=True)

        def transcribe(self, filename, **kwargs):
            assert kwargs == dict(language="zh", vad_filter=True)
            with wave.open(filename, "rb") as wav:
                assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()) == (1, 2, 16000, 16000)
            def segments():
                consumed.append(True)
                return
                yield
            return segments(), None

    fake_modules = {
        "huggingface_hub": types.SimpleNamespace(snapshot_download=download),
        "faster_whisper": types.SimpleNamespace(WhisperModel=Model),
    }
    with patch.dict(sys.modules, fake_modules), patch.dict(os.environ), patch.object(module.time, "sleep"), \
         patch.object(sys, "argv", ["download", "--models-dir", temp, "--attempts", "2"]):
        assert module.main() == 0
        assert len(calls) == 2 and consumed
        calls.clear()
        with patch.object(sys, "argv", ["download", "--models-dir", temp, "--attempts", "1"]):
            assert module.main() == 1
        assert len(calls) == 1
print("PASS: resumed single-worker cache download, bounded network retry, actual lazy decode check")
