"""Persist per-axis speeds and exercise the real ASR/chat-to-device decision path."""
from contextlib import ExitStack
import copy
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

import app as A
import ai_relay
import device_profile as P
import motion_timeline as T


class SpeedDefaultsTests(unittest.TestCase):
    def setUp(self):
        self.scope = ExitStack()
        self.tmp = self.scope.enter_context(tempfile.TemporaryDirectory(prefix='servo-defaults-'))
        self.scope.enter_context(patch.object(P, 'PROFILE_PATH', str(Path(self.tmp)/'profile.json')))
        for name in ['start_auto_tick', 'start_store', 'start_beacon', 'append_line']:
            self.scope.enter_context(patch.object(A, name))
        for name in ['record_command', 'record_interaction', 'record_device_report']:
            if hasattr(A.store, name): self.scope.enter_context(patch.object(A.store, name))
        self.scope.enter_context(patch.object(A.asr, 'warmup'))
        self.scope.enter_context(patch.object(A, 'load_config', return_value={'token':'servo-test'}))
        self.scope.enter_context(patch.object(A.device_controls, 'get',
                                             side_effect=lambda sid: {'servo_speed':45 if sid=='other' else 90}))
        A._pending_commands.clear()
        P.save(P.default_profile())
        self.client = A.app.test_client()
        with self.client.session_transaction() as session: session['authed']=True

    def tearDown(self):
        A._pending_commands.clear()
        self.scope.close()

    def save(self):
        response = self.client.post('/api/admin/servo-speeds', json={'speeds':{'1':270,'2':180,'3':0}})
        self.assertEqual(response.status_code, 200)
        return response.get_json()

    def test_auth_validation_and_no_partial_write(self):
        anonymous = A.app.test_client()
        self.assertEqual(anonymous.post('/api/admin/servo-speeds',json={'speeds':{'1':270}}).status_code,401)
        before = Path(P.PROFILE_PATH).read_bytes()
        for speeds in ({'1':270,'2':361},{'1':True},{'1':1.5},{'16':100},[],{}):
            self.assertEqual(self.client.post('/api/admin/servo-speeds',json={'speeds':speeds}).status_code,400)
            self.assertEqual(Path(P.PROFILE_PATH).read_bytes(),before)

    def test_save_reload_preserves_calibration_and_wire_compatibility(self):
        before = P.load()
        crc = P.motion_snapshot()['revision']
        self.save()
        after = P.load()
        self.assertEqual([s['speed'] for s in after['servos']],[270,180,0])
        for servo in after['servos']: servo.pop('speed')
        for servo in before['servos']: servo.pop('speed',None)
        self.assertEqual(after,before)
        self.assertEqual(P.motion_snapshot()['revision'],crc)
        self.assertEqual([s['speed'] for s in self.client.get('/api/actions').get_json()['servos']],[270,180,0])

    def test_old_profile_inherits_target_device_global_speed(self):
        profile = P.load()
        before = Path(P.PROFILE_PATH).read_bytes()
        frame,plan = T.compile_timeline({'steps':[{'at_ms':0,'move':{'type':'home'}}]},profile,45)
        self.assertTrue(frame.startswith('@TIMELINE:'))
        self.assertEqual([s['move']['speed'] for s in plan['steps']],[45,45,45])
        self.assertEqual(Path(P.PROFILE_PATH).read_bytes(),before)

    def test_gestures_home_and_timeline_use_each_axis_default(self):
        self.save()
        for aid,speed,sid in [('nod',180,2),('shake',270,1),('wave',180,2),('look_left',270,1)]:
            response = self.client.post('/api/actions',json={'action_id':aid}).get_json()
            self.assertTrue(response['ok'],response)
            self.assertTrue(all(s['move']['speed']==speed for s in response['action']['plan']['steps']))
            self.assertTrue(all(s['move']['id']==sid for s in response['action']['plan']['steps']))
        home = self.client.post('/api/actions',json={'action_id':'home'}).get_json()
        self.assertEqual([s['move']['speed'] for s in home['action']['plan']['steps']],[270,180,0])
        _,plan = T.compile_timeline({'steps':[{'at_ms':0,'move':{'type':'servo','id':2,'angle':20,'speed':30}}]},P.load())
        self.assertEqual(plan['steps'][0]['move']['speed'],30)
        raw = A.decision_to_raw({'moves':[{'type':'servo','id':2,'angle':20}]})
        self.assertEqual(raw,'@MOVE:servo,2,20,180')
        self.assertEqual(A.decision_to_raw({'moves':[{'type':'servo','id':3,'angle':20}]}),'@MOVE:servo,3,20,0')
        self.assertTrue(A.decision_to_raw({'moves':[{'type':'gesture','name':'shake'}]}).startswith('@TIMELINE:'))

    def test_recognized_text_injects_current_defaults_into_ai_and_command_queue(self):
        self.save()
        captured = {}
        def fake_ai(cfg,messages):
            captured['prompt'] = messages[0]['content']
            return json.dumps({'message':'收到','moves':[{'type':'servo','id':2,'angle':20}]},ensure_ascii=False),{}
        with patch.object(A.ai_relay,'get_ai_decision',side_effect=fake_ai):
            response = self.client.post('/api/data',json={'token':'servo-test','device_id':'other',
                    'user':'向左转头','need_ai':True,'enqueue_decision':True})
        self.assertEqual(response.status_code,200,response.get_json())
        self.assertTrue(response.get_json()['queued_to_device'])
        self.assertIn('默认速度 270°/秒',captured['prompt'])
        self.assertIn('默认速度 180°/秒',captured['prompt'])
        self.assertIn('不要擅自写固定90',captured['prompt'])
        self.assertIn('@MOVE:servo,2,20,180',A.pop_command('other')['raw'])
        self.assertIsNone(A.pop_command('esp-bridge'))

    def test_partial_axis_saves_do_not_lose_other_axes(self):
        errors=[]
        def save_axis(sid,speed):
            try:P.save_speeds({str(sid):speed})
            except Exception as error:errors.append(error)
        threads=[threading.Thread(target=save_axis,args=(sid,speed)) for sid,speed in [(1,270),(2,180),(3,0)]]
        for thread in threads:thread.start()
        for thread in threads:thread.join()
        self.assertEqual(errors,[])
        self.assertEqual([s['speed'] for s in P.load()['servos']],[270,180,0])

    def test_raw_out_of_range_batch_is_rejected_before_enqueue(self):
        for raw in ['@MOVE:servo,1,14#','@MOVE:servo,1,166#','@MOVE:servo,2,91#',
                    '@MOVE:servo,2,-1#','@MOVE:servo,3,181#',
                    '@MOVE:servo,1,90#@MOVE:servo,2,135#']:
            self.assertEqual(self.client.post('/api/command',json={'raw':raw}).status_code,400)
            self.assertEqual(A._queue_len('esp-bridge'),0)


if __name__=='__main__': unittest.main(verbosity=2)
