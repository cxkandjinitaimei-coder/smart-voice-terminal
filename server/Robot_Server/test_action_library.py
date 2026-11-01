"""Exercise real authenticated action routes with old recipes and live parameters."""
import copy
import json
from pathlib import Path
import struct
import tempfile
from unittest.mock import patch
import app as A
import device_profile as P

with tempfile.TemporaryDirectory(prefix='action-library-') as temp:
    with patch.object(P,'PROFILE_PATH',str(Path(temp)/'profile.json')), \
         patch.object(A,'start_auto_tick'),patch.object(A,'start_store'),patch.object(A,'start_beacon'), \
         patch.object(A.asr,'warmup'),patch.object(A,'append_line'),patch.object(A.store,'record_command'), \
         patch.object(A.device_controls,'get',return_value={'servo_speed':30}):
        profile=P.default_profile()
        for s,front,maximum in zip(profile['servos'],[30,60,90],[60,120,180]):
            s.update(min=0,mid=front,front=front,max=maximum)
        # User's legacy stored ID/gesture records must update without editing angles/files.
        profile['actions']=[dict(a,moves=[{'type':'gesture','name':a['id']}]) for a in profile['actions'] if a['id']!='home']
        Path(P.PROFILE_PATH).write_text(json.dumps(profile),encoding='utf-8')
        original=Path(P.PROFILE_PATH).read_bytes()
        A._pending_commands.clear()
        client=A.app.test_client()
        assert client.post('/api/actions',json={'action_id':'nod'}).status_code==401
        with client.session_transaction() as session:session['authed']=True
        library=client.get('/api/actions').get_json()
        assert library['servo_speed']==30
        assert any(a['id']=='home' for a in library['actions'])
        assert Path(P.PROFILE_PATH).read_bytes()==original
        # 当前约定：S1=yaw，S2=pitch；管理员角度可改，轴身份固定。
        expected={'nod':(2,[0,120,0,60]),'shake':(1,[0,60,30]),'wave':(2,[120,0,120,60]),
                  'look_left':(1,[0]),'look_right':(1,[60]),'nod_twice_timeline':(2,[0,120,0,60])}
        for aid,(sid,angles) in expected.items():
            response=client.post('/api/actions',json={'action_id':aid}).get_json()
            assert response['ok'],response
            steps=response['action']['plan']['steps']
            assert [s['move']['angle'] for s in steps]==angles
            assert all(s['move']['id']==sid and s['move']['speed']==30 for s in steps)
            data=bytes.fromhex(response['raw'].split(':',1)[1])
            assert len(data)==8+18*len(angles)
            assert [struct.unpack_from('<H',data,8+18*i+6)[0] for i in range(len(angles))]==angles
            assert A.pop_command('esp-bridge')['raw']==response['raw']
        assert client.post('/api/actions',json={'action_id':'home'}).get_json()['raw']=='@MOVE:home,all'
        A.pop_command('esp-bridge')
        assert client.post('/api/actions',json={'action_id':'happy'}).get_json()['raw']=='@MOVE:expression,happy'
        A.pop_command('esp-bridge')
        with patch.object(A.device_controls,'get',return_value={'servo_speed':0}):
            library=client.get('/api/actions').get_json()
            assert library['servo_speed']==0
            r=client.post('/api/actions',json={'action_id':'nod'}).get_json()
            assert r['ok'] and all(s['move']['speed']==0 for s in r['action']['plan']['steps'])
            assert [s['at_ms'] for s in r['action']['plan']['steps']]==[0,200,400,600]
            A.pop_command('esp-bridge')
        # 预览之后再校准，实际点击必须使用新角度，不改变固定舵机编号。
        profile['servos'][0].update(min=10,mid=25,max=50,front=25)
        profile['servos'][1].update(min=20,mid=55,max=100,front=55)
        P.save(profile)
        library=client.get('/api/actions').get_json()
        nod=next(a for a in library['actions'] if a['id']=='nod')
        assert '舵机2' in nod['summary'] and '回正55°' in nod['summary']
        profile['servos'][1]['front']=57;P.save(profile)
        r=client.post('/api/actions',json={'action_id':'nod'}).get_json()
        assert [s['move']['angle'] for s in r['action']['plan']['steps']]==[20,100,20,57]
        assert all(s['move']['id']==2 for s in r['action']['plan']['steps'])
        A.pop_command('esp-bridge')
        r=client.post('/api/actions',json={'action_id':'shake'}).get_json()
        assert [s['move']['angle'] for s in r['action']['plan']['steps']]==[10,50,25]
        assert all(s['move']['id']==1 for s in r['action']['plan']['steps'])
        A.pop_command('esp-bridge')
        profile['servos'][0]['role']='user_defined'
        try:
            P.save(profile)
        except ValueError:
            pass
        else:
            raise AssertionError('S1 yaw 作用必须固定')
        profile['servos'][0]['role']='yaw'
        profile['expressions']=[f for f in profile['expressions'] if f['id']!='happy']
        P.save(profile)
        library=client.get('/api/actions').get_json()
        assert next(a for a in library['actions'] if a['id']=='nod')['ready']
        assert client.post('/api/actions',json={'action_id':'happy'}).status_code==409
        assert client.post('/api/actions',json={'action_id':'missing'}).status_code==404
        assert A._queue_len('esp-bridge')==0
print('PASS interaction action routes: old-file preserved, 6 timed actions, editable calibration, fixed S1/S2 roles, home, expressions, fresh parameters, auth and unavailable-no-enqueue')
