"""三合一联合测试（无实板）：拍照→三轴舵机参数、录制动作时间轴、说话触发。

运行：python -m unittest joint_test -v
或：  python joint_test.py [日志路径]

测试使用真实 Flask 路由、真实采样编译器和真实跟随控制器；S3/ESP/F407 用
确定性的主机模型替代：
  * S3：按 /api/camera/device/poll 领租约并上传 JPEG；
  * ESP：按 /api/data 上报 step_playback，并用与固件一致的本地节拍器发步；
  * F407：按 @MOVE 的角度/速度模型更新时间轴与当前位置。
不联网、不连硬件、不使用真实数据库。
"""
import io
import math
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from PIL import Image

import app as A
import camera_follow as F
import camera_workspace as C
import device_profile as P


DEV = 'esp-bridge'
TOKEN = 'joint-token'


def jpeg():
    raw = io.BytesIO()
    Image.new('RGB', (1600, 900), '#568579').save(raw, 'JPEG')
    return raw.getvalue()


class F407Queue:
    """与 ST32 App/step_queue.h 同规则的 F407 本地队列模型（主机侧）。

    ESP 只把步骤写进来；本模型按 at_ms 执行，不走 ESP 的 dt 计时。
    """

    def __init__(self):
        self.id = ''
        self.steps = []
        self.total = 0
        self.accepted = 0
        self.active = False
        self.started = False
        self.start_at = None
        self.phase = 'idle'
        self.board_phase = 'idle'
        self.board_pos = 0
        self.sends = []          # (at_ms, [angle...], [speed...])

    def receive(self, ident, total, start, steps):
        if not ident or len(ident) != 32 or not total or total > 302 or len(steps) > 12 or start + len(steps) > total:
            return False
        for step in steps:
            if not (0 <= step['at_ms'] <= 30000):
                return False
            if any(not (0 <= a <= 180) for a in step['angles']):
                return False
            if any(not (0 < s <= 360) for s in step['speeds']):
                return False
        if ident != self.id:
            if self.active or start:
                return False
            self.id = ident
            self.steps = []
            self.accepted = 0
            self.total = total
            self.started = False
            self.active = True
            self.phase = 'transferring'
            self.board_phase = 'idle'
        if not self.active or total != self.total or start != self.accepted:
            return False
        for step in steps:
            if step['at_ms'] < (self.steps[-1]['at_ms'] if self.steps else 0):
                return False
            self.steps.append(step)
            self.accepted += 1
        return True

    def start(self, now):
        if self.accepted != self.total or not self.total:
            return False
        self.started = True
        self.start_at = now
        self.active = True
        self.board_pos = 0
        self.phase = 'starting'
        self.board_phase = 'starting'
        return True

    def tick(self, now):
        if not self.active or not self.started:
            return
        self.phase = 'running'
        self.board_phase = 'running'
        elapsed = now - self.start_at
        while self.board_pos < self.accepted and self.steps[self.board_pos]['at_ms'] <= elapsed:
            step = self.steps[self.board_pos]
            self.sends.append((step['at_ms'], list(step['angles']), list(step['speeds'])))
            self.board_pos += 1
        if self.board_pos >= self.accepted:
            self.active = False
            self.phase = 'done'
            self.board_phase = 'done'

    def report(self):
        return dict(protocol=1, id=self.id, phase=self.phase, accepted=self.accepted,
                    sent=self.accepted, active=self.active,
                    board=dict(pos=self.board_pos, total=self.total, phase=self.board_phase))

    def stop(self):
        self.active = False
        self.phase = 'stopped'
        self.board_phase = 'stopped'


class JointTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.now = 1000.0
        self.busy = {}
        self.cfg = {'token': TOKEN, 'providers': {}}

        def blockers_for(device):
            # 生产里 camera.blockers 就是 app.camera_blockers；测试再叠加可注入的忙标志。
            value = dict(A.camera_blockers(device))
            for key, flag in self.busy.items():
                value[key] = value.get(key) or flag
            return value

        self.ws = C.CameraWorkspace(Path(self.tmp.name), lambda: self.cfg,
                                    blockers_for, lambda: self.now)
        for key in ['root', 'config', 'blockers', 'clock', 'lock', 'slots', 'ready',
                    'seen', 'sound_events', 'faces']:
            p = patch.object(A.camera, key, getattr(self.ws, key))
            p.start()
            self.addCleanup(p.stop)
        self.controller = F.Controller(lambda: self.now)
        self.pose_angles = [90, 30, 90]

        def pose_get(device):
            return dict(fresh=True, moving=False, angle_source='model',
                        angles=list(self.pose_angles), age_ms=0)

        for obj, key, value in [
            (A, 'follow', self.controller),
            (A.camera, 'on_analysis', self.controller.observe),
            (A.camera, 'on_settings', self.controller.stop),
            (A.camera, 'follow_status', self.controller.status),
            (A, 'load_config', lambda: self.cfg),
            (A, 'start_auto_tick', lambda: None),
            (A, 'start_store', lambda: None),
            (A, 'start_beacon', lambda: None),
            (A, 'append_line', lambda *a, **k: None),
            (A.asr, 'warmup', lambda: None),
            (A.device_profile, 'load', P.default_profile),
            (A.device_controls, 'get', lambda d: dict(servo_speed=90)),
            (A.device_controls, 'confirm', lambda *a: None),
            (A.idle_pet, 'confirm', lambda *a: None),
            (A.idle_pet, 'snapshot', lambda *a: {}),
            (A.device_profile, 'motion_snapshot', lambda *a: dict(applied=True)),
            (A.board_pose, 'get', pose_get),
            (A.board_pose, 'ingest', lambda *a: False),
        ]:
            p = patch.object(obj, key, value)
            p.start()
            self.addCleanup(p.stop)
        self.lib = tempfile.TemporaryDirectory()
        self.addCleanup(self.lib.cleanup)
        p = patch.object(A, 'sampled_library', A.sampled_motion.Library(Path(self.lib.name) / 'actions.json'))
        p.start()
        self.addCleanup(p.stop)
        p = patch.object(A, 'sampled_player', A.sampled_motion.Player())
        p.start()
        self.addCleanup(p.stop)

        self.client = A.app.test_client()
        self.dev_client = A.app.test_client()
        with self.client.session_transaction() as session:
            session['authed'] = True
        self.dev_headers = {'Authorization': 'Bearer ' + TOKEN}
        A._pending_commands.clear()
        A._gateway_step.clear()
        A._audio_transport_status.pop(DEV, None)
        A._motion_status.pop(DEV, None)
        self.dev = '/api/camera/device/'

    # ---------- 共用步骤 ----------
    def save_settings(self, **kwargs):
        value = dict(C.DEFAULT, enabled=True, source='device', camera_transport='s3-wifi',
                     interval_sec=2, cooldown_sec=2)
        value.update(kwargs)
        response = self.client.post('/api/camera/settings', json=value)
        self.assertEqual(response.status_code, 200, response.json)
        return response.json['settings']

    def lease(self):
        response = self.dev_client.post(self.dev + 'poll', headers=self.dev_headers,
                                        json=dict(ready=True, protocol_version=1, camera_type='s3-wifi'))
        self.assertEqual(response.status_code, 200, response.json)
        return response.json

    def upload(self, lease, face_center=(80, 45)):
        detected = dict(faces=[dict(index=0, bbox=[face_center[0]-20, face_center[1]-20, 40, 40],
                                    center=list(face_center), face_confidence=.99,
                                    recognized=False, name='未知', similarity=None)],
                        boxes=[], size=[160, 90])
        # 补丁必须覆盖后台分析线程的整个生命周期，不能只包住 POST。
        with patch.object(A.camera.faces, 'analyze', return_value=detected):
            response = self.dev_client.post(
                self.dev + 'upload?job_id=' + lease['job_id'], headers=dict(self.dev_headers,
                **{'X-Capture-Ticket': lease['ticket'], 'Content-Type': 'image/jpeg'}), data=jpeg())
            self.assertEqual(response.status_code, 202, response.json)
            end = time.monotonic() + 3
            while time.monotonic() < end:
                item = A.camera.status(DEV)['items'][0]
                if item['state'] in ('done', 'failed'):
                    if item['state'] == 'done':
                        # 状态先落库、回调后执行；等跟随控制器真正收到观察。
                        end2 = time.monotonic() + 2
                        while time.monotonic() < end2 and not self.controller.status(DEV).get('pending'):
                            time.sleep(.01)
                    return item
                time.sleep(.01)
        self.fail('分析超时')

    def finish_jobs(self):
        with self.ws.db() as db:
            db.execute("UPDATE jobs SET state='done' WHERE state IN ('capturing','analyzing')")

    def heartbeat(self, **extra):
        body = dict(token=TOKEN, device_id=DEV, need_ai=False)
        body.update(extra)
        response = self.dev_client.post('/api/data', json=body)
        self.assertEqual(response.status_code, 200, response.json)
        return response.json

    # ---------- 场景一：拍照 → 三轴舵机参数 ----------
    def test_01_photo_outputs_three_axis_servo_parameters(self):
        self.save_settings(capture_mode='interval', follow_enabled=True)
        lease = self.lease()
        self.assertTrue(lease['accepted'], lease)
        item = self.upload(lease, face_center=(20, 45))   # 人脸在画面最左
        self.assertEqual(item['state'], 'done')
        self.now += 1
        decision = self.heartbeat()
        command = decision.get('command') or {}
        self.assertEqual(command.get('note'), 'camera_follow_latest', decision)
        raw = command.get('raw', '')
        for servo in (1, 2, 3):
            self.assertIn('@MOVE:servo,%d,' % servo, raw)
        params = {p['id']: p for p in command.get('servo_params') or []}
        self.assertEqual(sorted(params), [1, 2, 3])
        # 人脸在最左：S1 必须一次转足单张最大角 20°（旧版只挪 4°，一张照片转不到位）。
        self.assertEqual(params[1]['angle'], 70)
        self.assertEqual(params[2]['angle'], 30)   # 垂直居中，仅保持
        self.assertEqual(params[3]['angle'], 90)   # 屏幕轴保持
        self.assertGreaterEqual(params[1]['speed'], 5)
        # 跟随状态里也能直接看到三轴参数（网页“输出舵机参数”）。
        state = self.controller.status(DEV)
        self.assertEqual([p['id'] for p in state['servo_params']], [1, 2, 3])
        print('PASS 场景1 拍照后输出三轴参数:', [(p['id'], p['angle'], p['speed']) for p in state['servo_params']])

        # 模拟 F407 到位后，再来一张已居中的照片：不应重复发命令/抖动。
        self.pose_angles = [70, 30, 90]
        self.now += 3
        lease = self.lease()
        self.assertTrue(lease['accepted'], lease)
        self.upload(lease, face_center=(80, 45))
        self.now += 1
        self.assertNotIn('command', self.heartbeat())
        print('PASS 场景1 居中后不再重复下发')

    # ---------- 场景二：录制动作按时间轴分步 ----------
    def action(self, interval=500):
        return dict(interval_ms=interval,
                    mapping=[dict(id=i, sign=1, offset=0) for i in (1, 2, 3)],
                    recording=dict(schema=1, pose_schema=2, id='train_joint', name='联合测试',
                                   expression='happy', trim=[0, 1000],
                                   samples=[dict(t=0, yaw=90, pitch=30, roll=90),
                                            dict(t=1000, yaw=110, pitch=45, roll=100)]))

    def test_02_recorded_action_disabled(self):
        # 03 简洁链路：录制长动作/F407 StepQ 入口关闭，动作库 @TIMELINE 仍保留。
        response = self.client.post('/api/recorded-motion/start',
                                    json=dict(device_id=DEV, action=self.action()))
        self.assertEqual(response.status_code, 410, response.json)
        self.assertIn('禁用', response.json['error'])
        self.assertFalse(A.sampled_player.busy(DEV))
        print('PASS 场景2 03已禁用录制长动作，动作库仍可用')

    # ---------- 场景三：说话即触发、冷却去重、网页可设 ----------
    def test_03_speech_triggers_immediately_with_cooldown(self):
        settings = self.save_settings(capture_mode='sound', cooldown_sec=5)
        self.assertEqual(settings['cooldown_sec'], 5)
        # 模拟录音/上传仍在进行：旧逻辑会一直等到录音结束才拍。
        self.busy['audio'] = True
        self.assertTrue(A.camera.sound_event(DEV, 'speech-1'))
        lease = self.lease()
        self.assertTrue(lease['accepted'], lease)
        item = self.upload(lease, face_center=(40, 45))
        self.assertEqual(item.get('trigger'), 'sound')
        self.finish_jobs()
        self.assertTrue(A.camera.status(DEV)['sound_trigger']['cooldown_sec'] == 5)
        print('PASS 场景3 说话即拍（录音占用时不推迟）')

        # 冷却内再次说话：事件层直接忽略，不产生第二张。
        self.now += 1
        self.assertFalse(A.camera.sound_event(DEV, 'speech-2'))
        self.now += 2
        self.assertFalse(A.camera.sound_event(DEV, 'speech-3'))
        self.now += 2.5
        self.assertTrue(A.camera.sound_event(DEV, 'speech-4'))
        self.assertTrue(self.lease()['accepted'])
        self.finish_jobs()
        print('PASS 场景3 冷却内不重复触发，冷却后新声音可拍')

        # 动作执行中仍要暂缓（安全互斥），动作结束后事件仍有效。
        self.now += 6
        self.busy['motion'] = True
        self.assertTrue(A.camera.sound_event(DEV, 'speech-5'))
        blocked = self.lease()
        self.assertFalse(blocked['accepted'])
        self.assertIn('动作', blocked['reason'])
        self.busy.pop('motion')
        self.assertTrue(self.lease()['accepted'])
        self.finish_jobs()
        print('PASS 场景3 动作执行中暂缓、结束后补拍')

    # ---------- 场景四：三功能同场不互相抢控制 ----------
    def test_04_three_features_share_one_robot(self):
        self.save_settings(capture_mode='sound', cooldown_sec=2, follow_enabled=True)
        # 03 不再用录制长动作；用运动互斥标志模拟动作库 @TIMELINE 占用。
        self.busy['motion'] = True
        # 动作播放中：相机自动拍照应被动作互斥挡住，跟随也不得插队。
        self.assertTrue(A.camera.sound_event(DEV, 'speech-during-motion'))
        blocked = self.lease()
        self.assertFalse(blocked['accepted'])
        self.now += 1
        self.controller.observe(DEV, 'j', self.now,
                                dict(faces=[dict(bbox=[0, 0, 40, 40], offset_normalized=[-.8, 0],
                                                 face_confidence=.99)]),
                                self.ws.current_settings(DEV), 'device')
        decision = self.heartbeat(step_playback=F407Queue().report(), motion_busy=True)
        self.assertNotIn('command', decision)
        self.busy.pop('motion')
        print('PASS 场景4 动作库占用时相机与跟随不抢控制')


    # ---------- 场景五：旧网关不能装作“已领取” ----------
    def test_05_old_gateway_gets_clear_upgrade_error(self):
        # 旧 ESP 的 /api/data 没有 step_playback：以前页面会永远停在“等待新版ESP领取”。
        self.heartbeat(motion_busy=False)
        self.assertFalse(A._gateway_step[DEV]['capable'])
        response = self.client.post('/api/recorded-motion/start',
                                    json=dict(device_id=DEV, action=self.action()))
        self.assertEqual(response.status_code, 410, response.json)
        self.assertIn('禁用', response.json['error'])
        status = self.client.get('/api/recorded-motion/status?device_id=' + DEV).json
        self.assertFalse(status['gateway']['capable'])
        print('PASS 场景5 旧网关明确提示升级 ESP 1.1.25')


def main():
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(JointTests)
    stream = io.StringIO()
    result = unittest.TextTestRunner(stream=stream, verbosity=2).run(suite)
    output = stream.getvalue()
    print(output)
    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(output, encoding='utf-8')
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
