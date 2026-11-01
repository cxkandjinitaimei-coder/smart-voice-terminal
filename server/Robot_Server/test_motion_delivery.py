"""在隔离副本中运行：真实Flask完整设备轮询与回执，不联网、不发硬件命令。"""
from unittest.mock import patch
import time
import app as A
import motion_delivery as D

DEV = 'motion-delivery-test'
timeline = {'repeat': 1, 'duration_ms': 17000, 'steps': [
    {'at_ms': i*500, 'move': {'type':'servo', 'id':1 if i%2==0 else 2,
                            'angle':90 if i%2==0 else 30, 'speed':120}}
    for i in range(32)]}

with patch.object(A, 'load_config', return_value={'token':'test-only'}), \
     patch.object(A, 'start_auto_tick'), patch.object(A, 'start_store'), \
     patch.object(A, 'start_beacon'), patch.object(A.asr, 'warmup'), \
     patch.object(A, 'append_line'), patch.object(A.store, 'record_command'), \
     patch.object(A.store, 'record_interaction'), patch.object(A.store, 'record_sensor'):
    browser=A.app.test_client(); gateway=A.app.test_client()
    assert browser.get('/api/motion/delivery?id=bad').status_code==401
    with browser.session_transaction() as session:session['authed']=True
    def enqueue():
        r=browser.post('/api/command',json={'device_id':DEV,'timeline':timeline})
        assert r.status_code==200,r.get_json()
        return r.get_json()['command']['motion_id']
    def view(ident):return browser.get('/api/motion/delivery?id='+ident).get_json()['delivery']
    def report(ident,phase,**extra):
        value=dict(id=ident,phase=phase,sequence=31,attempts=2,board_state=phase+',31,32,32,1,1')
        value.update(extra)
        r=gateway.post('/api/data',json={'token':'test-only','device_id':DEV,'need_ai':False,
                                        'motion_busy':phase in ('running','paused','waiting_ack','stopping','timeout'),
                                        'motion_delivery':value})
        assert r.status_code==200,r.get_json()
        return r.get_json()
    ident=enqueue();assert view(ident)['phase']=='queued'
    response=gateway.post('/api/data',json={'token':'test-only','device_id':DEV,'need_ai':False})
    data=response.get_json();assert response.status_code==200
    assert data['command']['motion_id']==ident
    assert len(bytes.fromhex(data['command']['raw'].split(':',1)[1].rstrip('#')))==584
    assert len(response.data)<4096
    assert A._queue_len(DEV)==0 and view(ident)['phase']=='dispatched'
    # 网页模拟上报不能把投递标成完成。
    browser.post('/api/data',json={'device_id':DEV,'no_pop':True,'need_ai':False,
        'motion_delivery':dict(id=ident,phase='done',sequence=31,attempts=2)})
    assert view(ident)['phase']=='dispatched'
    report('wrong-id','done');assert view(ident)['phase']=='dispatched'
    report(ident,'running',attempts=True);assert view(ident)['phase']=='dispatched'
    next_id=enqueue()
    assert report(ident,'running').get('command') is None
    assert view(ident)['phase']=='running' and A._queue_len(DEV)==1
    assert report(ident,'paused').get('command') is None
    assert view(ident)['phase']=='paused' and view(next_id)['phase']=='queued'
    assert report(ident,'stopping').get('command') is None
    assert view(ident)['phase']=='stopping' and view(next_id)['phase']=='queued'
    assert report(ident,'stopped')['command']['motion_id']==next_id
    assert view(ident)['phase']=='stopped' and view(next_id)['phase']=='dispatched'
    report(ident,'running');assert view(ident)['phase']=='stopped'
    ident=enqueue();A.pop_command(DEV)
    with patch.object(D.time,'time',return_value=time.time()+16):assert view(ident)['phase']=='no_gateway_report'
    report(ident,'timeout');assert view(ident)['phase']=='timeout'
    ident=enqueue();A.pop_command(DEV)
    D.report('wrong-device',dict(id=ident,phase='done',sequence=1,attempts=1))
    assert view(ident)['phase']=='dispatched'
    report(ident,'limits_error');assert view(ident)['phase']=='limits_error'
    enqueue()
    browser.post('/api/command',json={'device_id':DEV,'timeline':{'stop':True}})
    r=report(ident,'running')
    assert r['command']['raw']=='@TIMELINE:STOP#' and A._queue_len(DEV)==1
    assert view('unknown')['phase']=='unknown'
print('PASS: actual 32-step HTTP poll <4KB, auth, queued/dispatched/running/paused/done/timeout/limits, stale report guard, browser isolation, old ESP diagnosis')
