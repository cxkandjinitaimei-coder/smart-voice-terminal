"""test_camera_workspace.py：使用真实服务模块的离线回归。

使用：由 tools/run_checks.py 在临时服务副本中单独运行；不连接硬件、不调用真实 AI。
范围：断言涵盖本文件列出的状态/边界；测试通过不代表飞线信号、供电和公网时延已验证。
依赖：server/Robot_Server/requirements.txt；数据由测试临时创建并清理。
"""
import io
import os
import json
import tempfile
import threading
import time
import unittest
from datetime import datetime
from pathlib import Path
from unittest.mock import patch

from PIL import Image
import app as A
import camera_workspace as C
import local_faces as F


def image():
    raw = io.BytesIO()
    Image.new('RGB', (1600, 900), '#568579').save(raw, 'PNG')
    return raw.getvalue()


class CameraTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.now = datetime(2026, 10, 15, 9, tzinfo=C.TZ).timestamp()
        self.busy = {}
        self.cfg = {'token':'camera-test', 'providers':{'fixture':{'enabled':True, 'model':'vision-test', 'keys':['test-only'], 'base_url':'http://127.0.0.1:1/v1'}}}
        self.ws = C.CameraWorkspace(Path(self.tmp.name), lambda:self.cfg, lambda device:self.busy, lambda:self.now)
        # Blueprint captures the existing service object; patch its attributes/method environment.
        for key in ['root','config','blockers','clock','lock','slots','ready','seen','faces']:
            p = patch.object(A.camera, key, getattr(self.ws, key)); p.start(); self.addCleanup(p.stop)
        for obj, key, value in [(A,'load_config',lambda:self.cfg),(A,'start_auto_tick',lambda:None),
                                (A,'start_store',lambda:None),(A,'start_beacon',lambda:None),(A.asr,'warmup',lambda:None)]:
            p=patch.object(obj,key,value);p.start();self.addCleanup(p.stop)
        self.client=A.app.test_client()
        with self.client.session_transaction() as s:s['authed']=True
        self.dev='/api/camera/device/'
        self.auth={'Authorization':'Bearer camera-test'}

    def settings(self, **kwargs):
        return self.client.post('/api/camera/settings',json=dict(C.DEFAULT,**kwargs))

    def upload(self, raw=None):
        return self.client.post('/api/camera/upload',data={'image':(io.BytesIO(image() if raw is None else raw),'test.png')})

    def poll(self):
        return self.client.post(self.dev+'poll',json={'ready':True,'protocol_version':1})

    def test_auth_scope_login_and_skin_registry(self):
        anon=A.app.test_client()
        for path in ['/api/camera/status','/api/ui/skins','/api/camera/image/x']:
            self.assertEqual(anon.get(path,headers=self.auth).status_code,401)
        self.assertEqual(anon.post(self.dev+'poll',json={'ready':True,'protocol_version':1},headers=self.auth).status_code,200)
        self.assertEqual(anon.post('/api/camera/settings',json={},headers=self.auth).status_code,401)
        self.assertEqual(anon.post(self.dev+'poll',json={},headers={'Authorization':'Bearer wrong'}).status_code,401)
        self.assertEqual(anon.post('/login',data={'token':'wrong'}).status_code,200)
        self.assertEqual(anon.post('/login',data={'token':'camera-test'}).status_code,302)
        html=anon.get('/').text
        self.assertIn('motion-trainer-frame',html)
        self.assertIn('appearance-skin',html)
        self.assertEqual({s['id'] for s in anon.get('/api/ui/skins').json['skins']},{'original','nexus-red'})
        login=anon.get('/login').text
        self.assertIn('name="token"',login)
        self.assertNotIn('cdnjs',login)

    def test_default_disabled_validation_and_no_commands(self):
        A._pending_commands.clear()
        self.assertFalse(self.poll().json['accepted'])
        for change in [{'interval_sec':True},{'keep_count':1},{'windows':[{'start':'25:00','end':'09:00'}]},
                       {'enabled':'yes'},{'windows':[{'start':'09:00','end':'09:00'}]}]:
            self.assertEqual(self.settings(**change).status_code,400,change)
        self.assertEqual(self.client.get('/api/camera/status?device_id=../oops').status_code,400)
        self.assertFalse(A._pending_commands)

    def test_schedule_interval_expiry_pause_and_cross_midnight(self):
        self.settings(enabled=True, windows=[{'start':'08:00','end':'10:00'}])
        lease=self.poll().json; self.assertTrue(lease['accepted'])
        self.assertFalse(self.poll().json['accepted'])
        self.now+=61
        self.assertTrue(self.poll().json['accepted'])
        self.settings(enabled=True,paused=True)
        self.assertFalse(self.poll().json['accepted'])
        self.settings(enabled=True,windows=[{'start':'22:00','end':'06:00'}])
        self.assertFalse(self.poll().json['accepted'])
        self.now=datetime(2026,10,15,23,tzinfo=C.TZ).timestamp()
        self.assertTrue(self.poll().json['accepted'])
        self.settings(enabled=True,windows=[{'start':'22:00','end':'06:00'}])
        self.now=datetime(2026,10,16,6,tzinfo=C.TZ).timestamp()
        self.assertFalse(self.poll().json['accepted'])

    def test_each_busy_gate_and_source(self):
        self.settings(enabled=True)
        for flag in ['audio','ota','motion']:
            self.busy.clear();self.busy[flag]=True
            self.assertFalse(self.poll().json['accepted'])
        self.busy.clear()
        self.settings(enabled=True,source='browser')
        self.assertFalse(self.poll().json['accepted'])
        self.assertTrue(self.client.post('/api/camera/capture').json['accepted'])

    def test_real_image_bounded_normalization_download_delete_and_persistence(self):
        result=self.upload();self.assertEqual(result.status_code,202,result.json)
        item=self.wait_finished();self.assertEqual(item['state'],'done')
        self.assertEqual((item['width'],item['height']),(1280,720))
        raw=self.client.get(item['image_url']);self.assertEqual(raw.status_code,200)
        self.assertEqual(Image.open(io.BytesIO(raw.data)).format,'JPEG')
        fresh=C.CameraWorkspace(Path(self.tmp.name),lambda:self.cfg,lambda _: {},lambda:self.now)
        self.assertEqual(fresh.status('esp-bridge')['items'][0]['id'],item['id'])
        self.assertEqual(self.client.delete('/api/camera/jobs/'+item['id']).status_code,200)
        self.assertEqual(self.client.get(item['image_url']).status_code,404)
        self.assertEqual(self.upload(b'not-an-image').status_code,400)
        self.assertEqual(self.upload(b'x'*(C.MAX_IMAGE_BYTES+1)).status_code,413)

    def test_capture_ticket_wrong_device_duplicate_and_cancelled_upload(self):
        self.settings(enabled=True)
        lease=self.poll().json
        url=self.dev+'upload?job_id='+lease['job_id']
        self.assertEqual(self.client.post(url,data=image()).status_code,409)
        headers={'X-Capture-Ticket':lease['ticket']}
        self.assertEqual(self.client.post(url+'&device_id=other',data=image(),headers=headers).status_code,409)
        self.assertEqual(self.client.post(url,data=image(),headers=headers).status_code,202)
        self.assertEqual(self.client.post(url,data=image(),headers=headers).status_code,202)
        self.wait_finished()
        self.now+=20;lease=self.poll().json
        self.settings(enabled=True,paused=True)
        self.assertEqual(self.client.post(self.dev+'upload?job_id='+lease['job_id'],data=image(),headers={'X-Capture-Ticket':lease['ticket']}).status_code,409)

    def wait_finished(self):
        end=time.monotonic()+5
        while time.monotonic()<end:
            item=self.client.get('/api/camera/status').json['items'][0]
            if item['state']!='analyzing':return item
            time.sleep(.02)
        self.fail('analysis timed out')

    def test_async_analysis_failure_retry_and_retention(self):
        self.settings(analyze=True,provider='fixture',keep_count=5)
        with patch.object(A.camera.faces,'analyze',side_effect=C.CameraError('模拟模型失败')):
            self.assertEqual(self.upload().status_code,202)
            item=self.wait_finished();self.assertEqual(item['state'],'failed')
        result=dict(has_person=True,person_count=1,summary='<b>真实接口测试</b>',boxes=[dict(x=.1,y=.1,w=.2,h=.4)])
        with patch.object(A.camera.faces,'analyze',return_value=result):
            self.assertEqual(self.client.post('/api/camera/jobs/'+item['id']+'/retry').status_code,202)
            completed=self.wait_finished()['analysis']
            self.assertEqual(completed['summary'],result['summary'])
            self.assertIsNone(completed['target'])
        self.settings(keep_count=5)
        for _ in range(7):
            self.now+=1;self.assertEqual(self.upload().status_code,202);self.wait_finished()
        self.assertEqual(len(self.client.get('/api/camera/status').json['items']),5)
        self.assertEqual(len(list(Path(self.tmp.name).glob('*.jpg'))),5)

    def test_two_concurrent_analysis_slots_backpressure(self):
        ready=threading.Event()
        def analyze(*args):ready.wait(4);return dict(has_person=False,person_count=0,summary='fixture',boxes=[])
        self.settings(analyze=True,provider='fixture')
        with patch.object(A.camera.faces,'analyze',side_effect=analyze):
            self.assertEqual(self.upload().status_code,202)
            # Different devices still share the bounded two-slot analysis pool.
            for device in ['other','third']:
                self.client.post('/api/camera/settings?device_id='+device,json=dict(C.DEFAULT,analyze=True,provider='fixture'))
            r=self.client.post('/api/camera/upload?device_id=other',data={'image':(io.BytesIO(image()),'x.png')})
            self.assertEqual(r.status_code,202)
            r=self.client.post('/api/camera/upload?device_id=third',data={'image':(io.BytesIO(image()),'x.png')})
            self.assertEqual(r.status_code,429)
            ready.set();self.wait_finished()
            end=time.monotonic()+5
            while self.client.get('/api/camera/status?device_id=other').json['items'][0]['state']=='analyzing':
                self.assertLess(time.monotonic(),end);time.sleep(.02)

    def test_migrate_legacy_disabled_and_remote_provider(self):
        self.settings(analyze=False,provider='some-remote-model')
        state=self.client.get('/api/camera/status').json
        self.assertEqual(state['settings']['provider'],'local-face')
        self.assertTrue(state['settings']['analyze'])
        self.assertNotIn('providers',state)
        # Normalization is independent of whether optional local weights have been installed.
        self.assertIsInstance(state['face_engine']['ready'],bool)
        with patch.object(A.camera.faces,'analyze',return_value=dict(size=[320,240],faces=[],boxes=[],face_count=0)):
            self.assertEqual(self.upload().status_code,202)
            self.assertEqual(self.wait_finished()['analysis']['face_count'],0)

    @unittest.skipUnless(os.environ.get('ROBOT_FACE_TEST_ASSETS'), 'optional --face-assets and --face-models are required')
    def test_real_original_models_unknown_enroll_match_and_delete_offline(self):
        import socket
        import cv2
        import numpy as np
        samples=Path(os.environ['ROBOT_FACE_TEST_ASSETS'])
        lena=(samples/'opencv_lena.jpg').read_bytes()
        messi=(samples/'opencv_messi5.jpg').read_bytes()
        def no_network(*args,**kwargs):raise AssertionError('face recognition must remain offline')
        with patch.object(socket.socket,'connect',no_network), patch.object(socket,'create_connection',no_network):
            self.assertEqual(self.upload(lena).status_code,202)
            initial=self.wait_finished()['analysis']
            self.assertEqual(initial['face_count'],1)
            self.assertEqual(initial['faces'][0]['name'],'未知人员')
            box=initial['boxes'][0]
            self.assertTrue(0<box['w']<1 and 0<box['h']<1)
            registered=self.client.post('/api/camera/faces/enroll',data={'name':'参考人员','photos':(io.BytesIO(lena),'single.jpg')})
            self.assertEqual(registered.status_code,200,registered.json)
            person_id=registered.json['people'][0]['id']
            engine=A.camera.faces.get()
            changed=cv2.resize(cv2.convertScaleAbs(engine.decode(lena),alpha=.92,beta=12),(400,400))
            raw=cv2.imencode('.jpg',changed)[1].tobytes()
            self.assertEqual(self.upload(raw).status_code,202)
            matched=self.wait_finished()['analysis']
            self.assertEqual(matched['faces'][0]['name'],'参考人员')
            self.assertTrue(matched['boxes'][0]['recognized'])
            self.assertEqual(matched['target']['name'],'参考人员')
            self.assertIn('angles_deg',matched['target'])
            self.assertEqual(sum(b['selected'] for b in matched['boxes']),1)
            self.assertEqual(self.upload(messi).status_code,202)
            unknown=self.wait_finished()['analysis']
            self.assertGreater(unknown['face_count'],0)
            self.assertTrue(all(not f['recognized'] for f in unknown['faces']))
            # Two independent face positions must yield two separate overlays.
            one=engine.decode(lena);two=np.hstack([one,one])
            self.assertEqual(self.upload(cv2.imencode('.jpg',two)[1].tobytes()).status_code,202)
            multiple=self.wait_finished()['analysis']
            self.assertEqual(multiple['face_count'],2)
            self.assertEqual(multiple['target']['similarity'],max(f['similarity'] for f in multiple['faces'] if f['recognized']))
            self.assertEqual(sum(b['selected'] for b in multiple['boxes']),1)
            restored=type(A.camera.faces)(A.camera.faces.data_dir)
            self.assertEqual(restored.get().public_state()['people'][0]['name'],'参考人员')
            self.assertEqual(self.client.post('/api/camera/faces/enroll',data={'name':'无效照','photos':(io.BytesIO(image()),'blank.png')}).status_code,400)
            self.assertEqual(self.client.post('/api/camera/faces/settings',json={'threshold':float('nan')}).status_code,400)
            self.assertEqual(self.client.post('/api/camera/faces/settings',json={'threshold':.55,'margin':.1}).status_code,200)
            anon=A.app.test_client()
            self.assertEqual(anon.get('/api/camera/faces/state').status_code,401)
            self.assertEqual(anon.post('/api/camera/faces/enroll',headers=self.auth).status_code,401)
            self.assertEqual(self.client.delete('/api/camera/faces/people/'+person_id).status_code,200)
            self.assertEqual(self.upload(lena).status_code,202)
            self.assertEqual(self.wait_finished()['analysis']['faces'][0]['name'],'未知人员')

    @unittest.skipUnless(os.environ.get('ROBOT_FACE_TEST_ASSETS'), 'optional --face-assets and --face-models are required')
    def test_device_and_browser_capture_upload_real_faces(self):
        samples=Path(os.environ['ROBOT_FACE_TEST_ASSETS'])
        raw=(samples/'opencv_lena.jpg').read_bytes()
        # A device has no browser session: it uses Bearer plus a one-time ticket.
        anon=A.app.test_client()
        self.settings(enabled=True,source='device')
        lease=anon.post(self.dev+'poll',json={'ready':True,'protocol_version':1},headers=self.auth).json
        self.assertTrue(lease['accepted'])
        url=lease['upload_url']+'?device_id=esp-bridge&job_id='+lease['job_id']
        headers=dict(self.auth,**{'Content-Type':'image/jpeg','X-Capture-Ticket':lease['ticket']})
        self.assertEqual(anon.post(url,data=raw).status_code,401)
        self.assertEqual(anon.post(url,data=raw,headers=headers).status_code,202)
        item=self.wait_finished()
        self.assertEqual(item['source'],'device')
        self.assertEqual(item['analysis']['faces'][0]['name'],'未知人员')
        self.assertEqual(len(item['analysis']['boxes']),1)
        self.assertEqual(anon.get(item['image_url'],headers=self.auth).status_code,401)
        # Browser webcam uses the same image processing, with session authentication.
        self.now+=20
        self.settings(enabled=True,source='browser')
        lease=self.client.post('/api/camera/capture').json
        self.assertTrue(lease['accepted'])
        response=self.client.post('/api/camera/upload?job_id='+lease['job_id'],
            headers={'X-Capture-Ticket':lease['ticket']},
            data={'image':(io.BytesIO(raw),'webcam.jpg')})
        self.assertEqual(response.status_code,202,response.json)
        item=self.wait_finished()
        self.assertEqual(item['source'],'browser')
        self.assertEqual(item['analysis']['face_count'],1)
        self.assertEqual(item['analysis']['faces'][0]['name'],'未知人员')


