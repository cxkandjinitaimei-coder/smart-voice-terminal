"""Persona migration keeps owner settings and factual context without old tone."""
import copy
import unittest
import ai_relay as A
import device_profile as P


class CompanionPersonaTests(unittest.TestCase):
    def test_migrate_and_keep_later_user_changes(self):
        old={'persona':'旧人设','token':'fixture','providers':{'test':{'keys':['fixture']}},'rules':['自定义规则']}
        snapshot=copy.deepcopy(old)
        new=A.companion_config(old)
        self.assertEqual(old,snapshot)
        self.assertEqual(new['persona_before_20261016'],'旧人设')
        self.assertEqual(new['providers'],old['providers'])
        self.assertEqual(new['rules'],old['rules'])
        new['persona']='用户后续自定性格'
        self.assertIs(A.companion_config(new),new)
        self.assertIn('用户后续自定性格', A._build_system_prompt(new))

    def test_context_without_assistant_imitation_and_hardware_constraints(self):
        cfg=P.apply_to_config(A.companion_config({}),90)
        messages=A.build_messages(cfg,[],[{'user':'我叫小明','message':'主人喵喵喵'}],'','你记得我吗')
        self.assertFalse(any(x['role']=='assistant' for x in messages))
        self.assertIn('我叫小明',messages[1]['content'])
        self.assertIn('你记得我吗',messages[-1]['content'])
        prompt=messages[0]['content']
        for value in ['JSON 对象','15~165','0~90','0~180','moves=[]','不模仿旧回复','字库']:
            self.assertIn(value,prompt)


if __name__=='__main__':unittest.main()
