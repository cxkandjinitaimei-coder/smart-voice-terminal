"""隔离测试固定间隔编译、网关窗口、持久动作与真实Flask轮询；不联网或驱动硬件。"""
import copy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import sampled_motion as M
import device_profile as P


def action(interval=500):
    return dict(interval_ms=interval, mapping=[dict(id=i, sign=1, offset=0) for i in (1,2,3)],
        recording=dict(schema=1, pose_schema=2, id='train_fixture', name='三轴测试', expression='happy',
            trim=[0,1000], samples=[dict(t=0,yaw=90,pitch=30,roll=90),dict(t=1000,yaw=100,pitch=40,roll=110)]))


class SampleTests(unittest.TestCase):
    def setUp(self):
        self.profile=P.effective(P.default_profile(),90)
        self.now=100.
        self.player=M.Player(lambda:self.now)

    def test_interpolation_endpoint_timing_and_roll(self):
        for interval, count in [(500,3),(100,11)]:
            c=M.compile_recording(action(interval),self.profile)
            self.assertEqual(len(c['steps']),count)
            self.assertEqual(c['duration_ms'],3500)
            self.assertEqual(c['steps'][-1]['angles'],[100,40,110])
            self.assertEqual(c['steps'][1]['speeds'],[10,10,20])
            self.assertEqual(c['steps'][0]['dt_ms'],2500)
        a=action();a['recording']['trim']=[250,850]
        c=M.compile_recording(a,self.profile)
        self.assertEqual(c['steps'][0]['angles'],[93,33,95])
        self.assertEqual(c['steps'][-1]['dt_ms'],100)

    def test_not_limited_to_old_32_steps(self):
        a=action(100);a['recording']['samples'][-1]['t']=30000;a['recording']['trim']=[0,30000]
        self.assertEqual(len(M.compile_recording(a,self.profile)['steps']),301)

    def test_invalid_calibration_mapping_and_speed(self):
        a=action();a['recording']['samples'][-1]['pitch']=91
        with self.assertRaises(ValueError):M.compile_recording(a,self.profile)
        a=action();a['mapping'][1]['id']=1
        with self.assertRaises(ValueError):M.compile_recording(a,self.profile)
        a=action(100);a['recording']['samples'].insert(1,dict(t=100,yaw=160,pitch=30,roll=90))
        with self.assertRaises(ValueError):M.compile_recording(a,self.profile)
        for value in (None,True,[],dict(recording=[])):
            with self.assertRaises(ValueError):M.compile_recording(value,self.profile)

    def test_window_repeats_progress_stop_and_no_false_done(self):
        c=M.compile_recording(action(),self.profile);job=self.player.start('d',c)
        packet=self.player.exchange('d',dict(protocol=1,id='',active=False))
        self.assertEqual(packet['start'],0)
        self.assertEqual(packet,self.player.exchange('d',dict(protocol=1,id='',active=False)))
        r=dict(protocol=1,id=job['id'],accepted=3,sent=1,phase='running',active=True)
        self.assertEqual(self.player.exchange('d',r)['start'],3)
        r.update(phase='done');self.player.exchange('d',r)
        self.assertTrue(self.player.busy('d'))
        r.update(sent=True);self.player.exchange('d',r);self.assertTrue(self.player.busy('d'))
        self.player.stop('d');r.update(sent=1,phase='running')
        self.assertTrue(self.player.exchange('d',r)['stop'])
        r.update(phase='stopped');self.player.exchange('d',r);self.assertFalse(self.player.busy('d'))

    def test_stop_before_transfer_and_transfer_timeout(self):
        c=M.compile_recording(action(),self.profile)
        self.player.start('d',c)
        self.assertTrue(self.player.busy('d'))
        self.player.stop('d')
        self.assertFalse(self.player.busy('d'))
        self.assertEqual(self.player.status('d')['phase'],'stopped')
        # 网关一直领取但 F407 不回 accepted：15 秒后自动失败，不能永久占着相机/队列。
        job=self.player.start('d',c)
        self.now+=10
        self.player.exchange('d',dict(protocol=1,id=job['id'],accepted=0,sent=0,
                                      phase='transferring',active=True))
        self.now+=6
        self.assertFalse(self.player.busy('d'))
        self.assertEqual(self.player.status('d')['phase'],'timeout')

    def test_disconnect_reboot_and_changed_limits(self):
        c=M.compile_recording(action(),self.profile)
        job=self.player.start('d',c);self.now+=13
        self.assertEqual(self.player.status('d')['phase'],'offline')
        self.assertTrue(self.player.exchange('d',dict(active=True,id=job['id']))['stop'])
        job=self.player.start('d',c)
        self.player.exchange('d',dict(protocol=1,id=job['id'],accepted=3,sent=1,phase='running'))
        self.player.exchange('d',dict(protocol=1,id='',active=False))
        self.assertEqual(self.player.status('d')['phase'],'gateway_restarted')
        self.player.start('d',c);self.profile['servos'][0]['max']=95
        self.assertTrue(self.player.exchange('d',dict(protocol=1,id=''),self.profile)['stop'])

    def test_library_revalidates_current_calibration(self):
        with tempfile.TemporaryDirectory() as tmp:
            lib=M.Library(Path(tmp)/'actions.json');lib.save(action())
            value=lib.read()['train_fixture'];self.profile['servos'][2]['max']=100
            with self.assertRaises(ValueError):M.compile_recording(value,self.profile)


