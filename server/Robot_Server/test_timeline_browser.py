"""Real Edge UI, isolated Flask and data; no physical commands or deployment."""
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from unittest.mock import patch
import requests
from werkzeug.serving import make_server, WSGIRequestHandler
from cdp_min import Browser, wait_port
import app as A
import device_profile as P


def wait(check):
    deadline=time.monotonic()+12
    while not check():
        assert time.monotonic()<deadline, "browser condition timed out"
        time.sleep(.1)


with tempfile.TemporaryDirectory(prefix="timeline-browser-",ignore_cleanup_errors=True) as temp:
    directory=Path(temp)
    with patch.object(P,"PROFILE_PATH",str(directory/"profile.json")), \
         patch.object(A,"load_config",return_value={"token":"timeline-browser-test", "auto_tick":{"enabled":False}}), \
         patch.object(A,"start_auto_tick"),patch.object(A,"start_store"),patch.object(A,"start_beacon"), \
         patch.object(A.asr,"warmup"),patch.object(A,"append_line"),patch.object(A.store,"record_command"), \
         patch.object(A.audio_rx,"WAV_DIR",str(directory/"wav")), \
         patch.object(A.device_controls,"PATH",str(directory/"controls.json")):
        P.save(P.default_profile())
        class Quiet(WSGIRequestHandler):
            def log_request(self,*args): pass
        server=make_server("127.0.0.1",0,A.app,threaded=True,request_handler=Quiet)
        threading.Thread(target=server.serve_forever,daemon=True).start()
        url="http://127.0.0.1:%d" % server.server_port
        port=9363
        edge=subprocess.Popen([r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
                "--headless=new","--disable-gpu","--no-first-run","--remote-debugging-port=%d" % port,
                "--user-data-dir="+str(directory/"edge"),"about:blank"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:
            assert wait_port(port,seconds=10)
            br=Browser(port); sid,_=br.new_page()
            for name in ["Page","Runtime","Network"]: br.call(name+".enable",{},session=sid)
            session=requests.Session(); assert session.post(url+"/login",data={"token":"timeline-browser-test"}).status_code==200
            br.call("Network.setCookie",{"name":"session","value":session.cookies.get("session"),"url":url,"path":"/"},session=sid)
            br.call("Emulation.setDeviceMetricsOverride",{"width":1200,"height":1600,"deviceScaleFactor":1,"mobile":False},session=sid)
            js=lambda code: br.evaluate(code,sid)
            br.call("Page.navigate",{"url":url+"/#test"},session=sid)
            wait(lambda: js("document.querySelector('#timeline-editor') && document.querySelector('#timeline-editor').value.length>0"))
            # Migrate only the legacy home preset; retain explicit user 90-degree commands.
            js("localStorage.setItem('remote_quick_servo_led_v2',JSON.stringify([{name:'三舵归中',frame:'@MOVE:servo,1,90#@MOVE:servo,2,90#@MOVE:servo,3,90#'},{name:'自定义90',frame:'@MOVE:servo,1,90#'}]));location.reload()")
            wait(lambda: js("document.querySelector('#remote-quick') && document.querySelector('#remote-quick').textContent.includes('自定义90')"))
            quick=json.loads(js("localStorage.getItem('remote_quick_servo_led_v2')"))
            assert quick[0]['frame']=='@MOVE:home,all#' and quick[1]['frame']=='@MOVE:servo,1,90#'
            js("Array.from(document.querySelectorAll('#remote-quick button')).find(b=>b.textContent==='三舵归中').click()")
            wait(lambda: A._queue_len('esp-bridge')==1)
            assert A.pop_command('esp-bridge')['raw']=='@MOVE:home,all#'
            data=json.loads(js("document.querySelector('#timeline-editor').value")); assert data["repeat"]==3 and len(data["steps"])==3
            js("document.querySelector('#timeline-preview').click()")
            wait(lambda: "检查通过" in js("document.querySelector('#timeline-result').textContent"))
            assert A._queue_len("esp-bridge")==0
            js("document.querySelector('#timeline-send').click()")
            wait(lambda: A._queue_len("esp-bridge")==1)
            assert A.pop_command("esp-bridge")["raw"].startswith("@TIMELINE:01")
            js("document.querySelector('#timeline-stop').click()")
            wait(lambda: A._queue_len("esp-bridge")==1)
            assert A.pop_command("esp-bridge")["raw"]=="@TIMELINE:STOP#"
            # Save limits through real administrator controls, then regenerate nod.
            js("location.hash='settings'")
            wait(lambda: js("!!document.querySelector('#admin-servo-list input[data-key=min]')"))
            js("for(const [key,value] of [['min',82],['max',98],['front',92]]) {const e=document.querySelector('#admin-servo-list input[data-key='+key+']');e.value=value;e.dispatchEvent(new Event('input',{bubbles:true}));} document.querySelector('#btn-admin-profile-save').click()")
            wait(lambda: P.load()["servos"][0]["front"]==92)
            js("location.hash='test'")
            wait(lambda: js("document.querySelector('[data-pane=test]').classList.contains('active')"))
            wait(lambda: '正面 92°' in js("document.querySelector('#servo-test-panel').textContent"))
            js("Array.from(document.querySelectorAll('#servo-test-panel button')).find(b=>b.textContent==='正面 92°').click()")
            wait(lambda: A._queue_len('esp-bridge')==1)
            assert A.pop_command('esp-bridge')['raw']=='@MOVE:home,1,90#'
            js("document.querySelector('#timeline-example').value='nod';document.querySelector('#timeline-fill').click()")
            wait(lambda: json.loads(js("document.querySelector('#timeline-editor').value"))["steps"][0]["move"]["type"]=="gesture")
            nod=json.loads(js("document.querySelector('#timeline-editor').value"))
            normalized=session.post(url+"/api/timeline/preview",json={"timeline":nod}).json()["timeline"]
            assert [s["move"]["angle"] for s in normalized["steps"]]==[82,98,82,92]
            # A hardware ack is rendered as a hardware fact, never inferred from enqueue.
            A._device_last_seen["esp-bridge"]=A.datetime.now()
            A._motion_status["esp-bridge"]={"state":"running,7,2,4,1,3","limits_applied":True,
                                           "profile_cached":True,"home":"1=92,2=90,3=90"}
            wait(lambda: "执行中" in js("document.querySelector('#timeline-status').textContent"))
            assert "已同步到 MCU" in js("document.querySelector('#timeline-status').textContent")
            assert "ESP断电缓存：已保存" in js("document.querySelector('#timeline-status').textContent")
            js("document.querySelector('#timeline-editor').value='{}';document.querySelector('#timeline-send').click()")
            wait(lambda: "未下发" in js("document.querySelector('#timeline-result').textContent"))
            assert A._queue_len("esp-bridge")==0
            assert not br.console_errors(), br.console_errors()
            js("document.querySelector('#timeline-editor').scrollIntoView()")
            evidence=Path(__file__).parent/"timeline_validation"
            evidence.mkdir(exist_ok=True)
            br.screenshot(sid,str(evidence/"时间序列网页.png"))
            print("PASS real Edge: legacy-home migration, explicit90 retained, semantic home buttons/current front, cached MCU status, preview/send/stop, edited limits/nod, invalid refused, no console errors")
        finally:
            try: br.call("Browser.close")
            except Exception: pass
            try: edge.wait(timeout=10)
            except subprocess.TimeoutExpired: edge.terminate();edge.wait(timeout=10)
            server.shutdown()
