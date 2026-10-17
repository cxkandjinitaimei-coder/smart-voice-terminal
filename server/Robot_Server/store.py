"""store.py：SQLite 遥测、事件、命令与交互记录，以及后台日志导入/聚合。

使用：start_store 启动后台整理，API 查询统计；数据库和原始日志默认存服务目录。
时序与边界：连接/事务按函数约定使用，统计过滤无效传感器行；采集频率不因分钟曲线聚合而改变。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
# -*- coding: utf-8 -*-

import json
import os
import sqlite3
import threading
from datetime import datetime, timedelta

import analytics

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.path.join(BASE_DIR, "relay.db")
DATA_HISTORY = os.path.join(BASE_DIR, "data_history.txt")
DECISION_LOG = os.path.join(BASE_DIR, "decision_log.txt")

TS_FMT = "%Y-%m-%d %H:%M:%S"

_lock = threading.RLock()
_initialized = False

# ---------------------------------------------------------------- schema

_SCHEMA = """
CREATE TABLE IF NOT EXISTS sensor_readings (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  ts        TEXT    NOT NULL,
  epoch     INTEGER,
  device_id TEXT,
  temp      REAL,
  humi      REAL,
  distance  REAL,
  presence  INTEGER,
  valid     INTEGER NOT NULL DEFAULT 0,
  raw       TEXT
);
CREATE INDEX IF NOT EXISTS idx_sr_epoch  ON sensor_readings(epoch);
CREATE INDEX IF NOT EXISTS idx_sr_vtime  ON sensor_readings(valid, epoch);

