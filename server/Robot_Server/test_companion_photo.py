"""Single-photo location, no live actuator commands, and calibrated short gestures."""
import copy
import struct
import time
import unittest
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock
from unittest.mock import patch
import app as A
import camera_workspace as C
import device_profile as P
import short_actions as S
import photo_locate as L
import companion_runtime as R
from test_camera_follow import FollowAPITests, face
import test_camera_workspace as fixtures


class ShortActions(unittest.TestCase):
    def test_calibration_limits_and_return_position(self):
        profile=P.default_profile()
        for s in profile['servos']:
            s.update(min=20,mid=25,max=35,front=25,speed=17)
        result=S.for_speed(profile,180)
        self.assertEqual(result['servos'],profile['servos'])
        for action in result['actions']:
            if action['id'] not in S.IDS:continue
            steps=action['timeline']['steps']
            self.assertTrue(all(20<=i['move']['angle']<=35 for i in steps))
            self.assertTrue(all(i['move']['speed']==17 for i in steps))
            self.assertEqual(steps[-1]['move']['angle'],25)
            A.motion_timeline.compile_timeline(action['timeline'],result,180)

    def test_zero_speed_and_idle_wire_fit(self):
        profile=S.for_speed(P.default_profile(),0)
        for a in profile['actions']:
            if a['id'] in S.IDS:
                self.assertTrue(all(s['move']['speed']==0 for s in a['timeline']['steps']))
        with patch.object(P,'motion_snapshot',return_value={'revision':1}):
            result=A.idle_pet.compile_settings(dict(enabled=True,idle_seconds=30,sleep_seconds=180,
                         gap_seconds=30,awake_actions=['short_nod','short_tilt_left'],sleep_action=''),profile,0)
        self.assertLessEqual(len(bytes.fromhex(result['wire'])),708)


class OnePhoto(unittest.TestCase):
    def setUp(self):
        self.now=100.0;self.loc=L.Locator(lambda:self.now)
        self.settings=dict(C.DEFAULT,enabled=False,follow_enabled=False)
        self.profile=P.effective(P.default_profile(),90)
        self.pose=dict(fresh=True,moving=False,angles=[90,30,90],age_ms=0)
        self.loc.start('d','job')

    def observe(self,job='job',source='device'):
        self.loc.observe('d',job,self.now,dict(faces=[face(x=.95,y=.2)]),self.settings,source)

    def test_exactly_one_full_geometric_correction(self):
        self.observe()
        command=self.loc.take('d',self.settings,self.pose,self.profile,{})
        self.assertEqual(command['note'],'photo_locate_once')
        self.assertEqual(len(command['servo_params']),3)
        self.assertGreater(command['servo_params'][0]['angle'],110) # no 20-degree step cap
        self.assertEqual(command['servo_params'][2]['angle'],90)
        self.assertIsNone(self.loc.take('d',self.settings,self.pose,self.profile,{}))
        self.loc.observe('d','new-frame',self.now,dict(faces=[face(x=-.8)]),self.settings,'device')
        self.assertIsNone(self.loc.take('d',self.settings,self.pose,self.profile,{}))

    def test_live_history_and_other_jobs_do_not_move(self):
        for job,source in [('live-1','device'),('old','device'),('job','manual')]:
            self.observe(job,source)
        self.assertIsNone(self.loc.take('d',self.settings,self.pose,self.profile,{}))

    def test_live_frames_never_enter_original_controller(self):
        import camera_follow
        controller=camera_follow.Controller(lambda:self.now)
        settings=dict(self.settings,enabled=True,follow_enabled=True)
        controller.observe('d','live-1',self.now,dict(faces=[face()]),settings,'device')
        self.assertFalse(controller.status('d')['pending'])

    def test_stale_pose_waits_then_expires_without_guessing(self):
        self.observe()
        pose=dict(self.pose,fresh=False)
        self.assertIsNone(self.loc.take('d',self.settings,pose,self.profile,{}))
        self.assertTrue(self.loc.pending('d'))
        self.now+=6
        self.assertIsNone(self.loc.take('d',self.settings,self.pose,self.profile,{}))
        self.assertEqual(self.loc.status('d')['phase'],'expired')

    def test_busy_waits_and_cancel_prevents_late_commands(self):
        self.observe()
        self.assertIsNone(self.loc.take('d',self.settings,self.pose,self.profile,{'ota':True}))
        self.assertTrue(self.loc.pending('d'))
        self.loc.cancel('d')
        self.assertIsNone(self.loc.take('d',self.settings,self.pose,self.profile,{}))


