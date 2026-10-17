"""test_camera_transport.py：使用真实服务模块的离线回归。

使用：由 tools/run_checks.py 在临时服务副本中单独运行；不连接硬件、不调用真实 AI。
范围：断言涵盖本文件列出的状态/边界；测试通过不代表飞线信号、供电和公网时延已验证。
依赖：server/Robot_Server/requirements.txt；数据由测试临时创建并清理。
"""
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
        if raw is None:raw=b'\xf8\x00'*76800
        headers={'X-Capture-Ticket':lease['ticket'],'X-Image-CRC32':f'{zlib.crc32(raw):08X}'}
        headers.update(kw)
        return self.client.post(self.dev+'upload?job_id='+lease['job_id'],data=raw,
                               content_type='application/x-rgb565',headers=headers)

    def test_manual_device_works_without_automatic_schedule(self):
        self.settings(source='browser',windows=[{'start':'22:00','end':'23:00'}])
        value=self.lease()
        self.assertEqual(self.client.post('/api/camera/request-device').status_code,409)
        result=self.send_raw(value);self.assertEqual(result.status_code,202,result.json)
        self.assertEqual(result.json['received'],153600)
        item=self.wait_finished();self.assertEqual((item['width'],item['height']),(320,240))
        image=Image.open(io.BytesIO(self.client.get(item['image_url']).data))
        self.assertGreater(image.getpixel((100,100))[0],245)
        self.assertLess(image.getpixel((100,100))[1],5)
        # The full body may be resent after a lost HTTP response: same photo/job.
        self.assertEqual(self.send_raw(value).status_code,202)
        self.assertEqual(len(self.client.get('/api/camera/status').json['items']),1)
        self.assertEqual(self.send_raw(value,b'\x07\xe0'*76800).status_code,409)

    def test_be_color_order_and_crc_length_validation(self):
        raw=b'\xf8\x00'*25600+b'\x07\xe0'*25600+b'\x00\x1f'*25600
        data,w,h=C.rgb565_image(raw,f'{zlib.crc32(raw):08X}')
        image=Image.open(io.BytesIO(data))
        for point,channel in [((10,10),0),((10,100),1),((10,200),2)]:
            pixel=image.getpixel(point)
            self.assertGreater(pixel[channel],245)
            self.assertLess(sum(pixel)-pixel[channel],10)
        value=self.lease()
        for payload,headers in [(raw[:-2],{}),(raw,{'X-Image-CRC32':'00000000'}),(raw,{'X-Image-CRC32':'x'})]:
            self.assertIn(self.send_raw(value,payload,**headers).status_code,(400,409))
        self.assertEqual(self.send_raw(value,raw).status_code,202)
        self.wait_finished()

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
