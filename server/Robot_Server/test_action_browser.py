"""Real interaction page buttons, live admin edits and unavailable states in Edge."""
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
from cdp_min import Browser,wait_port
import app as A
import device_profile as P

sys.stdout.reconfigure(encoding='utf-8')
def wait(check):
    deadline=time.monotonic()+15
    while not check():
        assert time.monotonic()<deadline,'browser condition timed out'
        time.sleep(.1)

with tempfile.TemporaryDirectory(prefix='action-browser-',ignore_cleanup_errors=True) as temp,ExitStack() as scope:
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
    scope.enter_context(patch.object(A,'load_config',return_value={'token':'action-browser-test','auto_tick':{'enabled':False}}))
    for name in ['start_auto_tick','start_store','start_beacon','append_line']:
        scope.enter_context(patch.object(A,name))
    scope.enter_context(patch.object(A.asr,'warmup'))
    profile=P.default_profile()
    for s,front,maximum in zip(profile['servos'],[30,60,90],[60,120,180]):
        s.update(min=0,mid=front,front=front,max=maximum)
    P.save(profile)
    A.device_controls.save('esp-bridge',dict(A.device_controls.DEFAULT,servo_speed=30))
    A._pending_commands.clear()
    class Quiet(WSGIRequestHandler):
        def log_request(self,*args):pass
    server=make_server('127.0.0.1',0,A.app,threaded=True,request_handler=Quiet)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    url='http://127.0.0.1:%d'%server.server_port
    edge=subprocess.Popen([r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
        '--headless=new','--disable-gpu','--no-first-run','--remote-debugging-port=9367',
        '--user-data-dir='+str(directory/'edge'),'about:blank'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        assert wait_port(9367,seconds=10)
        br=Browser(9367);sid,_=br.new_page()
        for name in ['Page','Runtime','Network']:br.call(name+'.enable',{},session=sid)
        session=requests.Session();assert session.post(url+'/login',data={'token':'action-browser-test'}).status_code==200
        br.call('Network.setCookie',{'name':'session','value':session.cookies.get('session'),'url':url,'path':'/'},session=sid)
        br.call('Emulation.setDeviceMetricsOverride',{'width':1200,'height':1400,'deviceScaleFactor':1,'mobile':False},session=sid)
        js=lambda code:br.evaluate(code,sid)
        br.call('Page.navigate',{'url':url+'/#interact'},session=sid)
        wait(lambda:js("!!document.querySelector('#action-library button[data-action-id=nod]')"))
        text=js("document.querySelector('#action-library-params').textContent")
        assert '正面30°' in text and '正面60°' in text and '30°/秒' in text
        assert 'F407 还没有定时动作队列' not in js("document.querySelector('[data-pane=interact]').textContent")
        for aid in ['nod','shake','wave','look_left','look_right','nod_twice_timeline','home','happy']:
            wait(lambda:js("!document.querySelector('#action-library button[data-action-id="+aid+"]').disabled"))
            js("document.querySelector('#action-library button[data-action-id="+aid+"]').click()")
            wait(lambda:A._queue_len('esp-bridge')==1)
            raw=A.pop_command('esp-bridge')['raw']
            if aid=='home':assert raw=='@MOVE:home,all'
            elif aid=='happy':assert raw=='@MOVE:expression,happy'
            else:assert raw.startswith('@TIMELINE:')
            wait(lambda:'已入队' in js("document.querySelector('#action-library-result').textContent"))
            assert '等待设备执行' in js("document.querySelector('#action-library-result').textContent")
        # Save through actual administrator UI; returning to interaction must refresh summaries.
        js("location.hash='settings'")
        wait(lambda:js("!!document.querySelector('#admin-servo-list input[data-key=front]')"))
        js("const e=document.querySelector('#admin-servo-list input[data-key=front]');e.value=35;e.dispatchEvent(new Event('input',{bubbles:true}));document.querySelector('#btn-admin-profile-save').click()")
        wait(lambda:P.load()['servos'][0]['front']==35)
        js("location.hash='interact'")
        wait(lambda:'正面35°' in js("document.querySelector('#action-library-params').textContent"))
        assert '收尾回正35°' in js("document.querySelector('#action-library button[data-action-id=nod]').title")
        js("document.querySelector('#action-library button[data-action-id=nod]').click()")
        wait(lambda:A._queue_len('esp-bridge')==1)
        data=bytes.fromhex(A.pop_command('esp-bridge')['raw'].split(':')[1])
        assert int.from_bytes(data[8+3*18+6:8+3*18+8],'little')==35
        # Refresh while staying on the page, then missing role must disable the button.
        profile=P.load();profile['servos'][0]['front']=37
        assert session.post(url+'/api/admin/device-profile',json={'profile':profile}).status_code==200
        wait(lambda:'正面37°' in js("document.querySelector('#action-library-params').textContent"))
        profile['servos'][0]['role']='user_defined'
        assert session.post(url+'/api/admin/device-profile',json={'profile':profile}).status_code==200
        wait(lambda:js("document.querySelector('#action-library button[data-action-id=nod]').disabled"))
        assert 'pitch' in js("document.querySelector('#action-library button[data-action-id=nod]').title")
        assert A._queue_len('esp-bridge')==0
        assert not br.console_errors(),br.console_errors()
        profile['servos'][0]['role']='pitch'
        assert session.post(url+'/api/admin/device-profile',json={'profile':profile}).status_code==200
        wait(lambda:not js("document.querySelector('#action-library button[data-action-id=nod]').disabled"))
        js("document.querySelector('#action-library').scrollIntoView({block:'center'})")
        evidence=Path(__file__).parent/'action_validation'
        evidence.mkdir(exist_ok=True)
        br.screenshot(sid,str(evidence/'互动动作网页.png'))
        print('PASS real Edge interaction: 8 existing/new buttons, complete task enqueue, current30/60/90 and speed, admin UI save, return/live refresh, new-front encoded, missing-role disabled, no console errors; temporary data only')
    finally:
        try:br.call('Browser.close')
        except Exception:pass
        try:edge.wait(timeout=10)
        except subprocess.TimeoutExpired:edge.terminate();edge.wait(timeout=10)
        server.shutdown()
