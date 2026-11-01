"""相机任务、租约和图像识别的服务端工作区。

职责：保存设备配置和人物资料，给 ESP32-S3 分配有界拍照租约，接收完整且可校验
的 JPEG，再交给本地/兼容视觉模型分析。设备不与本模块建立裸 MCU socket，而是
按 HTTP 任务协议领取、上传和确认；任务状态、去重票据和失败原因由本模块维护。

调用方式：Flask 路由调用 create/claim/upload/complete 等接口，网页只读取规范化
结果。限制：租约有过期时间，服务重启后只恢复允许恢复的数据，不伪造设备已执行；
图像大小、设备 ID、人物数量和并发分析都有上限，任何 token 或原图路径不得写入日志。
"""
import hashlib
import io
import json
import re
import secrets
import sqlite3
import threading
import time
import uuid
import zlib
from datetime import datetime, timedelta, timezone
from contextlib import contextmanager
from pathlib import Path

import local_faces
import camera_follow
from camera_live import LiveCamera
from PIL import Image, ImageOps, UnidentifiedImageError
from flask import Blueprint, jsonify, request, send_file
from werkzeug.exceptions import RequestEntityTooLarge

MAX_IMAGE_BYTES = 4 * 1024 * 1024
TZ = timezone(timedelta(hours=8))
DEFAULT = dict(enabled=False, paused=False, source='device', interval_sec=10,
               windows=[], analyze=True, provider='local-face', keep_count=50,
               block_audio=True, block_ota=True, block_motion=True, camera_transport='s3-wifi',
               capture_mode='interval', continuous_fps=2, cooldown_sec=10,
               **local_faces.ANGLE_DEFAULTS, **camera_follow.DEFAULTS)


class CameraError(Exception):
    def __init__(self, message, code=400):
        super().__init__(message)
        self.code = code


def device_id(value):
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', value):
        raise CameraError('设备名称需为 1~64 位字母、数字、下划线或短横线')
    return value


def validate(value):
    if not isinstance(value, dict):
        raise CameraError('配置必须为对象')
    result = dict(DEFAULT)
    if set(value) - set(DEFAULT):
        raise CameraError('包含不支持的相机配置项')
    result.update(value)
    # 老配置没有冷却字段时沿用原定时间隔，不悄悄把2秒改成10秒。
    if 'cooldown_sec' not in value:
        result['cooldown_sec'] = min(10, result['interval_sec']) if type(result['interval_sec']) is int else 10
    try:
        camera_follow.validate(result)
    except ValueError as error:
        raise CameraError(str(error))
    # 读取旧配置时迁移相机链路；其它用户设置和舵机校准不变。
    if result['camera_transport'] == 'legacy':
        result['camera_transport'] = 's3-wifi'
    if result['camera_transport'] != 's3-wifi':
        raise CameraError('当前仅支持 ESP32-S3 Wi-Fi 相机')
    if result['capture_mode'] not in ('interval', 'sound', 'interval_sound', 'continuous'):
        raise CameraError('采集模式需为定时、声音、定时加声音或连续')
    if result['capture_mode'] == 'continuous' and (result['camera_transport'] != 's3-wifi' or result['source'] != 'device'):
        raise CameraError('连续模式仅支持 ESP32-S3 Wi-Fi 相机')
    if result['capture_mode'] in ('sound', 'interval_sound') and result['source'] != 'device':
        raise CameraError('声音触发模式需选择机器人相机，由F407麦克风触发')
    try:
        result.update(local_faces.angle_config(result))
    except ValueError as error:
        raise CameraError(str(error))
    for key in ['enabled', 'paused', 'analyze', 'block_audio', 'block_ota', 'block_motion']:
        if type(result[key]) is not bool:
            raise CameraError(key + ' 必须为开关')
    for key, lower, upper in [('interval_sec', 2, 3600), ('cooldown_sec', 2, 3600), ('keep_count', 5, 500), ('continuous_fps', 1, 5)]:
        if type(result[key]) is not int or not lower <= result[key] <= upper:
            raise CameraError(key + ' 超出允许范围')
    if result['source'] not in ('device', 'browser'):
        raise CameraError('采集来源需为机器人或浏览器相机')
    if not isinstance(result['provider'], str) or len(result['provider']) > 100:
        raise CameraError('分析模型名称无效')
    windows = result['windows']
    if not isinstance(windows, list) or len(windows) > 8:
        raise CameraError('最多允许 8 个启用时段')
    for window in windows:
        if not isinstance(window, dict) or set(window) != {'start', 'end'}:
            raise CameraError('时段必须包含 start / end')
        for value in window.values():
            if not isinstance(value, str) or not re.fullmatch(r'(?:[01]\d|2[0-3]):[0-5]\d', value):
                raise CameraError('时间格式需为 HH:MM')
        if window['start'] == window['end']:
            raise CameraError('时段起止不能相同；全天启用请留空')
    # 阶段：旧 API 字段只迁移到本地识别引擎；上传照片统一进入检测流程。
    result.update(analyze=True, provider='local-face')
    return result