CREATE TABLE IF NOT EXISTS events (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  ts        TEXT    NOT NULL,
  epoch     INTEGER,
  device_id TEXT,
  kind      TEXT,
  code      TEXT,
  severity  TEXT,
  message   TEXT,
  source    TEXT,
  need_ai   INTEGER NOT NULL DEFAULT 0,
  ai_called INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_ev_epoch ON events(epoch);
CREATE INDEX IF NOT EXISTS idx_ev_code  ON events(code, epoch);

CREATE TABLE IF NOT EXISTS interactions (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  ts           TEXT    NOT NULL,
  epoch        INTEGER,
  device_id    TEXT,
  user_text    TEXT,
  ai_message   TEXT,
  mode         TEXT,
  provider     TEXT,
  model        TEXT,
  is_auto_tick INTEGER NOT NULL DEFAULT 0,
  is_mock      INTEGER NOT NULL DEFAULT 0,
  n_moves      INTEGER NOT NULL DEFAULT 0,
  moves_json   TEXT
);
CREATE INDEX IF NOT EXISTS idx_ia_epoch ON interactions(epoch);

CREATE TABLE IF NOT EXISTS commands (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  ts        TEXT    NOT NULL,
  epoch     INTEGER,
  device_id TEXT,
  raw       TEXT,
  source    TEXT,
  note      TEXT,
  ack       TEXT,
  ack_ts    TEXT,
  ack_epoch INTEGER
);
CREATE INDEX IF NOT EXISTS idx_cm_epoch ON commands(epoch);

CREATE TABLE IF NOT EXISTS meta (
  key   TEXT PRIMARY KEY,
  value TEXT
);
"""


# ---------------------------------------------------------------- 基础设施

def _conn():
    conn = sqlite3.connect(DB_PATH, timeout=10.0)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    return conn


def init_db():
    """幂等建表。启动时调用一次。"""
    global _initialized
    with _lock:
        if _initialized:
            return
        conn = _conn()
        try:
            with conn:
                conn.executescript(_SCHEMA)
        finally:
            conn.close()
        _initialized = True


def parse_ts(s):
    """'2026-09-14 22:00:00' → datetime；解析不了返回 None。"""
    try:
        return datetime.strptime(str(s)[:19], TS_FMT)
    except Exception:
        return None


def to_epoch(s):
    """时间字符串 → unix 秒（本地时区，和日志口径一致）。解析不了返回 None。"""
    dt = parse_ts(s)
    return int(dt.timestamp()) if dt else None


def _now_epoch():
    return int(datetime.now().timestamp())


def _advance(rows):
    """把 [{'ts':...,'epoch':...}] 补上缺失的 epoch（回填老数据用）。"""
    for r in rows:
        if not r.get("epoch"):
            r["epoch"] = to_epoch(r.get("ts"))
    return rows


# ---------------------------------------------------------------- meta

def get_meta(key, default=None):
    init_db()
    with _lock:
        conn = _conn()
        try:
            cur = conn.execute("SELECT value FROM meta WHERE key=?", (key,))
            row = cur.fetchone()
        finally:
            conn.close()
    return row["value"] if row else default


def set_meta(key, value):
    init_db()
    with _lock:
        conn = _conn()
        try:
            with conn:
                conn.execute(
                    "INSERT INTO meta(key,value) VALUES(?,?) "
                    "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                    (key, str(value)))
        finally:
            conn.close()


# ---------------------------------------------------------------- 写入

#  每个写入都有 _insert_xxx(conn, ...) 内部版 + record_xxx(...) 公开版。
#   分开是因为回填要一次灌几千行：公开版每行都"开连接 + 提交"，实测 7360 行要 46 秒，
#   会让每次启动都卡将近一分钟。内部版接受外部连接，回填时共用一个事务。
#
#   给所有带 `with conn:` 的公开版加上 _lock(RLock)，线程安全。

def _insert_sensor(conn, ts, device_id, raw, parsed=None):
    if parsed is None:
        parsed = analytics.parse_sensor(raw)
    cur = conn.execute(
        "INSERT INTO sensor_readings"
        "(ts,epoch,device_id,temp,humi,distance,presence,valid,raw) "
        "VALUES(?,?,?,?,?,?,?,?,?)",
        (ts, to_epoch(ts), device_id,
         parsed.get("temp"), parsed.get("humi"),
         parsed.get("distance"), parsed.get("presence"),
         1 if parsed.get("valid") else 0, raw))
    return cur.lastrowid, parsed


def record_sensor(ts, device_id, raw, parsed=None):
    """记一条传感器读数。parsed 不给就现场解析（口径见 analytics.parse_sensor）。

    返回 (row_id, parsed)。
    """
    init_db()
    with _lock:
        conn = _conn()
        try:
            with conn:
                return _insert_sensor(conn, ts, device_id, raw, parsed)
        finally:
            conn.close()


def _insert_event(conn, ts, device_id, kind, code, severity, message,
                  source="threshold", need_ai=False, ai_called=False):
    cur = conn.execute(
        "INSERT INTO events"
        "(ts,epoch,device_id,kind,code,severity,message,source,"
        " need_ai,ai_called) VALUES(?,?,?,?,?,?,?,?,?,?)",
        (ts, to_epoch(ts), device_id, kind, code, severity, message,
         source, 1 if need_ai else 0, 1 if ai_called else 0))
    return cur.lastrowid


def record_event(ts, device_id, kind, code, severity, message,
                 source="threshold", need_ai=False, ai_called=False):
    init_db()
    with _lock:
        conn = _conn()
        try:
            with conn:
                return _insert_event(conn, ts, device_id, kind, code, severity,
                                     message, source, need_ai, ai_called)
        finally:
            conn.close()


def _insert_interaction(conn, ts, device_id, user_text, ai_message, mode="",
                        provider="", model="", is_auto_tick=False,
                        is_mock=False, moves=None):
    moves = moves if isinstance(moves, list) else []
    cur = conn.execute(
        "INSERT INTO interactions"
        "(ts,epoch,device_id,user_text,ai_message,mode,provider,model,"
        " is_auto_tick,is_mock,n_moves,moves_json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
        (ts, to_epoch(ts), device_id, user_text, ai_message, mode,
         provider, model, 1 if is_auto_tick else 0, 1 if is_mock else 0,
         len(moves), json.dumps(moves, ensure_ascii=False)))
    return cur.lastrowid


def record_interaction(ts, device_id, user_text, ai_message, mode="",
                       provider="", model="", is_auto_tick=False,
                       is_mock=False, moves=None):
    init_db()
    with _lock:
        conn = _conn()
        try:
            with conn:
                return _insert_interaction(conn, ts, device_id, user_text,
                                           ai_message, mode, provider, model,
                                           is_auto_tick, is_mock, moves)
        finally:
            conn.close()


def _insert_command(conn, ts, device_id, raw="", source="web", note=""):
    cur = conn.execute(
        "INSERT INTO commands"
        "(ts,epoch,device_id,raw,source,note) VALUES(?,?,?,?,?,?)",
        (ts, to_epoch(ts), device_id, raw, source, note))
    return cur.lastrowid


def record_command(ts, device_id, raw="", source="web", note=""):
    init_db()
    with _lock:
        conn = _conn()
        try:
            with conn:
                return _insert_command(conn, ts, device_id, raw, source, note)
        finally:
            conn.close()


def _attach_ack(conn, device_id, ack, ts):
    """挂到该设备**最近一条还没 ack 的**命令上。返回更新行数（0 = 没找到，正常）。"""
    if not ack:
        return 0
    cur = conn.execute(
        "UPDATE commands SET ack=?, ack_ts=?, ack_epoch=? "
        "WHERE id = (SELECT id FROM commands "
        "           WHERE device_id=? AND (ack IS NULL OR ack='') "
        "           ORDER BY id DESC LIMIT 1)",
        (ack, ts, to_epoch(ts), device_id))
    return cur.rowcount


def attach_ack(device_id, ack, ts):
    """把 ESP 回传的 ack 挂到该设备最近一条还没 ack 的命令上。

    挂不上不报错 —— ESP 重启后可能回一条我这边没有对应命令的 ack。
    """
    init_db()
    with _lock:
        conn = _conn()
        try:
            with conn:
                return _attach_ack(conn, device_id, ack, ts)
        finally:
            conn.close()


# ---------------------------------------------------------------- 回填

def _read_lines(path):
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8") as f:
        return f.read().splitlines()


def _pending(path, meta_key):
    """算出还没导入过的行。返回 (行列表, 新的 count, 新的 last_line)。

    正常情况（文件只追加）：从旧 count 处往后切，准确。
    append_line() 裁过文件（>2MB 时只留末尾 5000 行）时行数会变小：
    退化成从"最后导入过的原文"往后找，可能极少量重复 —— 会打日志说明。
    """
    lines = _read_lines(path)
    try:
        count = int(get_meta(meta_key + "_count", 0) or 0)
    except (TypeError, ValueError):
        count = 0
    last_line = get_meta(meta_key + "_last", None)

    if count and len(lines) >= count:
        fresh = lines[count:]
    elif count and last_line:
        print("[STORE] %s 行数从 %d 降到 %d（被 append_line 裁过），"
              "改用 last_line 定位续接点" % (os.path.basename(path), count, len(lines)))
        idx = -1
        for i in range(len(lines) - 1, -1, -1):
            if lines[i] == last_line:
                idx = i
                break
        if idx < 0:
            print("[STORE] %s 续接点没找到，本轮整份重导（可能有重复）"
                  % os.path.basename(path))
            fresh = lines
        else:
            fresh = lines[idx + 1:]
    else:
        fresh = lines

    return fresh, len(lines), (lines[-1] if lines else None)


def backfill():
    """把两个 .txt 里还没导入的行补进库。启动时跑，幂等。

     全程一个连接、一个事务。逐行走 record_xxx() 那种写法实测要 46 秒
      （每行一次 fsync），会让每次启动都卡将近一分钟。
      这里手动把连接交给 _insert_xxx，最后一次提交。

    返回 (导入了多少传感器行, 导入了多少决策行)。
    """
    init_db()
    n_sensor = n_dec = 0
    data_meta = dec_meta = None       # (行数, 最后一行原文)，只有真读到新行才前移
    conn = _conn()
    try:
        with _lock:
            with conn:
                # ---- data_history.txt → sensor_readings / commands
                fresh, count, last_line = _pending(DATA_HISTORY, "bf_data")
                if fresh:
                    data_meta = (count, last_line)
                for line in fresh:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        r = json.loads(line)
                    except Exception:
                        continue
                    if not isinstance(r, dict):
                        continue
                    ts = str(r.get("time", "") or "")
                    did = str(r.get("device_id", "") or "esp-bridge")
                    sensor = str(r.get("sensor", "") or "").strip()
                    if sensor:
                        _insert_sensor(conn, ts, did, sensor)
                        n_sensor += 1
                    # 遥控/定时唤醒的入队记录（event == 'enqueue' / 'auto_tick'）
                    ev = str(r.get("event", "") or "")
                    if ev:
                        cmd = r.get("command") or {}
                        _insert_command(
                            conn, ts, did,
                            raw=str(cmd.get("raw", "") or ""),
                            source=("auto_tick" if ev == "auto_tick" else "web"),
                            note=str(cmd.get("note", "") or ev))
                    # 老数据里的 ack 回执（按顺序处理，前面的 enqueue 已经入库，
                    # 所以"最近一条未 ack 的命令"就是它该挂的那条）
                    ack = str(r.get("ack", "") or "").strip()
                    if ack:
                        _attach_ack(conn, did, ack, ts)

                # ---- decision_log.txt → interactions / events
                fresh, count, last_line = _pending(DECISION_LOG, "bf_dec")
                if fresh:
                    dec_meta = (count, last_line)
                for line in fresh:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        r = json.loads(line)
                    except Exception:
                        continue
                    if not isinstance(r, dict):
                        continue
                    ts = str(r.get("time", "") or "")
                    did = str(r.get("device_id", "") or "esp-bridge")
                    user = str(r.get("user", "") or "")
                    if not user:
                        continue      # 没用户输入的空决策不入互动统计
                    _insert_interaction(
                        conn, ts, did, user,
                        ai_message=str(r.get("message", "") or r.get("text", "") or ""),
                        mode=str(r.get("mode", "") or ""),
                        provider=str(r.get("provider", "") or ""),
                        model=str(r.get("model", "") or ""),
                        is_auto_tick=bool(r.get("tick")),
                        is_mock=str(r.get("provider", "")) == "mock",
                        moves=r.get("moves") if isinstance(r.get("moves"), list) else None)
                    n_dec += 1
                    # 老的异常类用户文本 → 一条事件（kind='legacy'，前端可区分）
                    for code, label in analytics.match_legacy_alert(user):
                        _insert_event(conn, ts, did, "legacy", code, "info", label,
                                      source="backfill")
    finally:
        conn.close()

    # 回填进度在事务外记（行数 + 最后一行原文，见 _pending 的说明）
    if data_meta:
        set_meta("bf_data_count", data_meta[0])
        set_meta("bf_data_last", data_meta[1] or "")
    if dec_meta:
        set_meta("bf_dec_count", dec_meta[0])
        set_meta("bf_dec_last", dec_meta[1] or "")

    if n_sensor or n_dec:
        print("[STORE] 回填完成：传感器 %d 行、决策 %d 行（库：%s）"
              % (n_sensor, n_dec, os.path.basename(DB_PATH)))
    return n_sensor, n_dec


# ---------------------------------------------------------------- 统计查询

def _today_start_epoch():
    now = datetime.now()
    return int(now.replace(hour=0, minute=0, second=0, microsecond=0).timestamp())


def summary():
    """总览页的四个数字（设备在线由 app.py 补，这里只管库里能算的）。"""
    init_db()
    t0 = _today_start_epoch()
    with _lock:
        conn = _conn()
        try:
            q = conn.execute
            return {
                "today_readings": q("SELECT COUNT(*) c FROM sensor_readings "
                                    "WHERE valid=1 AND epoch>=?", (t0,)).fetchone()["c"],
                "today_anomalies": q("SELECT COUNT(*) c FROM events "
                                     "WHERE epoch>=?", (t0,)).fetchone()["c"],
                "today_interactions": q("SELECT COUNT(*) c FROM interactions "
                                        "WHERE epoch>=?", (t0,)).fetchone()["c"],
                "total_readings": q("SELECT COUNT(*) c FROM sensor_readings "
                                    "WHERE valid=1").fetchone()["c"],
                "total_anomalies": q("SELECT COUNT(*) c FROM events").fetchone()["c"],
                "total_interactions": q("SELECT COUNT(*) c FROM interactions").fetchone()["c"],
                "last_reading_ts": (q("SELECT ts FROM sensor_readings WHERE valid=1 "
                                      "ORDER BY epoch DESC LIMIT 1").fetchone() or {"ts": ""})["ts"],
                "last_anomaly_ts": (q("SELECT ts FROM events "
                                      "ORDER BY epoch DESC LIMIT 1").fetchone() or {"ts": ""})["ts"],
            }
        finally:
            conn.close()


def sensor_series(hours=3, limit=400):
    """温湿度序列 + 区间统计。

     points 是**抽样**过的（超过 limit 就等间隔抽），只用来画线；
      下面的 min/max/avg 是拿**全量**算的 —— 抽样会漏掉尖峰，统计不能靠抽样。
    """
    init_db()
    cutoff = _now_epoch() - max(1, hours) * 3600
    with _lock:
        conn = _conn()
        try:
            rows = [dict(r) for r in conn.execute(
                "SELECT ts,epoch,temp,humi,distance,presence FROM sensor_readings "
                "WHERE valid=1 AND epoch>=? ORDER BY epoch ASC", (cutoff,)).fetchall()]
            agg = conn.execute(
                "SELECT COUNT(*) n, MIN(temp) tmin, MAX(temp) tmax, AVG(temp) tavg, "
                "       MIN(humi) hmin, MAX(humi) hmax, AVG(humi) havg "
                "FROM sensor_readings WHERE valid=1 AND epoch>=?", (cutoff,)).fetchone()
        finally:
            conn.close()

    points = rows
    if limit and len(points) > limit:
        stride = len(points) / float(limit)
        points = [points[int(i * stride)] for i in range(limit)]

    def _r(v):
        return round(v, 1) if v is not None else None

    return {
        "points": points,
        "samples": agg["n"] or 0,
        "temp": {"min": _r(agg["tmin"]), "max": _r(agg["tmax"]), "avg": _r(agg["tavg"])},
        "humi": {"min": _r(agg["hmin"]), "max": _r(agg["hmax"]), "avg": _r(agg["havg"])},
        "now": (points[-1] if points else None),
        "hours": hours,
    }


def presence_spans(hours=24):
    """人员在离区间。presence 只有 1/0 两态，按时间邻接合并成 [起,止] 区间。

     数据来源现在只有测试页注入的 `人:有/无`（毫米波雷达还没接），
      没有数据的时段既不算"有人"也不算"无人"，就是空白 —— 前端要如实画成空白。

     sec 是**下界**：一段区间 = 最后一个 presence=1 采样 − 第一个 presence=1 采样，
      所以真实在场时长最多比它多一个采样间隔（采样越稀，少算得越多）。
      只有一个 presence=1 采样时 sec=0 —— 不要将它当"没待"，是"只知道那一刻有人"。
    """
    init_db()
    cutoff = _now_epoch() - max(1, hours) * 3600
    with _lock:
        conn = _conn()
        try:
            rows = conn.execute(
                "SELECT ts,epoch,presence FROM sensor_readings "
                "WHERE valid=1 AND presence IS NOT NULL AND epoch>=? "
                "ORDER BY epoch ASC", (cutoff,)).fetchall()
        finally:
            conn.close()

    spans = []
    present_sec = 0
    cur = None
    for r in rows:
        if r["presence"] == 1:
            if cur is None:
                cur = {"start_ts": r["ts"], "start_epoch": r["epoch"],
                       "end_ts": r["ts"], "end_epoch": r["epoch"]}
            else:
                cur["end_ts"] = r["ts"]
                cur["end_epoch"] = r["epoch"]
        else:
            if cur is not None:
                spans.append(cur)
                cur = None
    if cur is not None:
        spans.append(cur)
    for s in spans:
        s["sec"] = max(0, (s["end_epoch"] or 0) - (s["start_epoch"] or 0))
        present_sec += s["sec"]

    return {
        "spans": spans,
        "present_count": len(spans),
        "present_sec": present_sec,
        "present_min": round(present_sec / 60.0, 1),
        "samples": len(rows),
        "hours": hours,
    }


def anomaly_stats(hours=24, code=None, severity=None, recent_limit=50):
    """异常统计：按类型/等级计数 + 按时间分桶 + 最近若干条。"""
    init_db()
    cutoff = _now_epoch() - max(1, hours) * 3600
    where = ["epoch>=?"]
    args = [cutoff]
    if code:
        where.append("code=?")
        args.append(code)
    if severity:
        where.append("severity=?")
        args.append(severity)
    w = " AND ".join(where)

    with _lock:
        conn = _conn()
        try:
            by_code = [dict(r) for r in conn.execute(
                "SELECT code, MAX(severity) severity, MAX(message) message, "
                "       COUNT(*) n, MAX(ts) last_ts FROM events WHERE " + w +
                " GROUP BY code ORDER BY n DESC", args).fetchall()]
            by_sev = {r["severity"] or "unknown": r["n"] for r in conn.execute(
                "SELECT severity, COUNT(*) n FROM events WHERE " + w +
                " GROUP BY severity", args).fetchall()}
            total = conn.execute(
                "SELECT COUNT(*) n FROM events WHERE " + w, args).fetchone()["n"]
            recent = [dict(r) for r in conn.execute(
                "SELECT ts,epoch,device_id,kind,code,severity,message,source "
                "FROM events WHERE " + w + " ORDER BY epoch DESC LIMIT ?",
                args + [recent_limit]).fetchall()]
        finally:
            conn.close()

    return {
        "total": total,
        "by_code": by_code,
        "by_severity": by_sev,
        "buckets": _raw_bucket_rows("events", cutoff, hours),
        "recent": recent,
        "hours": hours,
    }


def interaction_stats(hours=24, recent_limit=50):
    """用户需求统计：互动次数 + 动作类型分布 + 时间分桶 + 最近若干条。

    注意"互动"的三种来源要分清（前端要能区分）：
      真实AI（provider 是模型名） / 测试模式（provider=='mock'） / 定时唤醒（tick）
    """
    init_db()
    cutoff = _now_epoch() - max(1, hours) * 3600
    with _lock:
        conn = _conn()
        try:
            total = conn.execute(
                "SELECT COUNT(*) n FROM interactions WHERE epoch>=?",
                (cutoff,)).fetchone()["n"]
            flags = conn.execute(
                "SELECT SUM(is_mock) mock, SUM(is_auto_tick) tick FROM interactions "
                "WHERE epoch>=?", (cutoff,)).fetchone()
            recent = [dict(r) for r in conn.execute(
                "SELECT ts,epoch,device_id,user_text,ai_message,mode,provider,model,"
                "       is_auto_tick,is_mock,n_moves,moves_json FROM interactions "
                "WHERE epoch>=? ORDER BY epoch DESC LIMIT ?",
                (cutoff, recent_limit)).fetchall()]
            buckets = _raw_bucket_rows("interactions", cutoff, hours)
        finally:
            conn.close()

    by_move = {}
    for r in recent:
        try:
            moves = json.loads(r.get("moves_json") or "[]")
        except Exception:
            moves = []
        for m in moves:
            if isinstance(m, dict):
                t = str(m.get("type", "") or "?")
                by_move[t] = by_move.get(t, 0) + 1

    mock = int(flags["mock"] or 0)
    tick = int(flags["tick"] or 0)
    return {
        "total": total,
        "mock": mock,
        "auto_tick": tick,
        "real_ai": max(0, total - mock - tick),
        "by_move_type": by_move,
        "buckets": buckets,
        "recent": recent,
        "hours": hours,
    }


def _raw_bucket_rows(table, cutoff, hours, buckets=48):
    """按时间分桶计数（给柱状图用）。桶宽自适应，最多 48 根柱子。

     返回的是**连续**的桶：没数据的时段 count=0 也占一格。
      否则柱状图会把"23:47"和"02:15"画成相邻两根，看着像中间那两小时一直有异常。
    """
    span = max(1, hours) * 3600
    width = max(60, int(span / float(buckets)))
    origin = (cutoff // width) * width
    init_db()
    with _lock:
        conn = _conn()
        try:
            rows = conn.execute(
                "SELECT (epoch-?)/? AS b, COUNT(*) n FROM " + table +
                " WHERE epoch>=? GROUP BY b ORDER BY b", (origin, width, cutoff)
            ).fetchall()
        finally:
            conn.close()

    got = {int(r["b"]): r["n"] for r in rows}
    n = max(1, int((_now_epoch() - origin) / width) + 1)
    fmt = "%m-%d %H:%M" if width < 86400 else "%m-%d"
    return [{"epoch": origin + i * width,
             "label": datetime.fromtimestamp(origin + i * width).strftime(fmt),
             "count": got.get(i, 0)} for i in range(n)]
