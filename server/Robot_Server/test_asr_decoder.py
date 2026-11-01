"""Real faster-whisper/PyAV decoding; optional isolated PyAV 19 negative baseline.

No model download. --model-check also runs the real CPU model from the existing
cache with HF_HUB_OFFLINE=1; it can never download weights or send cloud requests.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import wave

import numpy as np
import av
from faster_whisper.audio import decode_audio
import asr_check


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bad-av-path")
    parser.add_argument("--model-check", action="store_true")
    parser.add_argument("--evidence")
    args = parser.parse_args()
    result = {"pyav": av.__version__, "decoder_passed": False}
    assert av.__version__ == "18.1.0", "Test the pinned deployment version"
    assert asr_check.dependencies() == 0
    with tempfile.TemporaryDirectory(prefix="real-asr-decoder-") as temp:
        wav_path = Path(temp) / "pcm.wav"
        # Check nonzero PCM too: passing a silent probe must not mean that
        # decoding simply discarded every microphone sample.
        expected = ((np.arange(16000, dtype=np.int32) % 100) * 256 - 12000).astype("<i2")
        with wave.open(str(wav_path), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(expected.tobytes())
        decoded = decode_audio(str(wav_path), sampling_rate=16000)
        assert len(decoded) == 16000
        assert np.allclose(decoded, expected.astype(np.float32) / 32768.0, atol=1 / 32768.0)
        result["decoder_passed"] = True
        print("PASS: pinned real PyAV/faster-whisper decodes silence and preserves nonzero microphone PCM")
    if args.bad_av_path:
        bad = Path(args.bad_av_path).resolve()
        script = """import sys
sys.path.insert(0, sys.argv[1])
sys.path.insert(1, sys.argv[2])
import av, asr_check
assert av.__version__.startswith('19.')
raise SystemExit(asr_check.dependencies())
"""
        process = subprocess.run([sys.executable, "-c", script, str(bad), str(Path(__file__).parent)],
                                 text=True, encoding="utf-8", errors="replace", capture_output=True, timeout=30)
        assert process.returncode == 1, process.stdout + process.stderr
        assert "unexpected keyword argument 'metadata_errors'" in process.stdout
        result["pyav19_baseline"] = process.stdout
        print("PASS: real isolated PyAV 19 reproduces metadata_errors; new preflight rejects before model load")
    if args.model_check:
        env = dict(os.environ, HF_HUB_OFFLINE="1")
        process = subprocess.run([sys.executable, str(Path(__file__).with_name("asr_check.py")),
                                  "--prepare", "--app-dir", str(Path(__file__).parent)],
                                 text=True, encoding="utf-8", errors="replace", capture_output=True,
                                 timeout=120, env=env)
        assert process.returncode == 0 and "ASR_PREPARED" in process.stdout, process.stdout + process.stderr
        result["real_cached_model"] = process.stdout
        print("PASS: real cached small CPU/int8, decoder and VAD complete; model network disabled")
    if args.evidence:
        Path(args.evidence).write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
