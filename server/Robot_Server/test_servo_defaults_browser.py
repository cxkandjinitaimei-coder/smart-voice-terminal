"""Use the real test-page speed save button, reload, and interaction parameter view."""
from contextlib import ExitStack
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from unittest.mock import patch

import requests
from werkzeug.serving import make_server, WSGIRequestHandler
from cdp_min import Browser, wait_port
import app as A
import device_profile as P

sys.stdout.reconfigure(encoding='utf-8')


def wait(check):
    deadline=time.monotonic()+15
    while not check():
        assert time.monotonic()<deadline,'Browser condition timed out'
        time.sleep(.1)


with tempfile.TemporaryDirectory(prefix='servo-default-browser-') as temp, ExitStack() as scope:
    directory=Path(temp)
    for obj,name,value in [(P,'PROFILE_PATH',str(directory/'profile.json')),
            (A.device_controls,'PATH',str(directory/'controls.json')),
            (A.audio_rx,'WAV_DIR',str(directory/'wav')),
            (A.audio_rx,'_STATE_FILE',str(directory/'audio_state.json')),
            (A.store,'DB_PATH',str(directory/'relay.db')),(A.store,'_initialized',False)]:
        scope.enter_context(patch.object(obj,name,value))
    for obj in [A,A.store]:
        for name in ['DATA_HISTORY','DECISION_LOG']:
            scope.enter_context(patch.object(obj,name,str(directory/(name+'.txt'))))
    scope.enter_context(patch.object(A,'load_config',return_value={'token':'speed-browser-test','auto_tick':{'enabled':False}}))
    for name in ['start_auto_tick','start_store','start_beacon','append_line']:
        scope.enter_context(patch.object(A,name))
    scope.enter_context(patch.object(A.asr,'warmup'))
    P.save(P.default_profile())
    A.device_controls.save('esp-bridge',dict(A.device_controls.DEFAULT,servo_speed=90))
    A._pending_commands.clear()
    class Quiet(WSGIRequestHandler):
        def log_request(self,*args):pass
    server=make_server('127.0.0.1',0,A.app,threaded=True,request_handler=Quiet)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    url='http://127.0.0.1:%d'%server.server_port
    edge=subprocess.Popen([r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
        '--headless=new','--disable-gpu','--no-first-run','--remote-debugging-port=9368',
        '--user-data-dir='+str(directory/'edge'),'about:blank'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        assert wait_port(9368,seconds=10)
        br=Browser(9368);sid,_=br.new_page()
        for name in ['Page','Runtime','Network']:br.call(name+'.enable',{},session=sid)
        session=requests.Session();assert session.post(url+'/login',data={'token':'speed-browser-test'}).status_code==200
        br.call('Network.setCookie',{'name':'session','value':session.cookies.get('session'),'url':url,'path':'/'},session=sid)
        js=lambda code:br.evaluate(code,sid)
        br.call('Page.navigate',{'url':url+'/#test'},session=sid)
        wait(lambda:js("!!document.querySelector('button[data-servo-speed-save=\"1\"]')"))
        # The old refreshControls path referenced undefined SERVO_GROUPS.
        wait(lambda:js("!document.querySelector('#controls-status').textContent.includes('读取参数失败') && document.querySelector('#controls-status').textContent.length > 0"))
        for servo,value in [(1,270),(2,180),(3,0)]:
            js(f"document.querySelector('#servo-speed-{servo}').value='{value}';document.querySelector('button[data-servo-speed-save=\"{servo}\"]').click()")
            wait(lambda:next(s for s in P.load()['servos'] if s['id']==servo).get('speed')==value)
            wait(lambda:js(f"!!document.querySelector('button[data-servo-speed-save=\"{servo}\"]:not([disabled])')"))
        calibration=[{k:s[k] for k in ['id','min','max','front']} for s in P.load()['servos']]
        br.call('Page.reload',{},session=sid)
        wait(lambda:js("document.querySelector('#servo-speed-1')?.value === '270' && document.querySelector('#servo-speed-2')?.value === '180' && document.querySelector('#servo-speed-3')?.value === '0'"))
        js("location.hash='#interact'")
        wait(lambda:js("document.querySelector('#action-library-params').textContent.includes('默认速度270°/秒') && document.querySelector('#action-library-params').textContent.includes('默认速度180°/秒')"))
        js("document.querySelector('#action-library button[data-action-id=shake]').click()")
        wait(lambda:A._queue_len('esp-bridge')>0)
        command=A.pop_command('esp-bridge')
        packet=bytes.fromhex(command['raw'].split(':',1)[1])
        import struct
        assert struct.unpack_from('<H',packet,16)[0]==270
        js("location.hash='#settings'")
        wait(lambda:js("document.querySelector('#admin-servo-list input[data-key=speed]')?.value === '270'"))
        assert calibration==[{k:s[k] for k in ['id','min','max','front']} for s in P.load()['servos']]
        print('PASS real Edge: test page saves 270/180/0, reload retains values, controls refresh no undefined variable, interaction shows per-axis speeds, shake wire uses180, admin editor shares saved values')
    finally:
        server.shutdown();edge.terminate()
        try:edge.wait(timeout=5)
        except subprocess.TimeoutExpired:edge.kill();edge.wait(timeout=5)