class APITests(unittest.TestCase):
    @unittest.skip('03 简洁链路已禁用录制长动作，改由 joint_test 覆盖 410')
    def test_real_poll_auth_browser_isolation_and_camera_exclusion(self):
        import app as A
        with tempfile.TemporaryDirectory() as tmp, patch.multiple(A,start_auto_tick=lambda:None,
            start_store=lambda:None,start_beacon=lambda:None,append_line=lambda *a:None), \
            patch.object(A,'load_config',return_value={'token':'fixture'}), \
            patch.object(A.asr,'warmup'), patch.object(A,'sampled_player',M.Player()), \
            patch.object(A,'sampled_library',M.Library(Path(tmp)/'lib.json')), \
            patch.object(A.device_profile,'load',return_value=P.default_profile()), \
            patch.object(A.device_controls,'get',return_value=dict(servo_speed=90)), \
            patch.object(A.camera,'current_settings',return_value={}):
            browser=A.app.test_client();gateway=A.app.test_client();dev='sampled-test'
            self.assertEqual(browser.get('/api/recorded-motion/status').status_code,401)
            with browser.session_transaction() as s:s['authed']=True
            A._pending_commands.clear()
            r=browser.post('/api/recorded-motion/preview',json=dict(action=action(),device_id=dev))
            self.assertEqual(r.status_code,200,r.json)
            self.assertFalse(A.sampled_player.busy(dev))
            r=browser.post('/api/recorded-motion/start',json=dict(action=action(),device_id=dev))
            self.assertEqual(r.status_code,200,r.json);ident=r.json['playback']['id']
            self.assertTrue(A.camera_blockers(dev)['motion'])
            browser.post('/api/data',json=dict(device_id=dev,no_pop=True,need_ai=False,
                step_playback=dict(protocol=1,id=ident,accepted=3,sent=3,phase='done')))
            self.assertEqual(A.sampled_player.status(dev)['accepted'],0)
            r=gateway.post('/api/data',json=dict(token='fixture',device_id=dev,need_ai=False,
                step_playback=dict(protocol=1,id='',active=False)))
            self.assertEqual(r.status_code,200,r.json)
            self.assertEqual(len(r.json['step_playback']['steps']),3)
            A.enqueue_command(dev,dict(raw='@MOVE:servo,1,90,90#'))
            r=gateway.post('/api/data',json=dict(token='fixture',device_id=dev,need_ai=False,
                step_playback=dict(protocol=1,id=ident,accepted=3,sent=1,phase='running',active=True)))
            self.assertNotIn('command',r.json);self.assertEqual(A._queue_len(dev),1)
            self.assertEqual(browser.post('/api/recorded-motion/start',json=dict(action=action(),device_id=dev)).status_code,409)
            browser.post('/api/recorded-motion/stop',json=dict(device_id=dev))
            r=gateway.post('/api/data',json=dict(token='fixture',device_id=dev,need_ai=False,
                step_playback=dict(protocol=1,id=ident,accepted=3,sent=1,phase='running',active=True)))
            self.assertTrue(r.json['step_playback']['stop'])
            # 声音事件只跟成功的第一块/有效流会话关联，重传由相机策略去重。
            with patch.object(A.camera,'sound_event') as sound, patch.object(A.audio_rx,'feed_chunk',return_value={'ok':True}):
                gateway.post('/api/audio/chunk?token=fixture&device_id=sampled-test&sess=a&seq=0',data=b'pcm')
                sound.assert_called_once_with(dev,'a')
                gateway.post('/api/audio/chunk?token=fixture&device_id=sampled-test&sess=a&seq=1',data=b'pcm')
                self.assertEqual(sound.call_count,1)
            A._pending_commands.clear()


if __name__=='__main__':unittest.main()
