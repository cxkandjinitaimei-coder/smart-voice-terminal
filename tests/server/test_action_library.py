"""test_action_library.py：真实动作路由与当前三轴契约的离线回归。

使用：tools/run_checks.py；每次使用临时档案，网络/设备操作全部由本地请求和替身隔离。
覆盖：鉴权、时间线编译、当前速度、动作禁用、旧档案迁移以及失败不入队。
"""
import copy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import app as A
import device_profile as P


class ActionLibraryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        for obj, name, value in [(P,'PROFILE_PATH',str(Path(temporary.name)/'profile.json')),
            (A,'start_auto_tick',lambda:None),(A,'start_store',lambda:None),
            (A,'start_beacon',lambda:None),(A.asr,'warmup',lambda:None),
            (A,'append_line',lambda *args,**kw:None),
            (A.store,'record_command',lambda *args,**kw:None),
            (A,'load_config',lambda:{'token':'actions-fixture'}),
            (A.device_controls,'get',lambda device:dict(A.device_controls.DEFAULT,servo_speed=30))]:
            handle=patch.object(obj,name,value);handle.start();self.addCleanup(handle.stop)
        P.save(P.default_profile());A._pending_commands.clear()
        self.client=A.app.test_client()
        with self.client.session_transaction() as session: session['authed']=True

    def test_auth_and_fixed_contract(self):
        self.assertEqual(A.app.test_client().post('/api/actions',json={'action_id':'nod'}).status_code,401)
        p=P.load();p['servos'][1]['front']=90
        with self.assertRaises(ValueError):P.save(p)
        self.assertEqual(P.load()['servos'][1]['front'],30)

    def test_timed_nod_shake_use_current_roles_limits_and_speed(self):
        for name,sid,expected in [('nod',2,[0,90,0,30]),('shake',1,[15,165,90])]:
            response=self.client.post('/api/actions',json={'action_id':name})
            self.assertEqual(response.status_code,200,response.json)
            steps=response.json['action']['plan']['steps']
            self.assertEqual([s['move']['angle'] for s in steps],expected)
            self.assertTrue(all(s['move']['id']==sid and s['move']['speed']==30 for s in steps))
            self.assertEqual(A.pop_command('esp-bridge')['raw'],response.json['raw'])

    def test_current_speed_after_preview_and_expression_availability(self):
        self.client.get('/api/actions')
        p=P.load();p['servos'][1]['speed']=180;P.save(p)
        response=self.client.post('/api/actions',json={'action_id':'nod'})
        self.assertTrue(all(s['move']['speed']==180 for s in response.json['action']['plan']['steps']))
        A.pop_command('esp-bridge')
        p=P.load();p['expressions']=[f for f in p['expressions'] if f['id']!='happy'];P.save(p)
        self.assertEqual(self.client.post('/api/actions',json={'action_id':'happy'}).status_code,409)
        self.assertEqual(self.client.post('/api/actions',json={'action_id':'missing'}).status_code,404)
        self.assertEqual(A._queue_len('esp-bridge'),0)


if __name__=='__main__':unittest.main(verbosity=2)
