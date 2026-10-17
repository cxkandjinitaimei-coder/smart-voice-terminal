"""test_servo_contract.py：使用真实服务模块的离线回归。

使用：由 tools/run_checks.py 在临时服务副本中单独运行；不连接硬件、不调用真实 AI。
范围：断言涵盖本文件列出的状态/边界；测试通过不代表飞线信号、供电和公网时延已验证。
依赖：server/Robot_Server/requirements.txt；数据由测试临时创建并清理。
"""
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import device_profile as P
import motion_timeline as T
import servo_contract as S


class ServoContractTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='servo-contract-')
        self.path = Path(self.tmp.name) / 'profile.json'
        self.patch = patch.object(P, 'PROFILE_PATH', str(self.path))
        self.patch.start()

    def tearDown(self):
        self.patch.stop()
        self.tmp.cleanup()

    def test_old_profile_migrates_once_preserving_libraries_and_speeds(self):
        old = P.default_profile()
        old.pop('servo_contract_revision')
        old['servos'][0].update(role='pitch', min=75, mid=90, max=105, front=90, speed=270)
        old['servos'][1].update(role='yaw', min=0, mid=90, max=180, front=90, speed=180)
        old['actions'].append({'id':'trained_test', 'name':'保留训练动作', 'custom':True,
            'confirmed':True, 'timeline':{'steps':[{'at_ms':0,'move':{'type':'servo','id':2,'angle':135}}]}})
        self.path.write_text(json.dumps(old, ensure_ascii=False), encoding='utf-8')
        upgraded = P.load()
        self.assertEqual([(s['role'],s['min'],s['max'],s['front']) for s in upgraded['servos']],
                         [('yaw',15,165,90),('pitch',0,90,30),('roll',0,180,90)])
        self.assertEqual([s.get('speed') for s in upgraded['servos']], [270,180,None])
        self.assertEqual(upgraded['expressions'], P.validate(old | {'servos':upgraded['servos']})['expressions'])
        custom = next(a for a in upgraded['actions'] if a['id']=='trained_test')
        self.assertEqual(custom['timeline'], old['actions'][-1]['timeline'])
        self.assertFalse(custom['ready'])  # Keep an old out-of-range recipe, block its execution.
        self.assertTrue(custom['unavailable_reason'])
        backup = Path(str(self.path)+'.before_servo_contract.json')
        self.assertEqual(json.loads(backup.read_text(encoding='utf-8')),old)
        saved,stamp = self.path.read_bytes(),self.path.stat().st_mtime_ns
        self.assertEqual(P.load(), upgraded)
        self.assertEqual((self.path.read_bytes(),self.path.stat().st_mtime_ns),(saved,stamp))

    def test_locked_fields_cannot_be_saved_or_axis_removed(self):
        P.save(P.default_profile())
        before = self.path.read_bytes()
        for sid,key,value in [(0,'min',0),(0,'role','pitch'),(1,'front',90),(1,'max',180),(2,'front',0)]:
            bad=P.load();bad['servos'][sid][key]=value
            with self.assertRaises(ValueError):P.save(bad)
            self.assertEqual(self.path.read_bytes(),before)
        bad=P.load();bad['servos'].pop()
        with self.assertRaises(ValueError):P.save(bad)

    def test_home_packet_and_roles_match_physical_axes(self):
        profile=P.load()
        data=bytes.fromhex(P.motion_snapshot()['wire'])
        self.assertEqual([struct.unpack_from('<6B',data,6+i*6) for i in range(3)],
                         [(1,15,165,90,2,0),(2,0,90,30,1,0),(3,0,180,90,0,0)])
        _,home=T.compile_timeline({'steps':[{'at_ms':0,'move':{'type':'home'}}]},profile)
        self.assertEqual([(s['move']['id'],s['move']['angle']) for s in home['steps']],[(1,90),(2,30),(3,90)])
        for name,sid in [('nod',2),('shake',1)]:
            _,plan=T.compile_timeline({'steps':[{'at_ms':0,'move':{'type':'gesture','name':name}}]},profile)
            self.assertEqual({s['move']['id'] for s in plan['steps']},{sid})

    def test_raw_and_timeline_reject_each_axis_outside_its_limits(self):
        profile=P.load()
        for sid,lo,hi in [(1,15,165),(2,0,90),(3,0,180)]:
            for angle in [lo,hi]:T.validate_raw('@MOVE:servo,%d,%d#'%(sid,angle),profile)
            for angle in [lo-1,hi+1]:
                with self.assertRaises(ValueError):T.validate_raw('@MOVE:servo,%d,%d#'%(sid,angle),profile)
                with self.assertRaises(ValueError):
                    T.compile_timeline({'steps':[{'at_ms':0,'move':{'type':'servo','id':sid,'angle':angle}}]},profile)

    def test_no_change_to_already_current_profile(self):
        profile=P.default_profile()
        profile['servos'][0]['name']='我的底座'
        profile['servos'][1]['speed']=48
        self.assertEqual(S.migrate(profile),profile)
        self.assertIsNot(S.migrate(profile),profile)


if __name__=='__main__':unittest.main(verbosity=2)
