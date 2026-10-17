"""board_pose.py：缓存 F407 经 ESP 回传的舵机姿态快照。

使用：设备上报时更新，3D 页面查询 get；只读数据不会直接驱动执行器。
时序与边界：姿态来自板端插值估计，不是编码器测量；离线/过期情况须由上层显示。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
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
