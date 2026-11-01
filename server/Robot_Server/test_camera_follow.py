"""相机跟随的时效、互斥、校准和真实 HTTP 链路回归。

运行 python -m unittest test_camera_follow -v。测试使用临时数据库和可控时钟，
不连接硬件、在线AI或用户数据库；基础相机用例另含真实本地人脸模型验证。
"""
import copy
import unittest
from unittest.mock import patch

import camera_follow as F
import camera_workspace as C
import device_profile as P
import test_camera_workspace as fixtures
import app as A


def face(x=.8, y=.5, area=40, **extra):
    return dict(bbox=[0, 0, area, area], offset_normalized=[x, y],
                face_confidence=.99, recognized=False, **extra)


class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.now = 100.
        self.f = F.Controller(lambda: self.now)
        self.s = dict(C.DEFAULT, enabled=True, follow_enabled=True)
        self.pose = dict(fresh=True, moving=False, angles=[90, 30, 90], age_ms=0)
        self.profile = P.effective(P.default_profile(), 90)

    def observe(self, faces=None, **kw):
        self.f.observe('d', kw.get('job', 'j'), kw.get('created', self.now),
                       dict(faces=faces if faces is not None else [face()]),
                       self.s, kw.get('source', 'device'))

    def take(self, **kw):
        return self.f.take('d', self.s, kw.get('pose', self.pose),
                           self.profile, kw.get('blockers', {}))

    def test_latest_only_and_one_shot(self):
        for i in range(100):
            self.observe(job=str(i))
        self.assertEqual(len(self.f.latest), 1)
        cmd = self.take()
        self.assertEqual(cmd['camera_job'], '99')
        self.assertEqual([m['angle'] for m in cmd['moves']], [110, 19, 90])
        self.assertIsNone(self.take())
        self.observe(); self.assertIsNone(self.take())  # 冷却期，不追加动作。
        self.now += 2; self.observe(); self.assertIsNotNone(self.take())

    def test_largest_and_known_person_policy(self):
        self.observe([face(x=-.8, area=20), face(x=.8, area=60)])
        self.assertEqual(self.take()['moves'][0]['angle'], 110)
        self.now += 2
        self.s.update(follow_target='known', follow_person='alice')
        self.observe([face(area=80, person_id='bob'),
                      face(x=-.8, area=20, person_id='alice')])
        self.assertIsNone(self.take())
        known = face(x=-.8, area=20, person_id='alice'); known['recognized'] = True
        self.observe([face(area=80, person_id='bob'), known])
        self.assertEqual(self.take()['moves'][0]['angle'], 70)

    def test_stale_or_non_device_images_never_move(self):
        for kw in [dict(created=94), dict(created=101), dict(source='manual'), dict(source='browser')]:
            self.observe(**kw); self.assertIsNone(self.take())
        self.observe(); self.now += 6; self.assertIsNone(self.take())

    def test_pose_ack_and_motion_guards(self):
        for change in [dict(fresh=False), dict(moving=True), dict(angles=[90, None, 90])]:
            self.observe(); self.assertIsNone(self.take(pose=dict(self.pose, **change)))
        self.observe(); self.assertIsNotNone(self.take())
        self.now += 2; self.observe()
        self.assertIsNone(self.take(pose=dict(self.pose, age_ms=2000)))

    def test_audio_ota_manual_and_stop_discard_pending(self):
        for key in ['audio', 'ota', 'motion', 'policy']:
            self.observe(); self.assertIsNone(self.take(blockers={key: True}))
            self.assertIsNone(self.take())
        self.observe(); self.f.stop('d', hold=5); self.assertIsNone(self.take())
        self.now += 1; self.observe(); self.assertIsNone(self.take())
        self.now += 5; self.observe(); self.assertIsNotNone(self.take())

    def test_admin_calibration_speed_and_deadzone(self):
        self.profile['servos'][0].update(min=88, max=92, speed=12)
        self.profile['servos'][1].update(min=29, max=31, speed=15)
        self.observe()
        cmd = self.take()
        self.assertEqual([(m['angle'], m['speed']) for m in cmd['moves']][:2], [(92, 12), (29, 15)])
        self.assertEqual(cmd['moves'][2]['angle'], 90)
        self.now += 2; self.observe([face(x=.05, y=.05)]); self.assertIsNone(self.take())

    def test_axis_sign_rotation_and_configuration_change(self):
        self.s['follow_yaw_sign'] = -1
        self.observe([face(x=.8, y=0)])
        self.assertEqual(self.take()['moves'][0]['angle'], 70)
        self.now += 2; self.s.update(follow_roll_deg=90, follow_yaw_sign=1)
        self.observe([face(x=.8, y=0)])
        moves = self.take()['moves']
        self.assertEqual([m['id'] for m in moves], [1, 2, 3])
        self.assertEqual(moves[0]['angle'], 90)  # 水平偏差旋转后仅俯仰修正。
        self.now += 2; self.observe(); self.s['paused'] = True; self.assertIsNone(self.take())
        self.s['paused'] = False; self.observe(); self.s['follow_gain'] = .2
        self.assertIsNone(self.take())

    def test_validation_disabled_and_low_confidence(self):
        for change in [dict(follow_enabled='yes'), dict(follow_step=True), dict(follow_speed=100),
                       dict(follow_gain=float('nan')), dict(follow_yaw_sign=0), dict(follow_step=1.5)]:
            with self.assertRaises(ValueError):
                F.validate(dict(self.s, **change))
        self.s['follow_enabled'] = False; self.observe(); self.assertIsNone(self.take())
        self.s['follow_enabled'] = True
        weak = face(); weak['face_confidence'] = .79
        self.observe([weak]); self.assertIsNone(self.take())


