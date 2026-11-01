"""连续接入的真实API/有界队列/停止竞态测试；隔离临时目录，不连接硬件。"""
import io
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch
from PIL import Image
import app as A
import camera_workspace as C
from camera_live import LiveCamera
import test_camera_workspace as previous


class LiveTests(unittest.TestCase):
    setUp_base = previous.CameraTests.setUp

    def setUp(self):
        self.setUp_base()
        self.live = LiveCamera(A.camera)
        p = patch.object(A.camera, 'live', self.live); p.start(); self.addCleanup(p.stop)
        p = patch.object(A.camera, 'on_analysis', None); p.start(); self.addCleanup(p.stop)
        p = patch.object(A.camera, 'on_settings', None); p.start(); self.addCleanup(p.stop)
        self.settings()
        self.analyze_patch = patch.object(A.camera.faces, 'analyze', lambda raw: dict(
            faces=[], boxes=[], size=[320,240], summary='未检测到人脸', elapsed_ms=1))
        self.analyze_patch.start(); self.addCleanup(self.analyze_patch.stop)
        raw = io.BytesIO(); Image.new('RGB',(320,240),'#557799').save(raw,'JPEG'); self.raw=raw.getvalue()

    def settings(self, **kw):
        value=dict(C.DEFAULT, enabled=True, camera_transport='s3-wifi', capture_mode='continuous')
        value.update(kw)
        r=self.client.post('/api/camera/settings',json=value);self.assertEqual(r.status_code,200,r.json)
        return r.json['settings']

    def grant(self, **kw):
        body=dict(ready=True, protocol_version=1, camera_type='s3-wifi',continuous_capable=True);body.update(kw)
        return self.client.post(self.dev+'poll',json=body).json

    def frame(self, lease, sequence=1, raw=None, client=None, **headers):
        h={**self.auth, 'X-Capture-Ticket':lease['ticket'], 'X-Frame-Sequence':str(sequence), **headers}
        return (client or self.client).post(self.dev+'live-frame',headers=h,
                     data=self.raw if raw is None else raw,content_type='image/jpeg')

    def wait(self, check):
        end=time.monotonic()+5
        while not check():
            self.assertLess(time.monotonic(),end,'工作线程未按时结束');time.sleep(.01)

    def test_continuous_grant_reuse_and_no_persistent_jobs(self):
        lease=self.grant();self.assertEqual(lease['mode'],'continuous')
        self.assertEqual(self.grant()['ticket'],lease['ticket'])
        self.assertEqual(self.frame(lease).status_code,202)
        self.wait(lambda:self.live.status('esp-bridge')['analyzed']==1)
        status=self.client.get('/api/camera/live/status').json
        self.assertEqual(status['analysis']['face_count'] if 'face_count' in status['analysis'] else len(status['analysis']['faces']),0)
        self.assertNotIn('ticket',status);self.assertEqual(self.client.get('/api/camera/status').json['items'],[])
        self.assertEqual(self.client.get('/api/camera/live/image').status_code,200)
        self.assertEqual(self.client.get('/api/camera/live/image?kind=analyzed&sequence=1').status_code,200)
        self.assertEqual(self.client.get('/api/camera/live/image?kind=analyzed&sequence=9').status_code,409)

    def test_device_auth_cannot_read_preview_or_change_settings(self):
        anon=A.app.test_client();lease=self.grant()
        self.assertEqual(self.frame(lease,client=anon).status_code,202)
        for path in ['/api/camera/live/status','/api/camera/live/image']:
            self.assertEqual(anon.get(path,headers=self.auth).status_code,401)
        self.assertEqual(anon.post('/api/camera/settings',json={},headers=self.auth).status_code,401)
        self.assertEqual(anon.post(self.dev+'live-frame',data=self.raw).status_code,401)

    def test_pause_change_disable_expiry_reject_old_frames(self):
        lease=self.grant();self.settings(paused=True)
        self.assertEqual(self.frame(lease).status_code,409)
        self.settings();fresh=self.grant();self.assertNotEqual(fresh['ticket'],lease['ticket'])
        self.now+=16;self.assertEqual(self.frame(fresh).status_code,409)
        fresh=self.grant();self.settings(enabled=False);self.assertEqual(self.frame(fresh).status_code,409)
        self.assertFalse(self.grant()['accepted'])

    def test_busy_and_time_window_gate_every_frame(self):
        for key in ['audio','ota','motion']:
            lease=self.grant();self.busy[key]=True
            self.assertEqual(self.frame(lease).status_code,409)
            self.assertFalse(self.grant()['accepted']);self.busy.clear()
        self.settings(windows=[dict(start='20:00',end='21:00')])
        self.assertFalse(self.grant()['accepted'])

    def test_bad_frame_sequence_rate_and_stale_preview(self):
        lease=self.grant()
        self.assertEqual(self.frame(lease,raw=b'bad').status_code,400)
        self.assertEqual(self.frame(lease,sequence=0).status_code,400)
        self.assertEqual(self.frame(lease,**{'X-Capture-Ticket':'wrong'}).status_code,409)
        self.assertEqual(self.frame(lease).status_code,202)
        self.assertEqual(self.frame(lease).status_code,409)
        self.assertEqual(self.frame(lease,sequence=2).status_code,429)
        self.now+=6;self.assertEqual(self.client.get('/api/camera/live/image').status_code,404)

    def test_latest_frame_replaces_pending_without_backlog(self):
        entered,release=threading.Event(),threading.Event();calls=[]
        def slow(raw):
            calls.append(raw);entered.set();release.wait(4)
            return dict(faces=[],boxes=[],size=[320,240],summary='fixture',elapsed_ms=1)
        with patch.object(A.camera.faces,'analyze',slow):
            lease=self.grant();self.assertEqual(self.frame(lease).status_code,202)
            self.assertTrue(entered.wait(2))
            for seq in range(2,7):
                self.now+=.6;self.assertEqual(self.frame(lease,seq).status_code,202)
            self.assertEqual(self.live.status('esp-bridge')['dropped'],4)
            release.set();self.wait(lambda:not self.live.sessions['esp-bridge']['working'])
            self.assertEqual(len(calls),2)
            self.assertEqual(self.live.status('esp-bridge')['analysis_sequence'],6)

    def test_stop_during_analysis_never_publishes_or_moves_servo(self):
        entered,release=threading.Event(),threading.Event();observed=[]
        def slow(raw):
            entered.set();release.wait(4);return dict(faces=[],boxes=[],size=[320,240],summary='fixture')
        with patch.object(A.camera.faces,'analyze',slow),patch.object(A.camera,'on_analysis',lambda *args:observed.append(args)):
            lease=self.grant();self.frame(lease);self.assertTrue(entered.wait(2))
            old=self.live.sessions['esp-bridge'];self.settings(enabled=False);release.set()
            self.wait(lambda:not old['working'])
            self.assertEqual(observed,[]);self.assertFalse(self.live.status('esp-bridge')['active'])

    def test_slow_upload_result_does_not_follow(self):
        observed=[]
        with patch.object(A.camera,'on_analysis',lambda *args:observed.append(args)):
            lease=self.grant()
            self.live.receive('esp-bridge',self.raw,lease['ticket'],1,captured_at=self.now-6)
            self.wait(lambda:not self.live.sessions['esp-bridge']['working'])
            self.assertEqual(observed,[])

    def test_manual_snapshot_priority_and_old_firmware_block(self):
        self.assertFalse(self.grant(continuous_capable=False)['accepted'])
        self.assertEqual(self.client.post('/api/camera/request-device').status_code,202)
        value=self.grant();self.assertIn('job_id',value);self.assertNotIn('mode',value)
        # 下一次轮询可以恢复视频，单拍任务仍使用原来的上传/保留链路。
        self.assertEqual(self.grant()['mode'],'continuous')

    def test_limits_and_session_count(self):
        for value in [0,6,True,2.5]:
            with self.assertRaises(C.CameraError):C.validate(dict(C.DEFAULT,continuous_fps=value))
        with self.assertRaises(C.CameraError):C.validate(dict(C.DEFAULT,capture_mode='continuous',source='browser'))
        for i in range(4):self.assertTrue(self.live.grant(str(i),A.camera.current_settings('esp-bridge'))['accepted'])
        self.assertFalse(self.live.grant('fifth',A.camera.current_settings('esp-bridge'))['accepted'])
        self.now+=16;self.assertTrue(self.live.grant('fifth',A.camera.current_settings('esp-bridge'))['accepted'])

    def test_continuous_actual_local_face_model(self):
        self.analyze_patch.stop()
        samples=Path(__file__).resolve().parents[3]/'工具/识别追踪/vision_local/samples'
        if not samples.exists():samples=Path(__file__).resolve().parents[1]/'验证记录/人脸样例'
        raw=(samples/'opencv_lena.jpg').read_bytes();lease=self.grant()
        self.assertEqual(self.frame(lease,raw=raw).status_code,202)
        self.wait(lambda:self.live.status('esp-bridge')['analyzed']==1)
        status=self.live.status('esp-bridge')
        self.assertEqual(status['analysis']['face_count'],1)
        self.assertEqual(status['analysis']['faces'][0]['name'],'未知人员')
        self.assertEqual(self.client.get('/api/camera/status').json['items'],[])


if __name__=='__main__':unittest.main()