class TargetGeometryTests(unittest.TestCase):
    def result(self, center=(320,240), score=.8, recognized=True):
        return dict(size=[640,480],faces=[dict(index=1,center=list(center),name='样例',person_id='p',
                    recognized=recognized,similarity=score)],boxes=[dict(index=1,x=.1,y=.1,w=.2,h=.2)])

    def project(self,result):
        return F.add_targeting(result,[1280,960],dict(horizontal_fov_deg=90,vertical_fov_deg=90))

    def test_center_axes_edges_and_saved_image_coordinates(self):
        center=self.project(self.result())['target']
        self.assertEqual(center['center_px'],[640,480])
        self.assertEqual(center['offset_px'],[0,0])
        self.assertEqual(center['angles_deg'],dict(yaw=0,pitch=0))
        right=self.project(self.result((640,240)))['target']
        self.assertEqual(right['offset_px'],[640,0])
        self.assertAlmostEqual(right['angles_deg']['yaw'],45,places=3)
        up=self.project(self.result((320,0)))['target']
        self.assertEqual(up['offset_px'],[0,-480])
        self.assertAlmostEqual(up['angles_deg']['pitch'],45,places=3)
        bottom_left=self.project(self.result((0,480)))['target']['angles_deg']
        self.assertEqual(bottom_left['yaw'],-45)
        self.assertAlmostEqual(bottom_left['pitch'],-35.264,places=3)

    def test_strongest_accepted_match_not_largest_or_first_face(self):
        result=self.result(score=.60)
        result['faces'] += [dict(index=2,center=[100,200],name='最佳',recognized=True,similarity=.91),
                            dict(index=3,center=[600,400],name='不确定',recognized=False,similarity=.95)]
        result['boxes'] += [dict(index=2),dict(index=3)]
        output=self.project(result)
        self.assertEqual(output['target']['index'],2)
        self.assertEqual([b['index'] for b in output['boxes'] if b['selected']],[2])
        self.assertNotIn('offset_px',result['faces'][0])

    def test_unknown_no_library_and_empty_clear_target(self):
        for score in [None,.9]:
            output=self.project(self.result(score=score,recognized=False))
            self.assertIsNone(output['target'])
            self.assertIn('offset_px',output['faces'][0])
            self.assertFalse(output['boxes'][0]['selected'])
        self.assertIsNone(self.project(dict(size=[640,480],faces=[],boxes=[]))['target'])

    def test_fov_validation_and_resolution_invariance(self):
        for bad in [0,180,True,'90',float('nan'),float('inf')]:
            with self.assertRaises(ValueError):F.angle_config(dict(horizontal_fov_deg=bad))
            with self.assertRaises(C.CameraError):C.validate(dict(vertical_fov_deg=bad))
        base=self.project(self.result((160,120)))
        scaled=F.add_targeting(self.result((160,120)),[640,480],dict(horizontal_fov_deg=90,vertical_fov_deg=90))
        self.assertEqual(base['target']['angles_deg'],scaled['target']['angles_deg'])
        self.assertEqual(base['target']['offset_normalized'],[-.5,-.5])
        self.assertEqual(C.validate({})['horizontal_fov_deg'],60)


if __name__=='__main__':unittest.main(verbosity=2)
