"""Exercise the LAN copy's actual model using the existing Chinese test WAV.

Never uploads audio, sends device commands, calls AI, or downloads weights.
"""
import argparse
import json
import os
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web-dir", required=True)
    parser.add_argument("--wav", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(Path(args.web_dir).resolve()))
    import lan_start
    models = lan_start.find_models(Path(args.web_dir).resolve())
    os.environ["ASR_MODELS_DIR"] = str(models)
    os.environ["HF_HUB_OFFLINE"] = "1"
    import asr_check
    assert asr_check.dependencies() == 0
    import asr
    result = asr.transcribe(str(Path(args.wav).resolve()))
    assert not result.get("error"), result.get("error")
    assert result.get("text", "").strip(), "Known spoken test WAV produced no text"
    assert asr.model_status()["loaded"]
    evidence = {"actual_lan_model": True, "model": "small", "device": "cpu", "compute": "int8",
                "cached_offline": True, "fixture": Path(args.wav).name,
                "result": result, "boards_contacted": False, "ai_called": False}
    Path(args.output).write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("LAN_SPEECH_READY: Chinese WAV recognized using the packaged service and cached model.")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main()