class FollowAPITests(fixtures.CameraTests):
    """继承原15条用例，一起证明旧相机链路仍可使用。"""
    def setUp(self):
        super().setUp()
        self.controller = F.Controller(lambda: self.now)
        for obj, key, value in [(A, 'follow', self.controller),
                               (A.camera, 'on_analysis', self.controller.observe),
                               (A.camera, 'on_settings', self.controller.stop),
                               (A.camera, 'follow_status', self.controller.status),
                               (A, 'append_line', lambda *a: None),
                               (A, 'camera_blockers', lambda d: self.busy),
                               (A.device_controls, 'confirm', lambda *a: None),
                               (A.idle_pet, 'confirm', lambda *a: None),
                               (A.device_profile, 'load', lambda: P.default_profile()),
                               (A.device_controls, 'get', lambda d: dict(servo_speed=90)),
                               (A.device_profile, 'motion_snapshot', lambda *a: dict(applied=True)),
                               (A.idle_pet, 'snapshot', lambda *a: {}),
                               (A.board_pose, 'ingest', lambda *a: None),
                               (A.board_pose, 'get', lambda d: dict(fresh=True, moving=False, angles=[90, 30, 90], age_ms=0))]:
            p = patch.object(obj, key, value); p.start(); self.addCleanup(p.stop)
        A._pending_commands.clear()

    def heartbeat(self, **kw):
        r = self.client.post('/api/data', json=dict(token='camera-test', device_id='esp-bridge', **kw))
        self.assertEqual(r.status_code, 200, r.json)
        return r.json

    def test_s3_transport_cannot_be_stolen_by_old_bridge(self):
        self.settings(enabled=True, camera_transport='s3-wifi')
        self.assertFalse(self.client.post(self.dev+'poll',json=dict(ready=True,protocol_version=1)).json['accepted'])
        lease = self.client.post(self.dev+'poll', json=dict(ready=True, protocol_version=1, camera_type='s3-wifi')).json
        self.assertTrue(lease['accepted'])
        self.assertFalse(self.client.post(self.dev+'poll',json=dict(ready=True,protocol_version=1)).json['accepted'])
        self.settings(enabled=True, camera_transport='legacy')
        self.now += 11  # 切换链路不绕过既有的拍照间隔。
        self.assertEqual(C.validate(dict(C.DEFAULT,camera_transport='legacy'))['camera_transport'],'s3-wifi')
        self.assertFalse(self.client.post(self.dev+'poll',json=dict(ready=True,protocol_version=1)).json['accepted'])

    def test_jpeg_analysis_to_real_device_command_browser_cannot_take(self):
        self.settings(enabled=True, interval_sec=2, camera_transport='s3-wifi', follow_enabled=True)
        lease = self.client.post(self.dev+'poll', json=dict(ready=True, protocol_version=1, camera_type='s3-wifi')).json
        detected = dict(faces=[dict(index=0, bbox=[90, 20, 50, 50], center=[135, 50],
                                   face_confidence=.99, recognized=False, name='未知', similarity=None)],
                        boxes=[], size=[160, 90])
        with patch.object(A.camera.faces, 'analyze', return_value=detected):
            result = self.client.post(self.dev+'upload?job_id='+lease['job_id'], data=fixtures.image(),
                                      headers={'X-Capture-Ticket': lease['ticket'], 'Content-Type': 'image/jpeg'})
            self.assertEqual(result.status_code, 202, result.json)
            self.wait_finished()
            # 分析状态持久化后回调才执行，测试等待回调，避免线程时序假失败。
            import time
            end = time.monotonic()+2
            while not self.controller.status('esp-bridge')['pending'] and time.monotonic()<end:
                time.sleep(.01)
        self.assertTrue(self.controller.status('esp-bridge')['pending'])
        self.assertNotIn('command', self.heartbeat(no_pop=True))
        cmd = self.heartbeat()['command']
        self.assertEqual(cmd['note'], 'camera_follow_latest')
        self.assertIn('@MOVE:servo,1,109,30', cmd['raw'])
        self.assertNotIn('command', self.heartbeat())
        self.assertFalse(A._pending_commands)

    def test_manual_and_pause_invalidate_observations(self):
        self.settings(enabled=True, camera_transport='s3-wifi', follow_enabled=True)
        settings = A.camera.current_settings('esp-bridge')
        self.controller.observe('esp-bridge', 'fixture', self.now, dict(faces=[face()]), settings, 'device')
        A.enqueue_command('esp-bridge', dict(raw='@FACE:HAPPY'))
        self.assertEqual(self.heartbeat()['command']['raw'], '@FACE:HAPPY')
        self.assertNotIn('command', self.heartbeat())
        self.now += 6
        self.controller.observe('esp-bridge', 'fixture2', self.now, dict(faces=[face()]), settings, 'device')
        self.settings(enabled=True, camera_transport='s3-wifi', follow_enabled=False)
        self.assertNotIn('command', self.heartbeat())


if __name__ == '__main__':
    unittest.main()
