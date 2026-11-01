"""S3 JPEG上传、旧链路拒绝、租约与幂等回归。"""
import io
import zlib
import unittest
from PIL import Image
import test_camera_workspace as previous
import camera_workspace as C

class CameraTransportTests(unittest.TestCase):
    setUp = previous.CameraTests.setUp
    poll = previous.CameraTests.poll
    settings = previous.CameraTests.settings
    wait_finished = previous.CameraTests.wait_finished

    def lease(self):
        result=self.client.post('/api/camera/request-device')
        self.assertEqual(result.status_code,202,result.json)
        value=self.poll().json
        self.assertTrue(value['accepted'],value)
        return value

    def send_raw(self,lease,raw=None,**kw):
        if raw is None:
            buf=io.BytesIO();Image.new('RGB',(320,240),'red').save(buf,'JPEG');raw=buf.getvalue()
        headers={'X-Capture-Ticket':lease['ticket'],'X-Image-CRC32':f'{zlib.crc32(raw):08X}'}
        headers.update(kw)
        return self.client.post(self.dev+'upload?job_id='+lease['job_id'],data=raw,
                               content_type='image/jpeg',headers=headers)

    def test_manual_device_works_without_automatic_schedule(self):
        self.settings(source='browser',windows=[{'start':'22:00','end':'23:00'}])
        value=self.lease()
        self.assertEqual(self.client.post('/api/camera/request-device').status_code,409)
        result=self.send_raw(value);self.assertEqual(result.status_code,202,result.json)
        self.assertGreater(result.json['received'],0)
        item=self.wait_finished();self.assertEqual((item['width'],item['height']),(320,240))
        image=Image.open(io.BytesIO(self.client.get(item['image_url']).data))
        self.assertGreater(image.getpixel((100,100))[0],245)
        self.assertLess(image.getpixel((100,100))[1],5)
        # The full body may be resent after a lost HTTP response: same photo/job.
        self.assertEqual(self.send_raw(value).status_code,202)
        self.assertEqual(len(self.client.get('/api/camera/status').json['items']),1)
        self.assertEqual(self.send_raw(value,b'\x07\xe0'*76800).status_code,409)

    def test_old_raw_transport_rejected_without_consuming_lease(self):
        value=self.lease()
        response=self.client.post(self.dev+'upload?job_id='+value['job_id'],
            data=b'\xf8\x00'*76800,content_type='application/x-rgb565',
            headers={'X-Capture-Ticket':value['ticket']})
        self.assertEqual(response.status_code,415)
        self.assertFalse(hasattr(C,'rgb565_image'))
        self.assertEqual(self.send_raw(value).status_code,202)
        self.wait_finished()

    def test_existing_legacy_settings_migrate_and_old_bridge_cannot_poll(self):
        import json
        old=dict(C.DEFAULT,camera_transport='legacy',enabled=True,interval_sec=17)
        # 从实际SQLite旧配置读取，验证自动迁移不会丢掉其它字段。
        with self.ws.db() as db:
            db.execute('INSERT OR REPLACE INTO settings VALUES (?,?)',('esp-bridge',json.dumps(old)))
        settings=self.ws.current_settings('esp-bridge')
        self.assertEqual(settings['camera_transport'],'s3-wifi')
        self.assertEqual(settings['interval_sec'],17)
        self.assertTrue(settings['enabled'])
        response=self.client.post(self.dev+'poll',json=dict(ready=True,protocol_version=1))
        self.assertFalse(response.json['accepted'])
        self.assertNotIn('legacy',self.client.get('/').text)

    def test_paused_busy_expired_restart_cancel_and_report(self):
        for key in ['audio','ota','motion']:
            self.busy[key]=True
            self.assertEqual(self.client.post('/api/camera/request-device').status_code,409)
            self.busy.clear()
        value=self.lease()
        self.assertEqual(self.client.post(self.dev+'report',json={'job_id':value['job_id'],'ticket':'bad','error':'bad'}).status_code,409)
        response=self.client.post(self.dev+'report',json={'job_id':value['job_id'],'ticket':value['ticket'],'error':'SCCB_BUS'})
        self.assertEqual(response.status_code,200)
        self.assertEqual(self.client.get('/api/camera/status').json['items'][0]['error'],'SCCB_BUS')
        value=self.lease();self.now+=61
        self.assertEqual(self.send_raw(value).status_code,409)
        self.settings(paused=True)
        self.assertEqual(self.client.post('/api/camera/request-device').status_code,409)
        self.settings(paused=False);value=self.lease()
        self.settings(paused=True)
        self.assertEqual(self.send_raw(value).status_code,409)

    def test_bearer_does_not_grant_camera_console_or_queue_access(self):
        import app as A
        anon=A.app.test_client()
        for path in ['/api/camera/request-device','/api/camera/settings']:
            self.assertEqual(anon.post(path,headers=self.auth,json={}).status_code,401)
        value=self.lease()
        self.assertEqual(anon.post(self.dev+'report',headers=self.auth,json={'job_id':value['job_id'],'ticket':value['ticket'],'error':'TIMEOUT'}).status_code,200)

if __name__=='__main__':unittest.main()