class PhotoAPI(FollowAPITests):
    def setUp(self):
        super().setUp()
        self.loc=L.Locator(lambda:self.now)
        for obj,key,value in [(R,'LOCATOR',self.loc),(A.camera,'on_analysis',lambda *args:R.observe_photo(A,*args)),
                              (A.idle_pet,'snapshot',lambda *a:{'applied':True}),
                              (P,'load',lambda:S.extend(P.default_profile()))]:
            p=patch.object(obj,key,value);p.start();self.addCleanup(p.stop)
        self.heartbeat()

    def test_one_photo_real_routes_to_one_three_servo_command(self):
        self.settings(enabled=False,follow_enabled=False)
        begun=self.client.post('/api/companion/locate',json={'device_id':'esp-bridge'})
        self.assertEqual(begun.status_code,200,begun.json)
        self.now+=12  # Time spent waiting for camera is not image age.
        lease=self.client.post(self.dev+'poll',json=dict(ready=True,protocol_version=1,camera_type='s3-wifi')).json
        self.assertTrue(lease['accepted'])
        detected=dict(faces=[dict(index=0,bbox=[90,20,50,50],center=[135,50],
                     face_confidence=.99,recognized=False,name='unknown',similarity=None)],boxes=[],size=[160,90])
        with patch.object(A.camera.faces,'analyze',return_value=detected):
            upload=self.client.post(self.dev+'upload?job_id='+lease['job_id'],data=fixtures.image(),
                    headers={'X-Capture-Ticket':lease['ticket'],'Content-Type':'image/jpeg'})
            self.assertEqual(upload.status_code,202,upload.json)
            end=time.monotonic()+3
            while self.loc.status('esp-bridge')['phase']=='waiting_photo' and time.monotonic()<end:
                time.sleep(.01)
        self.assertEqual(self.loc.status('esp-bridge')['phase'],'waiting_pose')
        self.assertNotIn('command',self.heartbeat(no_pop=True))
        response=self.heartbeat()
        self.assertIn('command',response,self.loc.status('esp-bridge'))
        command=response['command']
        self.assertEqual(command['note'],'photo_locate_once')
        for sid in [1,2,3]:self.assertIn('@MOVE:servo,%d,'%sid,command['raw'])
        self.assertNotIn('command',self.heartbeat())
        self.assertFalse(A.camera.current_settings('esp-bridge')['follow_enabled'])

    def test_cancel_and_offline_are_explicit(self):
        self.assertEqual(self.client.post('/api/companion/locate',json={}).status_code,200)
        self.assertEqual(self.client.post('/api/companion/locate',json={'cancel':True}).status_code,200)
        self.assertEqual(self.loc.status('esp-bridge')['phase'],'cancelled')
        with patch.object(A,'device_age_sec',return_value=500):
            self.assertEqual(self.client.post('/api/companion/locate',json={}).status_code,409)

    def test_sound_photo_outputs_one_command_but_live_does_not(self):
        self.settings(enabled=True,follow_enabled=True,capture_mode='sound')
        settings=A.camera.current_settings('esp-bridge')
        A.camera.on_analysis('esp-bridge','live-1',self.now,dict(faces=[face()]),settings,'device')
        self.assertNotIn('command',self.heartbeat())
        A.camera.on_analysis('esp-bridge','sound-1',self.now,dict(faces=[face()]),settings,'device')
        result=self.heartbeat()
        self.assertEqual(result['command']['note'],'photo_locate_once')
        self.assertNotIn('command',self.heartbeat())

    def test_cancelled_photo_cannot_rearm_with_automatic_location_enabled(self):
        self.settings(enabled=True,follow_enabled=True,capture_mode='sound')
        begun=self.client.post('/api/companion/locate',json={})
        self.assertEqual(begun.status_code,200,begun.json)
        job=begun.json['job_id']
        self.assertEqual(self.client.post('/api/companion/locate',json={'cancel':True}).status_code,200)
        settings=A.camera.current_settings('esp-bridge')
        for _ in range(2):
            A.camera.on_analysis('esp-bridge',job,self.now,dict(faces=[face(x=.8)]),settings,'device')
            self.assertNotIn('command',self.heartbeat())
            self.assertEqual(self.loc.status('esp-bridge')['phase'],'cancelled')

    def test_duplicate_completed_photos_cannot_send_again_after_a_new_job(self):
        self.settings(enabled=True,follow_enabled=True,capture_mode='sound')
        settings=A.camera.current_settings('esp-bridge')
        def observed(job):
            A.camera.on_analysis('esp-bridge',job,self.now,dict(faces=[face(x=.8)]),settings,'device')
        for job in ('sound-1','sound-2'):
            self.now+=5  # The second photo is captured after the first movement.
            observed(job)
            result=self.heartbeat()
            self.assertIn('command',result,self.loc.status('esp-bridge'))
            self.assertEqual(result['command']['note'],'photo_locate_once')
            self.assertNotIn('command',self.heartbeat())
            observed(job)
            self.assertNotIn('command',self.heartbeat())
        observed('sound-1')
        self.assertNotIn('command',self.heartbeat())


class Upgrade(unittest.TestCase):
    def test_output_folder_reuses_original_model_cache(self):
        import json,lan_start
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);cache=root/'existing-cache';cache.mkdir()
            (cache/'model.bin').write_bytes(b'fixture')
            web=root/'outputs/web';web.mkdir(parents=True)
            (web/'lan_seed.json').write_text(json.dumps({'source_models':str(cache)}),encoding='utf-8')
            with patch.dict('os.environ',{},clear=True):
                self.assertEqual(lan_start.find_models(web),cache.resolve())

    def test_one_time_setup_preserves_capture_mode_and_servo_calibration(self):
        with tempfile.TemporaryDirectory() as tmp:
            original=dict(C.DEFAULT,enabled=True,capture_mode='sound',follow_enabled=True)
            rel=SimpleNamespace(BASE_DIR=tmp,DEFAULT_DEVICE_ID='d',
                device_profile=SimpleNamespace(load=lambda:P.default_profile()),
                device_controls=SimpleNamespace(get=lambda d:{'servo_speed':90}),
                camera=SimpleNamespace(current_settings=lambda d:dict(original),save=Mock()),
                idle_pet=SimpleNamespace(save=Mock()),load_config=lambda:{},save_config=Mock())
            R.prepare(rel)
            saved=rel.camera.save.call_args.args[1]
            self.assertEqual(saved['capture_mode'],'sound')
            self.assertTrue(saved['follow_enabled'])
            R.prepare(rel)
            self.assertEqual(rel.camera.save.call_count,1)
            self.assertEqual(rel.idle_pet.save.call_count,1)


if __name__=='__main__':unittest.main()
