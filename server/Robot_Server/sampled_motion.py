"""固定间隔三轴动作：本机重采样、可读导出及有界网关预取。

网页传入原始录制和映射，compile_recording 按100/500ms生成普通舵机命令。
Player只保留每设备一段动作；ESP按已接收索引预取12步，本地计时逐步发送。
报告的进度是网关向UART发送的进度，不是舵机编码器回读；断线不自动重放。
保存的动作保留原始录制，执行前必须重新使用管理员当前限位校验。
"""
import copy
import json
import math
import re
import threading
import time
import uuid
from pathlib import Path


def integer(value, lo, hi, name):
    if type(value) is not int or not lo <= value <= hi:
        raise ValueError(name + ' 超出范围')
    return value


def compile_recording(value, profile):
    if not isinstance(value, dict):
        raise ValueError('动作必须是对象')
    interval = integer(value.get('interval_ms', 500), 100, 500, '采样间隔')
    if interval not in (100, 500):
        raise ValueError('采样间隔仅支持100或500ms')
    r = value.get('recording', {})
    if not isinstance(r, dict) or r.get('schema') != 1 or r.get('pose_schema') != 2:
        raise ValueError('请先在新版训练页载入并导出绝对角录制')
    samples = r.get('samples')
    if not isinstance(samples, list) or not 2 <= len(samples) <= 1000:
        raise ValueError('需要2至1000个原始采样点')
    previous = -1
    for p in samples:
        if not isinstance(p, dict):
            raise ValueError('采样点必须为对象')
        t = integer(p.get('t'), 0, 30000, '采样时间')
        if t <= previous or (previous == -1 and t != 0):
            raise ValueError('采样时间需从0开始递增')
        previous = t
        for key in ('yaw', 'pitch', 'roll'):
            a = p.get(key)
            if type(a) not in (int, float) or not math.isfinite(a) or not -360 <= a <= 360:
                raise ValueError('采样角度无效')
    trim = r.get('trim', [0, previous])
    if not isinstance(trim, list) or len(trim) != 2:
        raise ValueError('裁剪范围无效')
    start, end = [integer(t, 0, previous, '裁剪时间') for t in trim]
    if end-start < 250:
        raise ValueError('动作至少0.25秒')
    mapping = value.get('mapping')
    servos = {s['id']: s for s in profile['servos']}
    if not isinstance(mapping, list) or len(mapping) != 3:
        raise ValueError('需要三个轴的映射')
    ids = []
    for m in mapping:
        if not isinstance(m, dict):
            raise ValueError('映射格式无效')
        sid = integer(m.get('id'), 1, 3, '舵机编号')
        if sid not in servos or m.get('sign') not in (-1, 1) or type(m.get('sign')) is not int:
            raise ValueError('舵机编号或方向无效')
        offset = m.get('offset')
        if type(offset) not in (int, float) or not math.isfinite(offset) or abs(offset) > 360:
            raise ValueError('零点偏移无效')
        ids.append(sid)
    if len(set(ids)) != 3:
        raise ValueError('三个轴不可映射到同一舵机')

    def pose(t):
        i = 1
        while i < len(samples)-1 and samples[i]['t'] < t:
            i += 1
        a, b = samples[i-1], samples[i]
        k = (t-a['t'])/(b['t']-a['t'])
        result = [0, 0, 0]
        for axis, m in zip(('yaw', 'pitch', 'roll'), mapping):
            # 与浏览器Math.round一致；先插值，再映射，再按有效校准检查。
            angle = math.floor(m['offset']+m['sign']*(a[axis]+(b[axis]-a[axis])*k)+.5)
            servo = servos[m['id']]
            if not servo['min'] <= angle <= servo['max']:
                raise ValueError('S%d超出管理员当前限位' % m['id'])
            result[m['id']-1] = angle
        return result

    times = list(range(start, end, interval)) + [end]
    points = [pose(t) for t in times]
    # 2.5秒先到起点，之后每个步骤同时给三轴目标，保持该步dt_ms再取下一步。
    steps = [dict(index=0, at_ms=0, dt_ms=2500, angles=points[0], speeds=[90]*3)]
    at = 2500
    for i in range(1, len(times)):
        dt = times[i]-times[i-1]
        speeds = [max(1, math.ceil(abs(b-a)*1000/dt)) for a,b in zip(points[i-1], points[i])]
        if max(speeds) > 360:
            raise ValueError('录制变化超过360°/秒，请放慢动作；不会静默改速')
        steps.append(dict(index=i, at_ms=at, dt_ms=dt, angles=points[i], speeds=speeds))
        at += dt
    for step in steps:
        step['commands'] = ['@MOVE:servo,%d,%d,%d#' % (i+1, step['angles'][i], step['speeds'][i]) for i in range(3)]
    face = r.get('expression', '')
    if not isinstance(face, str) or not re.fullmatch('[a-z_]{1,20}', face):
        raise ValueError('表情名称无效')
    return dict(schema=1, kind='sampled-three-axis', name=str(r.get('name', '录制动作'))[:40],
                expression=face, interval_ms=interval, duration_ms=at, steps=steps)