def in_window(settings, now):
    minute = datetime.fromtimestamp(now, TZ).strftime('%H:%M')
    return not settings['windows'] or any(
        (w['start'] <= minute < w['end']) if w['start'] < w['end']
        else (minute >= w['start'] or minute < w['end']) for w in settings['windows'])


def normalize_image(raw):
    if not raw or len(raw) > MAX_IMAGE_BYTES:
        raise CameraError('图片为空或超过 4 MiB', 413)
    try:
        with Image.open(io.BytesIO(raw)) as image:
            if image.format not in ('JPEG', 'PNG'):
                raise CameraError('仅支持 JPEG / PNG 图片')
            if max(image.size) > 8192 or image.width * image.height > 16_000_000:
                raise CameraError('图片尺寸过大（最多 1600 万像素）')
            image.load()
            image = ImageOps.exif_transpose(image).convert('RGB')
            image.thumbnail((1280, 1280))
            out = io.BytesIO()
            image.save(out, format='JPEG', quality=85)  # Strip EXIF/GPS and original names.
            return out.getvalue(), image.width, image.height
    except (UnidentifiedImageError, OSError, Image.DecompressionBombError, ValueError):
        raise CameraError('图片损坏或格式不支持')



class CameraWorkspace:
    def __init__(self, root, config, blockers, clock=time.time):
        self.root = Path(root)
        self.config = config
        self.blockers = blockers
        self.clock = clock
        self.lock = threading.RLock()
        self.slots = threading.BoundedSemaphore(2)
        self.ready = False
        self.seen = {}
        self.sound_events = {}
        self.faces = local_faces.LocalFaces(self.root / 'face_library')
        self.on_analysis = None
        self.on_settings = None
        self.follow_status = lambda device: {}
        self.live = LiveCamera(self)

    @contextmanager
    def db(self):
        self.root.mkdir(parents=True, exist_ok=True)
        db = sqlite3.connect(str(self.root / 'camera.sqlite3'), timeout=10)
        db.row_factory = sqlite3.Row
        if not self.ready:
            db.executescript('''CREATE TABLE IF NOT EXISTS settings(device TEXT PRIMARY KEY, data TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS jobs(id TEXT PRIMARY KEY, device TEXT, source TEXT, state TEXT,
                created REAL, updated REAL, ticket TEXT, filename TEXT DEFAULT '', data TEXT NOT NULL);
                CREATE INDEX IF NOT EXISTS jobs_device ON jobs(device, created DESC);''')
            db.execute("UPDATE jobs SET state='failed', data=? WHERE state IN ('queued','capturing','analyzing')",
                       (json.dumps({'error': '网页服务重启，本次任务已中断，请重新拍照或重试'}),))
            db.commit()
            self.ready = True
        try:
            with db:
                yield db
        finally:
            db.close()

    def settings(self, device, db):
        row = db.execute('SELECT data FROM settings WHERE device=?', (device,)).fetchone()
        return validate(json.loads(row['data'])) if row else dict(DEFAULT)

    def save(self, device, value):
        value = validate(value)
        with self.lock, self.db() as db:
            db.execute('INSERT OR REPLACE INTO settings VALUES (?,?)', (device, json.dumps(value)))
            # 阶段：策略变更会取消未领取的租约，旧调度不能继续上传过期照片。
            db.execute("UPDATE jobs SET state='cancelled', updated=?, data=? WHERE device=? AND state IN ('queued','capturing')",
                       (self.clock(), json.dumps({'error': '采集配置已更改，请重新领取任务'}), device))
            self.prune(device, db, value['keep_count'])
            self.live.close(device)
            self.sound_events.pop(device, None)
        if self.on_settings:
            self.on_settings(device)
        return value

    def current_settings(self, device):
        with self.lock, self.db() as db:
            return self.settings(device, db)

    def sound_event(self, device, event):
        """仅由已认证音频上行调用；冷却内忽略，不在冷却结束后补拍旧声音。"""
        with self.lock, self.db() as db:
            settings = self.settings(device, db)
            if not settings['enabled'] or settings['paused'] or settings['capture_mode'] not in ('sound', 'interval_sound'):
                return False
            if not in_window(settings, self.clock()):
                return False
            old = self.sound_events.get(device, {})
            if old.get('event') == event:
                return False
            last = db.execute('SELECT created FROM jobs WHERE device=? ORDER BY created DESC LIMIT 1', (device,)).fetchone()
            if last and self.clock()-last['created'] < settings['cooldown_sec']:
                return False
            if device not in self.sound_events and len(self.sound_events) >= 64:
                self.sound_events.pop(next(iter(self.sound_events)))
            # 录音占用时保留至多30秒，结束后再拍；一次语音不会积累多个拍照任务。
            self.sound_events[device] = dict(event=str(event)[:96], at=self.clock(), pending=True)
            return True

    def expire(self, db):
        db.execute("UPDATE jobs SET state='expired', data=?, updated=? WHERE state IN ('queued','capturing') AND updated<?",
                   (json.dumps({'error': '拍照上传超时（60 秒），可重新触发'}), self.clock(), self.clock() - 60))

    def gate(self, device, settings, manual=False, allow_audio=False):
        if not manual and not settings['enabled']:
            return '自动采集未启用'
        if settings['paused']:
            return '采集已手动暂停'
        if not manual and not in_window(settings, self.clock()):
            return '当前不在启用时段（北京时间）'
        busy = self.blockers(device)
        for key, label in [('audio', '录音或音频上传中'), ('ota', 'OTA 更新中'), ('motion', '动作执行中')]:
            # 说话触发的要求是"一开口就拍"：录音占用不能把声音事件推迟到录音结束，
            # 只保留 OTA/动作这两类真正会抢同一套执行器的互斥。
            if key == 'audio' and allow_audio:
                continue
            if settings['block_' + key] and busy.get(key):
                return label + '，暂缓拍照'
        return ''

    def queue_device(self, device):
        with self.lock, self.db() as db:
            self.expire(db)
            settings = self.settings(device, db)
            reason = self.gate(device, settings, manual=True)
            if reason:
                raise CameraError(reason, 409)
            if db.execute("SELECT 1 FROM jobs WHERE device=? AND state IN ('queued','capturing','analyzing')", (device,)).fetchone():
                raise CameraError('该设备已有拍照或分析任务，请等待', 409)
            job = uuid.uuid4().hex; now = self.clock()
            db.execute('INSERT INTO jobs(id,device,source,state,created,updated,ticket,data) VALUES(?,?,?,?,?,?,?,?)',
                       (job, device, 'device', 'queued', now, now, '', json.dumps({'manual_request': True})))
            self.prune(device, db, settings['keep_count'])
            return {'ok': True, 'job_id': job, 'state': 'queued'}

    def report(self, device, value):
        if not isinstance(value, dict):
            raise CameraError('报告格式无效')
        with self.lock, self.db() as db:
            self.expire(db)
            row = db.execute('SELECT * FROM jobs WHERE id=? AND device=?', (value.get('job_id', ''), device)).fetchone()
            ticket = value.get('ticket', '')
            if not isinstance(ticket, str) or not row or row['source'] != 'device' or not secrets.compare_digest(row['ticket'], hashlib.sha256(ticket.encode()).hexdigest()):
                raise CameraError('拍照凭证无效', 409)
            if row['state'] == 'capturing':
                data = json.loads(row['data']); data['error'] = str(value.get('error', '设备拍照失败'))[:160]
                db.execute("UPDATE jobs SET state='failed', updated=?, data=? WHERE id=?",
                           (self.clock(), json.dumps(data), row['id']))
            return {'ok': True}

    def request_capture(self, device, source):
        with self.lock, self.db() as db:
            self.expire(db)
            settings = self.settings(device, db)
            queued = db.execute("SELECT * FROM jobs WHERE device=? AND source='device' AND state='queued' ORDER BY created LIMIT 1", (device,)).fetchone() if source == 'device' else None
            if queued:
                reason = self.gate(device, settings, manual=True)
                if reason:
                    return {'accepted': False, 'reason': reason}
                ticket = secrets.token_urlsafe(24)
                db.execute("UPDATE jobs SET state='capturing',updated=?,ticket=? WHERE id=?",
                           (self.clock(), hashlib.sha256(ticket.encode()).hexdigest(), queued['id']))
                return {'accepted': True, 'job_id': queued['id'], 'ticket': ticket, 'expires_in': 60,
                        'upload_url': '/api/camera/device/upload'}
            if settings['source'] != source:
                return {'accepted': False, 'reason': '当前采集来源为' + ('机器人相机' if settings['source'] == 'device' else '浏览器相机')}
            sound = self.sound_events.get(device, {})
            triggered = source == 'device' and sound.get('pending') and 0 <= self.clock()-sound['at'] <= 30
            # 声音事件就是"现在有人说话"；录音占用不推迟这一张，否则会变成
            # "说完才拍"，人已离开画面。
            reason = self.gate(device, settings, allow_audio=triggered)
            if reason:
                return {'accepted': False, 'reason': reason}
            row = db.execute('SELECT * FROM jobs WHERE device=? ORDER BY created DESC, rowid DESC LIMIT 1', (device,)).fetchone()
            active = db.execute("SELECT 1 FROM jobs WHERE device=? AND state IN ('queued','capturing','analyzing')", (device,)).fetchone()
            if active:
                return {'accepted': False, 'reason': '上一张仍在采集或分析，暂缓拍照'}
            interval_due = settings['capture_mode'] in ('interval', 'interval_sound') and (not row or self.clock()-row['created'] >= settings['interval_sec'])
            if row and self.clock() - row['created'] < settings['cooldown_sec']:
                if triggered:
                    # 冷却内不补拍：这次说话直接作废，等冷却结束后的新声音。
                    sound['pending'] = False
                return {'accepted': False, 'reason': '拍照冷却中', 'wait_sec': round(settings['cooldown_sec']-(self.clock()-row['created']), 1)}
            if not triggered and not interval_due:
                return {'accepted': False, 'reason': '等待麦克风触发' if settings['capture_mode'] == 'sound' else '尚未到拍照间隔',
                        'wait_sec': max(0, round(settings['interval_sec'] - (self.clock() - row['created']), 1)) if row else 0}
            job, ticket = uuid.uuid4().hex, secrets.token_urlsafe(24)
            now = self.clock()
            db.execute('INSERT INTO jobs(id,device,source,state,created,updated,ticket,data) VALUES(?,?,?,?,?,?,?,?)',
                       (job, device, source, 'capturing', now, now, hashlib.sha256(ticket.encode()).hexdigest(), json.dumps({'trigger': 'sound' if triggered else 'interval'})))
            if triggered:
                sound['pending'] = False
            self.prune(device, db, settings['keep_count'])
            return {'accepted': True, 'job_id': job, 'ticket': ticket, 'expires_in': 60,
                    'upload_url': '/api/camera/device/upload' if source == 'device' else '/api/camera/upload'}

    def public(self, row):
        data = json.loads(row['data'])
        return dict(id=row['id'], device=row['device'], source=row['source'], state=row['state'],
                    created=row['created'], updated=row['updated'], **data,
                    image_url='/api/camera/image/' + row['id'] if row['filename'] else None)

    def status(self, device):
        with self.lock, self.db() as db:
            self.expire(db)
            settings = self.settings(device, db)
            rows = db.execute('SELECT * FROM jobs WHERE device=? ORDER BY created DESC, rowid DESC LIMIT 50', (device,)).fetchall()
            reason = self.gate(device, settings)
            last = self.seen.get(device, 0)
            sound = self.sound_events.get(device, {})
            newest = rows[0] if rows else None
            cooldown_left = 0.0
            if newest:
                cooldown_left = max(0.0, settings['cooldown_sec'] - (self.clock() - newest['created']))
            return dict(settings=settings, items=[self.public(row) for row in rows], face_engine=self.faces.state(),
                        timezone='Asia/Shanghai', hardware_ready=bool(last and self.clock() - last < 20),
                        hardware_last_seen=last or None, reason=reason or '允许采集，等待相机请求',
                        protocol_version=1, image_limit_bytes=MAX_IMAGE_BYTES, follow=self.follow_status(device),
                        sound_trigger=dict(pending=bool(sound.get('pending')),
                                           at=sound.get('at'), trigger=sound.get('event', ''),
                                           cooldown_sec=settings['cooldown_sec'],
                                           cooldown_remaining=round(cooldown_left, 1)))

    def prune(self, device, db, keep):
        rows = db.execute("SELECT id,filename FROM jobs WHERE device=? AND state NOT IN ('queued','capturing','analyzing') ORDER BY created DESC, rowid DESC", (device,)).fetchall()
        for row in rows[keep:]:
            if row['filename']:
                (self.root / row['filename']).unlink(missing_ok=True)
            db.execute('DELETE FROM jobs WHERE id=?', (row['id'],))

    def upload(self, device, raw, job_id='', ticket='', source='manual'):
        wire_crc = zlib.crc32(raw)
        with self.lock, self.db() as db:
            self.expire(db)
            settings = self.settings(device, db)
            previous = {}
            if source != 'manual':
                row = db.execute('SELECT * FROM jobs WHERE id=? AND device=?', (job_id, device)).fetchone()
                if not row or row['source'] != source or not secrets.compare_digest(
                        row['ticket'], hashlib.sha256(ticket.encode()).hexdigest()):
                    raise CameraError('拍照任务不存在、已失效或凭证不匹配', 409)
                previous = json.loads(row['data'])
                if row['filename'] and row['state'] in ('stored','analyzing','done','failed'):
                    if previous.get('wire_crc32') == wire_crc and previous.get('wire_bytes') == len(raw):
                        return self.public(row)  # Lost HTTP response: retry is idempotent.
                    raise CameraError('该任务已有其他照片，请重新请求拍照', 409)
                if row['state'] != 'capturing':
                    raise CameraError('拍照任务已失效', 409)
                reason = self.gate(device, settings, manual=previous.get('manual_request', False),
                                   allow_audio=previous.get('trigger') == 'sound')
                if reason:
                    raise CameraError(reason, 409)
            else:
                if db.execute("SELECT 1 FROM jobs WHERE device=? AND state IN ('queued','capturing','analyzing')", (device,)).fetchone():
                    raise CameraError('上一张仍在采集或分析，请等待', 409)
                job_id = uuid.uuid4().hex
            data, width, height = normalize_image(raw)
            analyze = settings['analyze']
            if analyze and not self.slots.acquire(blocking=False):
                raise CameraError('分析队列已满，请稍后再试', 429)
            filename = job_id + '.jpg'
            details = dict(width=width, height=height, bytes=len(data), wire_crc32=wire_crc, wire_bytes=len(raw), provider='local-face',
                           analysis=None, error='', received_at=self.clock(), trigger=previous.get('trigger', ''),
                           note='手动上传测试' if source == 'manual' else '')
            try:
                (self.root / filename).write_bytes(data)
                now = self.clock()
                if source == 'manual':
                    db.execute('INSERT INTO jobs(id,device,source,state,created,updated,ticket,filename,data) VALUES(?,?,?,?,?,?,?,?,?)',
                               (job_id, device, source, 'analyzing' if analyze else 'stored', now, now, '', filename, json.dumps(details)))
                else:
                    db.execute('UPDATE jobs SET state=?,updated=?,filename=?,data=? WHERE id=?',
                               ('analyzing' if analyze else 'stored', now, filename, json.dumps(details), job_id))
                self.prune(device, db, settings['keep_count'])
                # 阶段：先提交接收记录，再让识别线程打开自己的数据库连接，避免读到未提交状态。
                db.commit()
            except Exception:
                if analyze:
                    self.slots.release()
                (self.root / filename).unlink(missing_ok=True)
                raise
            if analyze:
                threading.Thread(target=self.analyze_job, args=(job_id, settings['provider']), daemon=True,
                                 name='camera-analysis').start()
            return self.public(db.execute('SELECT * FROM jobs WHERE id=?', (job_id,)).fetchone())

    def analyze_job(self, job_id, provider_name):
        try:
            with self.lock, self.db() as db:
                row = db.execute('SELECT * FROM jobs WHERE id=?', (job_id,)).fetchone()
                if not row:
                    return
                data = json.loads(row['data'])
                raw = (self.root / row['filename']).read_bytes()
                projection = self.settings(row['device'], db)
            try:
                data['analysis'] = local_faces.add_targeting(self.faces.analyze(raw),
                                                           (data['width'], data['height']), projection)
                state, data['error'] = 'done', ''
            except (CameraError, ValueError) as error:
                state, data['error'] = 'failed', str(error)
            except Exception:
                state, data['error'] = 'failed', '分析任务异常，可重试或删除记录'
            with self.lock, self.db() as db:
                db.execute('UPDATE jobs SET state=?,updated=?,data=? WHERE id=?',
                           (state, self.clock(), json.dumps(data), job_id))
                self.prune(row['device'], db, self.settings(row['device'], db)['keep_count'])
            if self.on_analysis:
                self.on_analysis(row['device'], job_id, data.get('received_at', row['created']), data.get('analysis') or {},
                                 projection, row['source'] if state == 'done' else 'failed')
        finally:
            self.slots.release()

    def retry(self, job_id):
        with self.lock, self.db() as db:
            row = db.execute('SELECT * FROM jobs WHERE id=?', (job_id,)).fetchone()
            if not row or not row['filename']:
                raise CameraError('找不到已上传的图片', 404)
            if row['state'] == 'analyzing':
                raise CameraError('该图片仍在分析中', 409)
            if db.execute("SELECT 1 FROM jobs WHERE device=? AND id!=? AND state IN ('queued','capturing','analyzing')",
                          (row['device'], job_id)).fetchone():
                raise CameraError('该设备仍有采集或分析任务，请稍后重试', 409)
            provider = self.settings(row['device'], db)['provider']
            if not self.slots.acquire(blocking=False):
                raise CameraError('分析队列已满', 429)
            data = json.loads(row['data']); data.update(provider=provider, error='', analysis=None)
            db.execute('UPDATE jobs SET state=?,updated=?,data=? WHERE id=?', ('analyzing', self.clock(), json.dumps(data), job_id))
            db.commit()
            threading.Thread(target=self.analyze_job, args=(job_id, provider), daemon=True, name='camera-analysis').start()

    def delete(self, job_id):
        with self.lock, self.db() as db:
            row = db.execute('SELECT * FROM jobs WHERE id=?', (job_id,)).fetchone()
            if not row:
                raise CameraError('记录不存在', 404)
            if row['state'] in ('queued', 'capturing', 'analyzing'):
                raise CameraError('任务进行中，请先等待结束或暂停采集', 409)
            if row['filename']:
                (self.root / row['filename']).unlink(missing_ok=True)
            db.execute('DELETE FROM jobs WHERE id=?', (job_id,))


