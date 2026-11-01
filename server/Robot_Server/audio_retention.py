"""Persist the user-selected audio limit separately from mode and credentials."""
import json
import os
import tempfile
import threading

PATH = os.path.join(os.path.dirname(__file__), "audio_retention.json")
_lock = threading.Lock()


def get():
    with _lock:
        try:
            with open(PATH, encoding="utf-8") as source:
                value = json.load(source)
            limit = value.get("limit")
            if value.get("enabled") is True and type(limit) is int and 1 <= limit <= 5000:
                return {"enabled": True, "limit": limit}
        except (OSError, ValueError, AttributeError):
            pass
    # First deployment never silently removes existing historical recordings.
    return {"enabled": False, "limit": 200}


def save(limit):
    if type(limit) is not int or not 1 <= limit <= 5000:
        raise ValueError("音频保留数量必须是1~5000的整数")
    settings = {"enabled": True, "limit": limit}
    with _lock:
        parent = os.path.dirname(os.path.abspath(PATH))
        fd, temporary = tempfile.mkstemp(prefix="audio-retention-", suffix=".json", dir=parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as output:
                json.dump(settings, output)
            os.replace(temporary, PATH)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    return settings
