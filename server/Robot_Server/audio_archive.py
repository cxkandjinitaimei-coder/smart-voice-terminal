"""audio_archive.py：SQLite 录音归档及待发送文本 FIFO。

使用：audio_rx 用记录 ID 更新阶段，网页按 ID 试听或确认发送；调用时传入录音目录。
时序与边界：数据库事务内更新，文件删除前验证所属目录；保留数量受 audio_retention 控制，不能仅保留最近一条。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
import json
import os
import sqlite3
import time
import uuid
import wave
from contextlib import contextmanager


@contextmanager
def _connect(directory):
    os.makedirs(directory, exist_ok=True)
    db = sqlite3.connect(os.path.join(directory, "audio_history.sqlite3"), timeout=10)
    db.execute("CREATE TABLE IF NOT EXISTS recordings ("
               "seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT UNIQUE NOT NULL, "
               "sess TEXT NOT NULL, data TEXT NOT NULL, phase TEXT NOT NULL DEFAULT '')")
    if "phase" not in {row[1] for row in db.execute("PRAGMA table_info(recordings)")}:
        db.execute("ALTER TABLE recordings ADD COLUMN phase TEXT NOT NULL DEFAULT ''")
        for seq, data in db.execute("SELECT seq,data FROM recordings").fetchall():
            db.execute("UPDATE recordings SET phase=? WHERE seq=?", (json.loads(data).get("inject", ""), seq))
    db.execute("CREATE INDEX IF NOT EXISTS recordings_phase ON recordings(phase,seq)")
    try:
        with db:
            yield db
    finally:
        db.close()


def save(directory, record):
    if not record.get("id"):
        record["id"] = uuid.uuid4().hex
    with _connect(directory) as db:
        db.execute("INSERT INTO recordings(id,sess,data,phase) VALUES(?,?,?,?) "
                   "ON CONFLICT(id) DO UPDATE SET data=excluded.data,phase=excluded.phase",
                   (record["id"], record["sess"], json.dumps(record, ensure_ascii=False), record.get("inject", "")))


def get(directory, key):
    with _connect(directory) as db:
        row = db.execute("SELECT data FROM recordings WHERE id=? OR sess=? "
                         "ORDER BY (id=?) DESC,seq DESC LIMIT 1", (key, key, key)).fetchone()
    return json.loads(row[0]) if row else None


def page(directory, offset=0, limit=100):
    offset, limit = max(0, int(offset)), max(1, min(100, int(limit)))
    with _connect(directory) as db:
        total = db.execute("SELECT COUNT(*) FROM recordings").fetchone()[0]
        rows = db.execute("SELECT seq,data FROM recordings ORDER BY seq DESC LIMIT ? OFFSET ?",
                          (limit, offset)).fetchall()
    return {"total": total, "offset": offset,
            "items": [dict(json.loads(data), number=seq) for seq, data in rows]}


def pending(directory):
    with _connect(directory) as db:
        rows = db.execute("SELECT data FROM recordings WHERE phase IN ('pending','manual_failed','auto_failed') ORDER BY seq").fetchall()
    return [record for row in rows for record in [json.loads(row[0])]
            if record.get("inject") in ("pending", "manual_failed", "auto_failed") and record.get("text")]


def prune(directory, limit):
    """Keep the newest limit records, plus all unfinished/retryable older ones.

    Only archive-owned WAV files may be deleted. Hold a SQLite write transaction
    so an archived phase cannot change between selection and removal.
    """
    if type(limit) is not int or limit < 1:
        raise ValueError("Recording limit must be a positive integer")
    deletable = {"auto", "manual", "dropped", "skipped", "imported"}
    deleted, protected, failed = 0, 0, 0
    base = os.path.realpath(directory)
    with _connect(directory) as db:
        if not db.in_transaction:
            db.execute("BEGIN IMMEDIATE")
        rows = db.execute("SELECT id,data,phase FROM recordings ORDER BY seq DESC").fetchall()
        for record_id, data, phase in rows[limit:]:
            if phase not in deletable:
                protected += 1
                continue
            record = json.loads(data)
            path = os.path.realpath(record.get("saved") or "")
            # No nested directories, symlink escapes, arbitrary files or indexes.
            if (os.path.dirname(path) != base or
                    not os.path.basename(path).startswith("rec_") or not path.lower().endswith(".wav")):
                failed += 1
                continue
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass  # Remove a stale index too; the WAV was already absent.
            except OSError:
                failed += 1
                continue  # Keep the index if the file was locked/unremovable.
            db.execute("DELETE FROM recordings WHERE id=?", (record_id,))
            deleted += 1
    return {"deleted": deleted, "protected": protected, "failed": failed,
            "remaining": len(rows) - deleted}


def import_existing(directory):
    """Expose old WAVs too; unknown old completeness never grants ASR eligibility."""
    with _connect(directory) as db:
        records = [json.loads(row[0]) for row in db.execute("SELECT data FROM recordings")]
    known = {os.path.realpath(r.get("saved", "")) for r in records}
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if not name.startswith("rec_") or not name.lower().endswith(".wav") or os.path.realpath(path) in known:
            continue
        try:
            with wave.open(path, "rb") as wav:
                duration = wav.getnframes() / float(wav.getframerate())
                received = wav.getnframes() * wav.getsampwidth() * wav.getnchannels()
            save(directory, {"id": uuid.uuid4().hex, "sess": "legacy:" + name,
                             "at": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(path))),
                             "saved": path, "dur": round(duration, 3), "received": received,
                             "total": received, "complete": False, "playable": True,
                             "text": "", "inject": "imported", "err": "历史录音，完整性未核验"})
        except (OSError, wave.Error, EOFError):
            continue
    # Do not replay AI actions automatically after a process restart.
    for record in records:
        if record.get("inject") in ("queued", "processing", "sending"):
            record.update(inject="pending" if record.get("text") else "interrupted",
                          err="服务重启中断处理；有文字的记录可手动重试")
            save(directory, record)
