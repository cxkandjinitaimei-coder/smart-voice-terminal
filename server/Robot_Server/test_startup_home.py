"""Compile real ESP offline cache/replay and exercise real cloud home semantics."""
import copy
import json
from pathlib import Path
import re
import subprocess
import tempfile
from unittest.mock import patch
import zlib
import app as A
import ai_relay
import device_profile as P
import motion_timeline as T

ROOT=next(p for p in Path(__file__).resolve().parents if (p / 'AGENTS.md').is_file())
ESP=ROOT/'sketch_aug28a'
TEST=ROOT/'ST32/AIAssistant/Tools/timeline_test'

def function(source, name):
    start=source.index('void '+name+'() {')
    at=source.index('{',start);depth=1;end=at+1
    while depth:
        if source[end]=='{':depth+=1
        if source[end]=='}':depth-=1
        end+=1
    return source[start:end]

source=(ESP/'sketch_aug28a.ino').read_text(encoding='utf-8-sig')
main=r'''
int main(){
    auto first=profile(),second=profile(35);uint32_t rev=0;
    assert(motionProfileValid(first.c_str(),rev) && rev);
    MockFS fs;SavedMotionProfile<MockFS> c;c.begin(fs);assert(!c.revision() && !c.cached());
    assert(c.accept(first.c_str()) && c.cached() && fs.writes==1);
    assert(c.accept(first.c_str()) && fs.writes==1);
    SavedMotionProfile<MockFS> reboot;reboot.begin(fs);assert(reboot.cached() && first==reboot.wire());
    auto bad=first;bad[8]=bad[8]=='0'?'1':'0';assert(!c.accept(bad.c_str()) && first==c.wire());
    fs.shortWrite=true;assert(c.accept(second.c_str()) && !c.cached());
    reboot.begin(fs);assert(reboot.cached() && first==reboot.wire());
    fs.shortWrite=false;fs.renameOK=false;assert(c.accept(second.c_str()) && !c.cached());
    reboot.begin(fs);assert(reboot.cached() && first==reboot.wire());
    fs.renameOK=true;assert(c.accept(second.c_str()) && c.cached());reboot.begin(fs);assert(second==reboot.wire());
    fs.files["/servo-profile.hex"]=bad;reboot.begin(fs);assert(!reboot.revision() && !reboot.cached());
    SavedMotionProfile<MockFS> live;assert(live.accept(first.c_str()) && !live.cached());
    filesystem.files["/servo-profile.hex"]=first;
    motionProfileCache.begin(filesystem);replaySavedMotionProfile();assert(Serial.data.size()==32 && Serial.data[2]==0x22);
    uint32_t r=motionProfileCache.revision();assert(motionSentRevision==r);
    auto count=Serial.data.size();tick=1999;replaySavedMotionProfile();assert(Serial.data.size()==count);
    tick=2000;replaySavedMotionProfile();assert(Serial.data.size()==count*2);
    motionAck=r;tick=8000;replaySavedMotionProfile();assert(Serial.data.size()==count*2);
    // MCU-only reset clears acknowledgements; the offline cache is enough.
    motionAck=motionSentRevision=0;motionReplayRequested=true;audio=true;
    replaySavedMotionProfile();assert(Serial.data.size()==count*2);
    audio=false;iap=true;replaySavedMotionProfile();assert(Serial.data.size()==count*2);
    iap=false;mcuUpdating=true;replaySavedMotionProfile();assert(Serial.data.size()==count*2);
    mcuUpdating=false;replaySavedMotionProfile();assert(Serial.data.size()==count*3);
    Serial.data.clear();uartSelfTestToF407();assert(std::string(Serial.data.begin(),Serial.data.end())=="@PING\n");
    puts("PASS actual ESP saved profile: CRC, repeat no Flash write, power loss, rename failure, corrupted cache, live fallback, offline boot/reboot/retry/ACK, audio/OTA mutex, ping without motors");
}
'''
with tempfile.TemporaryDirectory(prefix='home-host-') as temp:
    p=Path(temp)/'home.cpp'
    p.write_text((TEST/'esp_home_test.cpp').read_text()+ '\n'+function(source,'replaySavedMotionProfile')+
                 '\n'+function(source,'uartSelfTestToF407')+'\n'+main,encoding='utf-8')
    exe=Path(temp)/'home.exe'
    subprocess.run(['g++','-Wall','-Wextra','-Werror','-I',str(ESP),str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)

with tempfile.TemporaryDirectory(prefix='home-web-') as temp:
    with patch.object(P,'PROFILE_PATH',str(Path(temp)/'profile.json')):
        profile=P.default_profile()
        for s,front in zip(profile['servos'],[30,60,90]):
            s.update(min=0,mid=front,front=front)
        P.save(profile)
        assert A.decision_to_raw({'moves':[{'type':'home'}]})=='@MOVE:home,all'
        assert A.decision_to_raw({'moves':[{'type':'home','id':2,'speed':30}]})=='@MOVE:home,2,30'
        assert A.decision_to_raw({'moves':[{'type':'home','id':4}]})==''
        assert A.decision_to_raw({'moves':[{'type':'servo','id':1}]})==''
        assert A.decision_to_raw({'moves':[{'type':'servo','angle':30}]})==''
        assert A.decision_to_raw({'moves':[{'type':'servo','id':1,'angle':0}]})=='@MOVE:servo,1,0'
        _,normalized=T.compile_timeline({'steps':[{'at_ms':0,'move':{'type':'home'}}]},profile)
        assert [s['move']['angle'] for s in normalized['steps']]==[30,60,90]
        profile['servos'][0]['front']=35;P.save(profile)
        _,normalized=T.compile_timeline({'steps':[{'at_ms':0,'move':{'type':'home'}}]},P.load())
        assert normalized['steps'][0]['move']['angle']==35
        data=bytes.fromhex(P.motion_snapshot()['wire'])
        assert int.from_bytes(data[2:6],'little')==(zlib.crc32(data[6:]) or 1)
        prompt=ai_relay._build_system_prompt(P.apply_to_config({}))
        assert '不能默认90度' in prompt and '正面角度 35' in prompt and 'mid只是几何参考' in prompt
print('PASS real cloud: semantic home, live front update, timed home, missing-angle no90, explicit0 retained, CRC/prompt parity')
