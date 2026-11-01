"""Isolated Edge UI checks; no physical device or production server access."""
import base64,json,socket,subprocess,threading,time,sys
from pathlib import Path
from unittest.mock import patch
import requests
from werkzeug.serving import make_server,WSGIRequestHandler
from cdp_min import Browser,wait_port
from test_companion_photo import PhotoAPI
import app as A
import companion_runtime as R

def main():
    out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
    fixture=PhotoAPI();fixture.setUp();server=edge=None
    try:
        root=Path(fixture.tmp.name)
        with patch.object(A.store,'DB_PATH',str(root/'relay.db')),patch.object(A.store,'_initialized',False),patch.object(A.idle_pet,'PATH',str(root/'idle.json')):
            class Quiet(WSGIRequestHandler):
                def log_request(self,*args):pass
            server=make_server('127.0.0.1',0,A.app,threaded=True,request_handler=Quiet)
            threading.Thread(target=server.serve_forever,daemon=True).start()
            url='http://127.0.0.1:%s'%server.server_port
            with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
            edge=subprocess.Popen([r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
                '--headless=new','--disable-gpu','--no-first-run','--remote-debugging-port='+str(port),
                '--window-size=1440,1100','--user-data-dir='+str(root/'edge'),'about:blank'],
                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
            assert wait_port(port,seconds=15)
            deadline=time.monotonic()+20
            while True:
                try:
                    ready=requests.get('http://127.0.0.1:%d/json/version'%port,timeout=1)
                    if ready.status_code==200 and ready.json().get('webSocketDebuggerUrl'):break
                except (requests.RequestException,ValueError):pass
                if time.monotonic()>deadline:raise RuntimeError('Edge CDP was not ready')
                time.sleep(.2)
            br=Browser(port);sid,_=br.new_page()
            for domain in ['Page','Runtime','Network']:br.call(domain+'.enable',{},session=sid)
            session=requests.Session();session.post(url+'/login',data={'token':'camera-test'},timeout=3)
            br.call('Network.setCookie',dict(name='session',value=session.cookies.get('session'),url=url,path='/'),session=sid)
            js=lambda code:br.evaluate(code,sid)
            def wait(check):
                end=time.monotonic()+15
                while not check():
                    if time.monotonic()>end:raise AssertionError('UI wait timed out')
                    time.sleep(.1)
            for name in ['page-trainer.js','page-presence.js']:
                source=(Path(__file__).parent/'static'/name).read_text(encoding='utf-8')
                assert js('(()=>{new Function('+json.dumps(source)+');return true})()')
            br.call('Page.navigate',{'url':url+'/#presence'},session=sid)
            wait(lambda:js("document.getElementById('camera-auto-status')?.textContent.length>0"))
            assert js("document.getElementById('camera-follow-start').textContent.includes('拍照定位')")
            js("document.getElementById('camera-follow-start').click()")
            wait(lambda:R.LOCATOR.status('esp-bridge')['phase']=='waiting_photo')
            assert A.camera.current_settings('esp-bridge')['follow_enabled'] is False
            js("document.getElementById('camera-locate-cancel').click()")
            wait(lambda:R.LOCATOR.status('esp-bridge')['phase']=='cancelled')
            js("window.DASH_API.showTab('trainer')")
            wait(lambda:js("document.querySelectorAll('#short-buttons button').length===4"))
            assert js("document.getElementById('short-buttons').textContent.includes('歪头')")
            assert js("!document.querySelector('#timeline-editor')")
            image=br.call('Page.captureScreenshot',{'format':'png','captureBeyondViewport':False},session=sid)
            (out/'短动作页.png').write_bytes(base64.b64decode(image['data']))
            js("window.DASH_API.showTab('presence')")
            wait(lambda:js("document.getElementById('pane-presence').classList.contains('active')"))
            image=br.call('Page.captureScreenshot',{'format':'png','captureBeyondViewport':False},session=sid)
            (out/'单次定位页.png').write_bytes(base64.b64decode(image['data']))
            print('PASS: JS parse; single-photo button; request; cancellation; no persistent following; four short gestures; no long-record editor; two screenshots')
    finally:
        if edge:
            edge.terminate()
            try:edge.wait(timeout=5)
            except subprocess.TimeoutExpired:edge.kill()
        if server:server.shutdown();server.server_close()
        fixture.doCleanups()

if __name__=='__main__':main()
