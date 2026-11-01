"""隔离随机端口与临时数据，通过真实浏览器验证连续采集。

用法：python test_camera_live_browser.py <截图输出目录>。
模拟S3的HTTP上传，实际调用本地YuNet模型；不连接相机或舵机。
"""
import json
from pathlib import Path
import subprocess
import sys
import threading
import time
from unittest.mock import patch

import requests
from werkzeug.serving import make_server, WSGIRequestHandler
from cdp_min import Browser, wait_port
from test_camera_live import LiveTests
import app as A


def main():
    out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    fixture = LiveTests(); fixture.setUp(); fixture.analyze_patch.stop()
    scratch = Path(fixture.tmp.name)
    server = edge = None
    try:
        with patch.object(A.store, 'DB_PATH', str(scratch/'relay.db')), patch.object(A.store, '_initialized', False):
            class Quiet(WSGIRequestHandler):
                def log_request(self, *args): pass
            server = make_server('127.0.0.1', 0, A.app, threaded=True, request_handler=Quiet)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            url = 'http://127.0.0.1:%d' % server.server_port
            edge = subprocess.Popen([r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
                '--headless=new', '--disable-gpu', '--no-first-run', '--remote-debugging-port=9374',
                '--window-size=1440,1100', '--user-data-dir='+str(scratch/'edge'), 'about:blank'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            assert wait_port(9374, seconds=15)
            br = Browser(9374); sid, _ = br.new_page()
            for domain in ['Page', 'Runtime', 'Network']: br.call(domain+'.enable', {}, session=sid)
            client = requests.Session()
            assert client.post(url+'/login', data={'token':'camera-test'}).status_code == 200
            br.call('Network.setCookie', dict(name='session', value=client.cookies.get('session'), url=url, path='/'), session=sid)
            js = lambda code: br.evaluate(code, sid)
            def wait(check):
                end = time.monotonic()+25
                while not check():
                    assert time.monotonic()<end, '浏览器等待超时'
                    time.sleep(.1)
            br.call('Page.navigate', {'url':url+'/#presence'}, session=sid)
            wait(lambda: js("!!document.querySelector('#s3-live-start') && document.querySelector('#camera-config-state').textContent.length>0"))
            js("document.getElementById('s3-live-fps').value='3';document.getElementById('s3-live-start').click()")
            wait(lambda: A.camera.current_settings('esp-bridge')['continuous_fps']==3)
            saved=A.camera.current_settings('esp-bridge')
            assert saved['enabled'] and saved['capture_mode']=='continuous' and saved['camera_transport']=='s3-wifi'
            headers={'Authorization':'Bearer camera-test'}
            lease=requests.post(url+'/api/camera/device/poll', headers=headers,
                json=dict(ready=True,protocol_version=1,camera_type='s3-wifi',continuous_capable=True)).json()
            assert lease['mode']=='continuous',lease
            headers.update({'X-Capture-Ticket':lease['ticket'],'X-Frame-Sequence':'1','Content-Type':'image/jpeg'})
            sample=Path(__file__).resolve().parents[3]/'工具/识别追踪/vision_local/samples/opencv_lena.jpg'
            if not sample.exists(): sample=Path(__file__).resolve().parents[1]/'验证记录/人脸样例/opencv_lena.jpg'
            assert requests.post(url+'/api/camera/device/live-frame',headers=headers,data=sample.read_bytes()).status_code==202
            wait(lambda: js("!document.getElementById('s3-live-view').hidden && !document.getElementById('s3-live-detection').hidden"))
            status=client.get(url+'/api/camera/live/status').json()
            assert status['analysis']['face_count']==1,status
            assert len(status['analysis']['boxes'])==1
            assert js("document.getElementById('s3-live-result').textContent.includes('分析帧 #1')")
            js("document.getElementById('s3-live-panel').scrollIntoView({block:'start'})")
            br.screenshot(sid,str(out/'连续图传识别_浏览器验收.png'))
            js("document.getElementById('s3-live-stop').click()")
            wait(lambda:not A.camera.current_settings('esp-bridge')['enabled'])
            headers['X-Frame-Sequence']='2'
            assert requests.post(url+'/api/camera/device/live-frame',headers=headers,data=sample.read_bytes()).status_code==409
            assert client.get(url+'/api/camera/status').json()['items']==[]
            wait(lambda:js("document.getElementById('s3-live-view').hidden"))
            exceptions=[e for e in br.console_errors() if e.startswith('EXCEPTION')]
            assert not exceptions,exceptions
            print(json.dumps(dict(result='PASS',checks=['按钮保存连续模式及3fps', '模拟S3真实HTTP上传',
                'YuNet检测到1张人脸','最新及识别画面可见','分析帧号对应','停止后拒收旧会话',
                '停止后隐藏预览','连续帧不写历史','无未捕获JS异常'],
                screenshot='连续图传识别_浏览器验收.png'),ensure_ascii=False))
    finally:
        if server:server.shutdown();server.server_close()
        if edge:edge.terminate();edge.wait(timeout=15)
        time.sleep(.5);fixture.doCleanups()


if __name__=='__main__':main()
