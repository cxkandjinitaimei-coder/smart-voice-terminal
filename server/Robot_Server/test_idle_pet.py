"""Real profile/compiler/authenticated Flask idle and board telemetry routes."""
import copy,json,struct,tempfile,time,unittest
from pathlib import Path
from unittest.mock import patch
import app as A
import device_profile as P
import idle_pet as I
import board_pose as B
class IdleTest(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
  root=Path(self.tmp.name)
  self.stack=[]
  for obj,key,value in [(P,'PROFILE_PATH',str(root/'profile.json')),(I,'PATH',str(root/'idle.json')),
    (A,'load_config',lambda:{'token':'pet-test','modes':{'default':'idle'},'auto_tick':{'enabled':False}}),
    (A,'start_auto_tick',lambda:None),(A,'start_store',lambda:None),(A,'start_beacon',lambda:None),
    (A.asr,'warmup',lambda:None),(A,'append_line',lambda *a,**k:None),
    (A.device_controls,'get',lambda d:{'servo_speed':60,'revision':1}),
    (A.store,'record_command',lambda *a,**k:None)]:
   p=patch.object(obj,key,value);p.start();self.addCleanup(p.stop)
  P.save(P.default_profile());I.ACKS.clear();B.POSES.clear();A._pending_commands.clear()
  self.client=A.app.test_client()
  with self.client.session_transaction() as s:s['authed']=True
 def confirm(self,aid):
  p=P.load();a=next(a for a in p['actions'] if a['id']==aid);a['confirmed']=True
  P.save(p)
 def test_confirmation_and_auth(self):
  c=A.app.test_client();self.assertEqual(c.post('/api/admin/idle-pet',json={}).status_code,401)
  s=copy.deepcopy(I.DEFAULT);s['enabled']=True
  r=self.client.post('/api/admin/idle-pet',json={'settings':s});self.assertEqual(r.status_code,400)
  self.assertEqual(self.client.post('/api/actions',json={'action_id':'idle_look'}).status_code,409)
  for aid in ['idle_blink','idle_look','idle_sleep']:self.confirm(aid)
  r=self.client.post('/api/admin/idle-pet',json={'settings':s});self.assertTrue(r.json['ok'],r.json)
  sync=r.json['sync'];wire=bytes.fromhex(sync['wire']);self.assertLessEqual(len(wire),708)
  self.assertEqual(struct.unpack_from('<BBBB',wire),(1,3,1,0));self.assertFalse(sync['applied'])
  I.confirm('esp-bridge',sync['revision']);self.assertTrue(I.snapshot('esp-bridge',P.load(),60)['applied'])
  run=self.client.post('/api/actions',json={'action_id':'idle_look'});self.assertTrue(run.json['ok'])
 def test_custom_ai_context_and_limit_change_preserves_profile(self):
  p=P.load();p['servos'][0].update(min=80,mid=90,max=100,front=93)
  custom={'id':'my_head','name':'自定义偏头','meaning':'好奇地偏头','custom':True,'confirmed':True,
   'timeline':{'steps':[{'at_ms':0,'move':{'type':'servo','id':1,'angle':95,'speed':45}},
                        {'at_ms':1000,'move':{'type':'expression','face':'think'}}]}}
  p['actions'].append(custom);saved=P.save(p);self.assertTrue(next(a for a in saved['actions'] if a['id']=='my_head')['ready'])
  cfg=P.apply_to_config({},60);self.assertIn('my_head',json.dumps(cfg))
  settings=dict(I.DEFAULT,enabled=True,awake_actions=['my_head'],sleep_action='')
  I.save('esp-bridge',settings,saved,60)
  before=I.snapshot('esp-bridge',saved,60)['revision']
  p=P.load();p['servos'][0].update(max=94);P.save(p)
  source=Path(P.PROFILE_PATH).read_bytes();after=P.load()
  self.assertEqual(after['servos'][0]['front'],93);self.assertEqual(source,Path(P.PROFILE_PATH).read_bytes())
  snap=I.snapshot('esp-bridge',after,60);self.assertFalse(snap['enabled']);self.assertTrue(snap['error']);self.assertNotEqual(before,snap['revision'])
 def test_independent_editor_saves_preserve_latest_parameters_and_actions(self):
  stale=P.load()
  calibrated=P.load();calibrated['servos'][0]['front']=92;P.save_parameters(calibrated)
  recipe={'id':'my_recipe','custom':True,'confirmed':True,'name':'my recipe',
          'timeline':{'steps':[{'at_ms':0,'move':{'type':'expression','face':'think'}}]}}
  P.save_action(recipe)
  self.assertEqual(P.load()['servos'][0]['front'],92)
  stale['servos'][1]['speed']=180;P.save_parameters(stale)
  current=P.load();self.assertTrue(any(a['id']=='my_recipe' for a in current['actions']))
  self.assertEqual(current['servos'][1]['speed'],180)
 def test_malformed_reject_and_packet_boundary(self):
  settings=copy.deepcopy(I.DEFAULT)
  for bad in [dict(settings,idle_seconds=True),dict(settings,sleep_seconds=2),dict(settings,gap_seconds=0),
               dict(settings,enabled='yes'),dict(settings,awake_actions=['idle_look','idle_look'])]:
   with self.assertRaises(ValueError):I.validate(bad)
  p=P.load()
  for i in range(3):
   p['actions'].append({'id':'max'+str(i),'custom':True,'confirmed':True,
    'timeline':{'duration_ms':12000,'steps':[{'at_ms':j*1000,'move':{'type':'expression','face':'happy'}} for j in range(12)]}})
  p=P.save(p);s=dict(settings,enabled=True,awake_actions=['max0','max1'],sleep_action='max2')
  packet=bytes.fromhex(I.compile_settings(s,p,60)['wire']);self.assertEqual(len(packet),708)
  packet_revision=struct.unpack_from('<I',packet,4)[0];self.assertGreater(packet_revision,0)
  too_many=copy.deepcopy(p);next(a for a in too_many['actions'] if a['id']=='max0')['timeline']['steps'].append({'at_ms':11500,'move':{'type':'expression','face':'think'}})
  with self.assertRaises(ValueError):I.compile_settings(s,too_many,60)
 def test_pose_validity_staleness_mask_and_web_cannot_spoof(self):
  data={'uptime_ms':100,'mask':5,'age_ms':100,'angles':[30,90,150],'moving':True,'idle':1,'idle_revision':23}
  self.assertTrue(B.ingest('esp-bridge',data));r=self.client.get('/api/device/pose').json['pose']
  self.assertEqual(r['angles'],[30,None,150]);self.assertTrue(r['fresh'])
  for bad in [dict(data,age_ms=1501),dict(data,mask=8),dict(data,angles=[False,90,90])]:self.assertFalse(B.ingest('esp-bridge',bad))
  self.client.post('/api/data',json={'no_pop':True,'board_pose':dict(data,angles=[100,100,100])})
  self.assertEqual(B.get('esp-bridge')['angles'],[30,None,150])
  value,seen=B.POSES['esp-bridge'];B.POSES['esp-bridge']=(value,seen-3)
  r=B.get('esp-bridge');self.assertFalse(r['fresh']);self.assertEqual(r['angles'],[30,None,150])
if __name__=='__main__':unittest.main()
