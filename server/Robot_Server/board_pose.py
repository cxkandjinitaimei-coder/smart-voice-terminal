"""Timestamped last ACKed PWM estimates. No simulated/default shaft positions."""
import threading, time, copy
LOCK=threading.Lock()
POSES={}
def ingest(device, data):
    if not isinstance(data,dict):return False
    values={}
    for key,hi in [('uptime_ms',0xffffffff),('mask',7),('age_ms',1500),('idle',4),('idle_revision',0xffffffff)]:
        value=data.get(key)
        if isinstance(value,bool) or not isinstance(value,int) or not 0<=value<=hi:return False
        values[key]=value
    angles=data.get('angles')
    if not isinstance(angles,list) or len(angles)!=3 or any(isinstance(a,bool) or not isinstance(a,int) or not 0<=a<=180 for a in angles):return False
    if not isinstance(data.get('moving'),bool):return False
    values.update(angles=angles[:],moving=data['moving'],source='f407_pwm_ack')
    with LOCK:POSES[device]=(values,time.monotonic()-values['age_ms']/1000)
    return True
def get(device):
    with LOCK:
        if device not in POSES:return {'fresh':False,'angles':[None]*3,'message':'尚未收到F407位置上报'}
        values,seen=POSES[device];result=copy.deepcopy(values)
    age=max(0,int((time.monotonic()-seen)*1000));result['age_ms']=age
    result['fresh']=age<=2500
    result['angles']=[a if values['mask']&(1<<i) else None for i,a in enumerate(values['angles'])]
    result['message']='板端PWM估计（无物理反馈）' if result['fresh'] else '上报过期，保留最后位置（录音/OTA时暂停上报）'
    return result
