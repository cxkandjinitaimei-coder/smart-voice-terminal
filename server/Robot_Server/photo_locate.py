"""One request, one fresh photo, at most one angle command. No tracking loop."""
import copy
from collections import OrderedDict
import threading
import time
import camera_follow


class Locator:
    def __init__(self, clock=time.time):
        self.clock=clock
        self.lock=threading.RLock()
        self.controller=camera_follow.Controller(clock)
        self.requests={}
        # Remember accepted jobs, including cancelled/expired/completed requests.
        # Repeated analysis must never turn a terminal request into a new move.
        self.recent_jobs={}

    def pending(self, device):
        return self.status(device)['phase'] in ('waiting_photo','waiting_pose')

    def start(self, device, job):
        with self.lock:
            recent=self.recent_jobs.setdefault(device,OrderedDict())
            if job in recent:
                return False
            recent[job]=None
            while len(recent)>256:
                recent.popitem(last=False)
            self.controller.stop(device,hold=0)
            self.requests[device]=dict(job=job,at=self.clock(),phase='waiting_photo',
                                       reason='拍照请求已排队，等待相机上传本次照片')
            return True

    def cancel(self, device, reason='单次定位已取消'):
        with self.lock:
            self.controller.stop(device,reason)
            old=self.requests.get(device,{})
            self.requests[device]=dict(old,phase='cancelled',reason=reason)

    def status(self, device):
        with self.lock:
            item=self.requests.get(device)
            if not item:
                return dict(phase='idle',reason='点击“拍照定位并下发角度”执行一次定位')
            expired=(item['phase']=='waiting_photo' and self.clock()-item['at']>60 or
                     item['phase']=='waiting_pose' and self.clock()-item['captured']>5)
            if expired:
                item.update(phase='expired',reason='本次照片或等待已超时，请重新拍照定位')
                self.controller.stop(device,item['reason'])
            return copy.deepcopy(item)

    def observe(self, device, job, captured, analysis, settings, source):
        with self.lock:
            item=self.requests.get(device)
            if not item or item['phase']!='waiting_photo' or item['job']!=job:
                return
            if source!='device':
                item.update(phase='failed',reason='本次照片采集或分析失败，请重新定位')
                return
            if not 0<=self.clock()-captured<=5:
                item.update(phase='expired',reason='照片已过期，请重新拍照定位')
                return
            virtual=dict(settings,enabled=True,follow_enabled=True,follow_gain=1.0,follow_step=180)
            self.controller.observe(device,job,captured,analysis,virtual,source)
            state=self.controller.status(device)
            if not state['pending']:
                item.update(phase='no_face',reason=state['reason'])
                return
            item.update(phase='waiting_pose',captured=captured,settings=copy.deepcopy(settings),
                        reason='人脸定位完成，等待执行板新鲜角度上报')

    def take(self, device, settings, pose, profile, blockers):
        with self.lock:
            item=self.requests.get(device)
            if self.status(device)['phase']!='waiting_pose':
                return None
            if settings!=item['settings']:
                self.cancel(device,'相机设置已改变，请重新拍照定位')
                return None
            if (any(blockers.values()) or not pose.get('fresh') or pose.get('moving') or
                    any(a is None for a in pose.get('angles',[None]*3))):
                item['reason']='等待录音、OTA、动作结束及 F407 新鲜静止角度；超时需重拍'
                return None
            virtual=dict(settings,enabled=True,follow_enabled=True,follow_gain=1.0,follow_step=180)
            cmd=self.controller.take(device,virtual,pose,profile,blockers)
            state=self.controller.status(device)
            item['reason']=state['reason']
            if cmd:
                item.update(phase='sent_to_gateway',reason='目标角度已交给 ESP8266，定位结束并保持位置',
                            servo_params=copy.deepcopy(cmd['servo_params']))
                cmd['note']='photo_locate_once'
            else:
                item['phase']='done'
            return cmd
