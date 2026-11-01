"""User-confirmed idle recipes compiled as one bounded, atomic MCU snapshot."""
import copy, json, os, struct, tempfile, threading, time, zlib
import motion_timeline
import device_profile
import short_actions
PATH = os.path.join(os.path.dirname(__file__), 'idle_pet_settings.json')
LOCK = threading.RLock()
ACKS = {}
DEFAULT = {'enabled': False, 'idle_seconds': 30, 'sleep_seconds': 180, 'gap_seconds': 30,
           'awake_actions': ['idle_look', 'idle_blink'], 'sleep_action': 'idle_sleep'}

def _load():
    try:
        with open(PATH, encoding='utf-8') as f:return json.load(f)
    except (OSError, ValueError):return {}

def validate(data):
    if not isinstance(data, dict) or not isinstance(data.get('enabled'), bool):
        raise ValueError('待机动作开关必须为布尔值')
    result = {'enabled': data['enabled']}
    for key in ('idle_seconds','sleep_seconds','gap_seconds'):
        v = data.get(key)
        if isinstance(v,bool) or not isinstance(v,int) or not 5<=v<=3600:
            raise ValueError(key+' 必须为5~3600秒整数')
        result[key] = v
    if result['sleep_seconds'] < result['idle_seconds']:
        raise ValueError('睡眠时间不能短于待机等待时间')
    actions = data.get('awake_actions')
    if not isinstance(actions,list) or len(actions)>2 or len(set(actions))!=len(actions) or any(not isinstance(x,str) for x in actions):
        raise ValueError('普通待机动作最多2个且不能重复')
    sleep = data.get('sleep_action')
    if not isinstance(sleep,str):raise ValueError('睡眠动作需为动作ID，空字符串表示不用')
    result.update(awake_actions=actions[:], sleep_action=sleep)
    if result['enabled'] and not (actions or sleep):raise ValueError('启用时至少选择一个动作')
    return result

def get(device):
    with LOCK:
        try:return validate(_load().get(device,copy.deepcopy(DEFAULT)))
        except (TypeError,ValueError):return copy.deepcopy(DEFAULT)

def compile_settings(settings, profile, speed):
    settings=validate(settings)
    profile=short_actions.for_speed(profile,speed)
    limits=device_profile.motion_snapshot()['revision']
    clips=[]
    if settings['enabled']:
        for kind, aid in [(1,a) for a in settings['awake_actions']]+([(2,settings['sleep_action'])] if settings['sleep_action'] else []):
            action=next((a for a in profile['actions'] if a['id']==aid),None)
            if not action or not action.get('ready'):raise ValueError('动作 '+aid+' 尚未确认启用或不可用')
            recipe=action.get('timeline') or {'steps':[{'at_ms':0,'move':m} for m in action.get('moves',[])]}
            raw, plan=motion_timeline.compile_timeline(recipe,profile,speed)
            wire=bytes.fromhex(raw[len('@TIMELINE:'):].rstrip('#'))
            if wire[1]>12 or wire[2]!=1:raise ValueError('待机动作展开后最多12步、repeat必须为1')
            clips.append(struct.pack('<BBH',kind,0,len(wire))+wire)
    wire=struct.pack('<BBBBIIIII',1,len(clips),int(settings['enabled']),0,0,limits,
                     settings['idle_seconds']*1000,settings['sleep_seconds']*1000,settings['gap_seconds']*1000)+b''.join(clips)
    revision=zlib.crc32(wire) or 1
    wire=wire[:4]+struct.pack('<I',revision)+wire[8:]
    assert len(wire)<=708
    return {'wire':wire.hex(),'revision':revision,'limits_revision':limits,'enabled':settings['enabled']}

def snapshot(device, profile, speed):
    error=''
    try:result=compile_settings(get(device),profile,speed)
    except (ValueError,TypeError) as exc:
        # New limits can invalidate old recipes. Explicitly disable the board,
        # never silently clamp an approved recipe into a different action.
        error=str(exc)
        disabled=get(device);disabled['enabled']=False
        result=compile_settings(disabled,profile,speed)
    with LOCK:ack,at=ACKS.get(device,(0,0))
    result.update(applied=ack==result['revision'] and time.monotonic()-at<15, error=error)
    return result

def confirm(device, revision):
    if isinstance(revision,int) and not isinstance(revision,bool) and 0<revision<=0xffffffff:
        with LOCK:ACKS[device]=(revision,time.monotonic())

def save(device,data,profile,speed):
    value=validate(data);compile_settings(value,profile,speed)
    with LOCK:
        all_settings=_load();all_settings[device]=value
        fd,tmp=tempfile.mkstemp(prefix='idle-',suffix='.json',dir=os.path.dirname(PATH))
        try:
            with os.fdopen(fd,'w',encoding='utf-8') as f:json.dump(all_settings,f,ensure_ascii=False,indent=2)
            os.replace(tmp,PATH)
        finally:
            if os.path.exists(tmp):os.unlink(tmp)
    return value
