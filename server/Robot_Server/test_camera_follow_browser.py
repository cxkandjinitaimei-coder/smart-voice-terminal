"""隔离的Edge浏览器验收：保存相机链路、跟随参数与停止入口。

运行本文件会启动随机端口的临时Flask和无头Edge，结束后关闭二者。
不占用现有5000、不读写用户图库、不连接硬件；截图写入指定验证目录。
"""
import base64
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
from test_camera_follow import FollowAPITests
import app as A


def main():
    out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    fixture = FollowAPITests(); fixture.setUp()
    scratch = Path(fixture.tmp.name)
    server = edge = None
    try:
        with patch.object(A.store, 'DB_PATH', str(scratch/'relay.db')), patch.object(A.store, '_initialized', False):
            class Quiet(WSGIRequestHandler):
                def log_request(self, *args):
                    pass
            server = make_server('127.0.0.1', 0, A.app, threaded=True, request_handler=Quiet)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            url = 'http://127.0.0.1:%d' % server.server_port
            edge = subprocess.Popen([r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
                '--headless=new', '--disable-gpu', '--no-first-run', '--remote-debugging-port=9374',
                '--window-size=1440,1100', '--user-data-dir='+str(scratch/'edge'), 'about:blank'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            assert wait_port(9374, seconds=15)
            br = Browser(9374); sid, _ = br.new_page()
            for domain in ['Page', 'Runtime', 'Network']:
                br.call(domain+'.enable', {}, session=sid)
            session = requests.Session()
            assert session.post(url+'/login', data={'token': 'camera-test'}).status_code == 200
            br.call('Network.setCookie', dict(name='session', value=session.cookies.get('session'), url=url, path='/'), session=sid)
            js = lambda code: br.evaluate(code, sid)
            def wait(check):
                end = time.monotonic()+20
                while not check():
                    assert time.monotonic()<end, '浏览器等待超时'
                    time.sleep(.1)
            br.call('Page.navigate', {'url': url+'/#presence'}, session=sid)
            wait(lambda: js("!!document.querySelector('#camera-transport') && document.querySelector('#camera-config-state').textContent.length>0"))
            js("""for(const e of document.querySelectorAll('details.camera-capture-settings, #camera-settings details'))e.open=true;
              document.getElementById('camera-transport').value='s3-wifi';
              document.getElementById('camera-enabled').checked=true;
              document.getElementById('camera-follow-enabled').checked=true;
              document.getElementById('camera-follow-step').value='3';
              document.getElementById('camera-settings').requestSubmit();""")
            wait(lambda: A.camera.current_settings('esp-bridge')['follow_enabled'])
            saved = A.camera.current_settings('esp-bridge')
            assert saved['camera_transport']=='s3-wifi' and saved['follow_step']==3
            br.call('Page.reload', {}, session=sid)
            wait(lambda: js("document.querySelector('#camera-follow-enabled')?.checked && document.querySelector('#camera-follow-step')?.value==='3'"))
            js("for(const e of document.querySelectorAll('details.camera-capture-settings, #camera-settings details'))e.open=true;document.getElementById('camera-follow-state').scrollIntoView({block:'center'})")
            br.screenshot(sid, str(out/'跟随设置_浏览器验收.png'))
            js("document.getElementById('camera-follow-stop').click()")
            wait(lambda: not A.camera.current_settings('esp-bridge')['follow_enabled'])
            wait(lambda: js("document.getElementById('camera-message').textContent.includes('已停止自动跟随')"))
            exceptions = [e for e in br.console_errors() if e.startswith('EXCEPTION')]
            assert not exceptions, exceptions
            print(json.dumps(dict(result='PASS', checks=['真实表单保存', '链路选择', '参数持久化', '刷新恢复', '停止跟随', '无未捕获JS异常'], screenshot='跟随设置_浏览器验收.png'), ensure_ascii=False))
    finally:
        if server:
            server.shutdown(); server.server_close()
        if edge:
            edge.terminate(); edge.wait(timeout=15)
        # Edge子进程短暂持有缓存文件；让测试清理在其退出后执行。
        time.sleep(.5)
        fixture.doCleanups()


if __name__ == '__main__':
    main()
