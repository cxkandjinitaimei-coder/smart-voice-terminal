"""定时/声音/冷却策略测试；用临时数据库与假时钟，不打开摄像头。"""
import tempfile
import unittest
from pathlib import Path
import camera_workspace as C


class TriggerTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.now=100.;self.busy={}
        self.ws=C.CameraWorkspace(Path(self.tmp.name),lambda:{},lambda d:self.busy,lambda:self.now)

    def save(self,**kw):
        return self.ws.save('d',dict(C.DEFAULT,enabled=True,**kw))

    def finish(self):
        with self.ws.db() as db:db.execute("UPDATE jobs SET state='done'")

    def test_sound_cooldown_dedup_and_no_delayed_old_sound(self):
        self.save(capture_mode='sound',cooldown_sec=10)
        self.assertFalse(self.ws.request_capture('d','device')['accepted'])
        self.assertTrue(self.ws.sound_event('d','a'));self.assertFalse(self.ws.sound_event('d','a'))
        lease=self.ws.request_capture('d','device');self.assertTrue(lease['accepted']);self.finish()
        self.now+=1;self.assertFalse(self.ws.sound_event('d','b'))
        self.now+=10;self.assertFalse(self.ws.request_capture('d','device')['accepted'])
        self.assertTrue(self.ws.sound_event('d','c'));self.assertTrue(self.ws.request_capture('d','device')['accepted'])

    def test_audio_does_not_defer_sound_but_motion_does(self):
        self.save(capture_mode='sound');self.busy['audio']=True
        self.assertTrue(self.ws.sound_event('d','a'))
        # 说话即拍：录音/上传占用不能把这一张推迟到录音结束。
        self.assertTrue(self.ws.request_capture('d','device')['accepted']);self.finish()
        self.now+=10;self.busy.clear();self.busy['motion']=True
        self.assertTrue(self.ws.sound_event('d','b'))
        self.assertFalse(self.ws.request_capture('d','device')['accepted'])
        self.busy.clear();self.assertTrue(self.ws.request_capture('d','device')['accepted']);self.finish()
        self.now+=11;self.assertTrue(self.ws.sound_event('d','c'));self.now+=31
        self.assertFalse(self.ws.request_capture('d','device')['accepted'])
        self.save(capture_mode='sound',paused=True);self.assertFalse(self.ws.sound_event('d','d'))

    def test_interval_combined_and_manual_override(self):
        self.save(capture_mode='interval_sound',interval_sec=20,cooldown_sec=10)
        self.assertTrue(self.ws.request_capture('d','device')['accepted']);self.finish()
        self.now+=9;self.assertFalse(self.ws.request_capture('d','device')['accepted'])
        self.now+=2;self.assertTrue(self.ws.sound_event('d','a'))
        self.assertTrue(self.ws.request_capture('d','device')['accepted']);self.finish()
        self.now+=20;self.assertTrue(self.ws.request_capture('d','device')['accepted']);self.finish()
        self.ws.queue_device('d');self.assertTrue(self.ws.request_capture('d','device')['accepted'])

    def test_old_config_migration_and_invalid_cooldown(self):
        self.assertEqual(C.validate(dict(interval_sec=2))['cooldown_sec'],2)
        for value in (0,True,3601,'10'):
            with self.assertRaises(C.CameraError):C.validate(dict(cooldown_sec=value))


if __name__=='__main__':unittest.main()