def blueprint(workspace):
    bp = Blueprint('camera_workspace', __name__)

    @bp.errorhandler(CameraError)
    def error(exc):
        return jsonify(error=str(exc)), exc.code

    @bp.errorhandler(RequestEntityTooLarge)
    def too_large(exc):
        return jsonify(error='图片请求超过大小上限（图片最多 4 MiB）'), 413

    def device():
        return device_id(request.args.get('device_id', 'esp-bridge'))

    @bp.route('/api/camera/status')
    def status():
        return jsonify(workspace.status(device()))

    @bp.route('/api/camera/settings', methods=['POST'])
    def settings():
        return jsonify(ok=True, settings=workspace.save(device(), request.get_json(silent=True)))

    @bp.route('/api/camera/faces/state')
    def face_state():
        return jsonify(workspace.faces.state())

    @bp.route('/api/camera/faces/settings', methods=['POST'])
    def face_settings():
        value = request.get_json(silent=True)
        try:
            return jsonify(workspace.faces.get().save_settings(value))
        except ValueError as error:
            raise CameraError(str(error))

    @bp.route('/api/camera/faces/enroll', methods=['POST'])
    def face_enroll():
        request.max_content_length = 10 * MAX_IMAGE_BYTES + 128 * 1024
        uploads = request.files.getlist('photos')
        if not 1 <= len(uploads) <= 10:
            raise CameraError('每次登记需上传 1~10 张单人清晰照片')
        raw = [normalize_image(item.stream.read(MAX_IMAGE_BYTES + 1))[0] for item in uploads]
        try:
            return jsonify(workspace.faces.get().enroll(request.form.get('name'), raw,
                           request.form.get('person_id') or None))
        except ValueError as error:
            raise CameraError(str(error))

    @bp.route('/api/camera/faces/people/<person_id>', methods=['DELETE'])
    def face_delete(person_id):
        try:
            return jsonify(workspace.faces.get().delete_person(person_id))
        except ValueError as error:
            raise CameraError(str(error))

    @bp.route('/api/camera/capture', methods=['POST'])
    def capture():
        return jsonify(workspace.request_capture(device(), 'browser'))

    @bp.route('/api/camera/request-device', methods=['POST'])
    def request_device():
        return jsonify(workspace.queue_device(device())), 202

    @bp.route('/api/camera/device/report', methods=['POST'])
    def device_report():
        return jsonify(workspace.report(device(), request.get_json(silent=True)))

    @bp.route('/api/camera/device/poll', methods=['POST'])
    def poll():
        value = request.get_json(silent=True) or {}
        if value.get('ready') is not True or value.get('protocol_version') != 1:
            raise CameraError('设备需上报 ready=true 和 protocol_version=1')
        ident = device()
        transport = workspace.current_settings(ident)['camera_transport']
        if value.get('camera_type') != transport:
            return jsonify(accepted=False, reason='当前仅接收 ESP32-S3 相机，旧MCU拍照链路已停用')
        workspace.seen[ident] = workspace.clock()
        settings = workspace.current_settings(ident)
        if settings['capture_mode'] == 'continuous':
            if value.get('continuous_capable') is not True:
                return jsonify(accepted=False, reason='连续模式需要 S3 -04 或更新固件')
            # 用户手动拍照优先；保存一张历史记录后恢复连续帧，不把视频写成无穷照片。
            with workspace.lock, workspace.db() as db:
                queued = db.execute("SELECT 1 FROM jobs WHERE device=? AND state='queued'", (ident,)).fetchone()
            if not queued:
                return jsonify(workspace.live.grant(ident, settings))
        return jsonify(workspace.request_capture(ident, 'device'))

    @bp.route('/api/camera/device/live-frame', methods=['POST'])
    def live_frame():
        received_at = workspace.clock()  # 从接收请求体之前计时，慢上传不产生新鲜的跟随观测。
        request.max_content_length = MAX_IMAGE_BYTES
        if request.mimetype != 'image/jpeg':
            raise CameraError('连续上传必须为 JPEG')
        try:
            sequence = int(request.headers.get('X-Frame-Sequence', ''))
        except ValueError:
            raise CameraError('缺少有效帧序号')
        return jsonify(workspace.live.receive(device(), request.stream.read(MAX_IMAGE_BYTES+1),
                       request.headers.get('X-Capture-Ticket', ''), sequence, received_at)), 202

    @bp.route('/api/camera/live/status')
    def live_status():
        return jsonify(workspace.live.status(device()))

    @bp.route('/api/camera/live/image')
    def live_image():
        ident = device()
        with workspace.lock:
            if request.args.get('kind') == 'analyzed' and request.args.get('sequence'):
                state = workspace.live.status(ident)
                if str(state.get('analysis_sequence')) != request.args['sequence']:
                    raise CameraError('分析帧已更新，请读取最新状态', 409)
            raw = workspace.live.image(ident, request.args.get('kind') == 'analyzed')
        if raw is None:
            raise CameraError('尚无新鲜连续画面', 404)
        response = send_file(io.BytesIO(raw), mimetype='image/jpeg')
        response.headers['Cache-Control'] = 'private, no-store'
        response.headers['X-Content-Type-Options'] = 'nosniff'
        return response

    @bp.route('/api/camera/upload', methods=['POST'])
    @bp.route('/api/camera/device/upload', methods=['POST'])
    def upload():
        request.max_content_length = MAX_IMAGE_BYTES + 128 * 1024
        if request.content_length and request.content_length > MAX_IMAGE_BYTES + 128 * 1024:
            raise CameraError('图片超过 4 MiB', 413)
        is_device = request.path.endswith('/device/upload')
        if is_device:
            if request.mimetype != 'image/jpeg':
                raise CameraError('S3设备照片必须为 JPEG', 415)
            raw = request.stream.read(MAX_IMAGE_BYTES + 1)
            source = 'device'
        else:
            item = request.files.get('image')
            if not item:
                raise CameraError('请选择图片')
            raw = item.stream.read(MAX_IMAGE_BYTES + 1)
            source = 'browser' if request.args.get('job_id') else 'manual'
        result = workspace.upload(device(), raw, request.args.get('job_id', ''),
                                  request.headers.get('X-Capture-Ticket', ''), source)
        if is_device:
            return jsonify(ok=True, job_id=result['id'], received=len(raw), crc32=zlib.crc32(raw)), 202
        return jsonify(ok=True, item=result), 202

    @bp.route('/api/camera/image/<job_id>')
    def image(job_id):
        with workspace.lock, workspace.db() as db:
            row = db.execute('SELECT filename FROM jobs WHERE id=?', (job_id,)).fetchone()
            if not row or not row['filename'] or not (workspace.root / row['filename']).is_file():
                raise CameraError('图片不存在或已按保留策略清理', 404)
            # 阶段：在工作区锁内读取已经限制尺寸并归一化的 JPEG；这样保留策略不会在
            # 慢速 Windows 客户端下载期间删除同一个文件，响应拿到独立内存后再释放锁。
            response = send_file(io.BytesIO((workspace.root / row['filename']).read_bytes()),
                                 mimetype='image/jpeg', download_name=row['filename'])
            response.headers['Cache-Control'] = 'private, no-store'
            response.headers['X-Content-Type-Options'] = 'nosniff'
            return response

    @bp.route('/api/camera/jobs/<job_id>/retry', methods=['POST'])
    def retry(job_id):
        workspace.retry(job_id)
        return jsonify(ok=True), 202

    @bp.route('/api/camera/jobs/<job_id>', methods=['DELETE'])
    def delete(job_id):
        workspace.delete(job_id)
        return jsonify(ok=True)

    return bp