class Player:
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.lock = threading.RLock()
        self.jobs = {}

    def status(self, device):
        with self.lock:
            j = self.jobs.get(device)
            if not j:
                return dict(phase='idle', active=False)
            if j['active'] and self.clock()-j['seen'] > 12:
                j.update(phase='offline', active=False)
            # 网关一直领取但 accepted 始终 0：说明 F407 没回 @STEPQ，不能让播放器永久占着。
            if j['active'] and j['accepted'] == 0 and self.clock()-j['started_at'] > 15:
                j.update(phase='timeout', active=False)
            # 停止时板端不回 stopped（本来就没 accepted）：最多等 5 秒。
            if j['active'] and j['phase'] == 'stopping' and self.clock()-j.get('stop_at', 0) > 5:
                j.update(phase='stopped', active=False)
            return {k: copy.deepcopy(v) for k,v in j.items()
                    if k not in ('steps', 'seen', 'started_at', 'stop_at')}

    def busy(self, device):
        return self.status(device)['active']

    def start(self, device, compiled):
        with self.lock:
            if self.busy(device):
                raise ValueError('该设备已有逐步动作，请先停止')
            if device not in self.jobs and len(self.jobs) >= 64:
                old = next((d for d in self.jobs if not self.busy(d)), None)
                if old is None:
                    raise ValueError('动作设备数已达上限')
                self.jobs.pop(old)
            self.jobs[device] = dict(id=uuid.uuid4().hex, phase='waiting_gateway', active=True,
                accepted=0, sent=0, total=len(compiled['steps']), steps=compiled['steps'],
                name=compiled['name'], expression=compiled['expression'], seen=self.clock(),
                duration_ms=compiled['duration_ms'], board_pos=0, board_phase='transferring',
                started_at=self.clock(), stop_at=0)
            return self.status(device)

    def stop(self, device):
        with self.lock:
            if self.busy(device):
                # 还没 accepted 任何一步：板端没有会话，不必等 stopped，直接清。
                if self.jobs[device]['accepted'] == 0:
                    self.jobs[device].update(phase='stopped', active=False)
                else:
                    self.jobs[device].update(phase='stopping', stop_at=self.clock())
            return self.status(device)

    def exchange(self, device, report, profile=None):
        with self.lock:
            self.status(device)
            j = self.jobs.get(device)
            if not j or not j['active']:
                # 服务重启/未知会话：明确要求网关停止，绝不恢复历史动作。
                return dict(stop=True, id=report.get('id', '')) if isinstance(report, dict) and report.get('active') else None
            if not isinstance(report, dict) or report.get('protocol') != 1:
                return None
            j['seen'] = self.clock()
            if profile:
                limits = {s['id']: s for s in profile['servos']}
                if any(not limits[i+1]['min'] <= s['angles'][i] <= limits[i+1]['max'] for s in j['steps'] for i in range(3)):
                    j['phase'] = 'stopping'
            if not report.get('id') and j['accepted']:
                j.update(phase='gateway_restarted', active=False)
                return dict(stop=True, id='')
            if report.get('id') == j['id']:
                accepted, sent = report.get('accepted'), report.get('sent')
                if sent is None:
                    sent = accepted
                if not (type(accepted) is int and type(sent) is int and j['accepted'] <= accepted <= j['total'] and j['sent'] <= sent <= accepted):
                    return None
                j.update(accepted=accepted, sent=sent)
                board = report.get('board')
                if isinstance(board, dict):
                    pos = board.get('pos')
                    if type(pos) is int and not isinstance(pos, bool) and 0 <= pos <= j['total']:
                        j['board_pos'] = pos
                    if isinstance(board.get('phase'), str) and len(board['phase']) <= 16:
                        j['board_phase'] = board['phase']
                phase = report.get('phase')
                if phase == 'done' and sent != j['total']:
                    return None
                if phase in ('transferring', 'starting', 'running', 'buffering', 'paused',
                             'done', 'stopped', 'link_lost', 'expired', 'error'):
                    if j['phase'] != 'stopping' or phase in ('stopped', 'link_lost', 'expired', 'error'):
                        j['phase'] = phase
                    if phase in ('done', 'stopped', 'link_lost', 'expired', 'error'):
                        j['active'] = False
                        return None
            if j['phase'] == 'stopping':
                return dict(id=j['id'], stop=True)
            if report.get('id') and report['id'] != j['id'] and report.get('active'):
                return dict(id=report['id'], stop=True)
            start = j['accepted']
            return dict(id=j['id'], total=j['total'], start=start, expression=j['expression'],
                steps=[{k:s[k] for k in ('at_ms', 'angles', 'speeds')} for s in j['steps'][start:start+12]])


class Library:
    def __init__(self, path):
        self.path = Path(path)
        self.lock = threading.RLock()

    def read(self):
        with self.lock:
            return json.loads(self.path.read_text(encoding='utf-8')) if self.path.exists() else {}

    def save(self, value):
        key = value['recording'].get('id', '')
        if not isinstance(key, str) or not re.fullmatch(r'train_[a-z0-9_]{1,40}', key):
            raise ValueError('动作编号无效')
        with self.lock:
            items = self.read()
            if key not in items and len(items) >= 100:
                raise ValueError('动作库已满100个')
            items[key] = value
            temp = self.path.with_suffix('.tmp')
            temp.write_text(json.dumps(items, ensure_ascii=False, indent=2), encoding='utf-8')
            temp.replace(self.path)
        return key
