"""Wi-Fi 相机的人脸居中控制器。

使用方法：识别完成调用 observe；仅 ESP8266 的真实设备轮询调用 take。
这里只保存最新观察，绝不向普通命令 FIFO 连续追加动作。角度来自新鲜的
F407 PWM 上报，限位/速度来自当前管理员档案；没有上报时不猜测当前位置。
暂停、配置变更、旧照片、录音、OTA、手动操作都会使旧观察失效。
"""
import copy
import math
import threading
import time

DEFAULTS = dict(follow_enabled=False, follow_target='largest', follow_person='',
                follow_deadzone=0.12, follow_gain=0.9, follow_step=20,
                follow_speed=30, follow_yaw_sign=1, follow_pitch_sign=1,
                follow_roll_deg=0, follow_with_roll=False)


def validate(settings):
    for key in ('follow_enabled', 'follow_with_roll'):
        if type(settings[key]) is not bool:
            raise ValueError(key + ' 必须为开关')
    if settings['follow_target'] not in ('largest', 'known'):
        raise ValueError('跟随目标必须为 largest 或 known')
    person = settings['follow_person']
    if not isinstance(person, str) or len(person) > 64:
        raise ValueError('指定人员编号无效')
    for key, lo, hi in [('follow_deadzone', .03, .5), ('follow_gain', .1, 1.),
                        ('follow_step', 1, 30), ('follow_speed', 5, 90),
                        ('follow_roll_deg', -180, 180)]:
        value = settings[key]
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not lo <= value <= hi:
            raise ValueError(key + ' 超出允许范围')
        if key in ('follow_step', 'follow_speed') and type(value) is not int:
            raise ValueError(key + ' 必须为整数')
    for key in ('follow_yaw_sign', 'follow_pitch_sign'):
        if type(settings[key]) is not int or settings[key] not in (-1, 1):
            raise ValueError(key + ' 必须为 -1 或 1')


class Controller:
    def __init__(self, clock=time.time):
        self.clock = clock
        self.lock = threading.RLock()
        self.latest = {}
        self.hold_until = {}
        self.sent_at = {}
        self.states = {}

    def stop(self, device, reason='配置改变，等待新照片', hold=0):
        with self.lock:
            self.latest.pop(device, None)
            self.hold_until[device] = max(self.hold_until.get(device, 0), self.clock() + hold)
            self.states[device] = dict(reason=reason, at=self.clock())

    def observe(self, device, job, created, analysis, settings, source):
        if str(job).startswith('live-'):
            return  # Preview frames never drive servos; only individual photos may locate.
        with self.lock:
            self.latest.pop(device, None)
            # 状态一：只有机器人当前拍摄的照片可触发动作，历史重分析和浏览器上传只展示结果。
            if source != 'device' or not settings['follow_enabled'] or not settings['enabled'] or settings['paused']:
                return
            if not 0 <= self.clock() - created <= 5 or created <= self.sent_at.get(device, 0):
                self.states[device] = dict(reason='照片过期，等待下一张', at=self.clock())
                return
            candidates = [f for f in analysis.get('faces', []) if f.get('face_confidence', 0) >= .8]
            if settings['follow_target'] == 'known':
                candidates = [f for f in candidates if f.get('recognized') and
                              (not settings['follow_person'] or f.get('person_id') == settings['follow_person'])]
            if not candidates:
                self.states[device] = dict(reason='未发现符合条件的人脸，保持位置', at=self.clock())
                return
            face = max(candidates, key=lambda f: f['bbox'][2] * f['bbox'][3])
            self.latest[device] = dict(job=job, created=created, face=copy.deepcopy(face),
                                       settings=copy.deepcopy(settings))
            self.states[device] = dict(reason='检测完成，等待设备领取最新偏差', at=self.clock(), job=job)

    def status(self, device):
        with self.lock:
            value = dict(self.states.get(device, dict(reason='跟随默认关闭，等待启用', at=self.clock())))
            value['pending'] = bool(device in self.latest and self.clock() - self.latest[device]['created'] <= 5)
            if self.clock() - value['at'] > 5 and value['reason'].startswith(('检测完成', '已发送')):
                value['reason'] = '等待新照片或设备轮询；旧偏差不会继续执行'
            return value

    def take(self, device, settings, pose, profile, blockers):
        with self.lock:
            observation = self.latest.pop(device, None)
            if not observation:
                return None
            now = self.clock()
            def refuse(reason, params=None):
                state = dict(reason=reason, at=now)
                if params:
                    state['servo_params'] = copy.deepcopy(params)
                self.states[device] = state
                return None
            # 状态二：领取时重新核对互斥和配置，分析期间发生的暂停/校准不会漏过。
            if not settings['follow_enabled'] or not settings['enabled'] or settings['paused'] or settings != observation['settings']:
                return refuse('采集配置已改变，等待新照片')
            if any(blockers.values()) or now < self.hold_until.get(device, 0):
                return refuse('录音 / OTA / 手动动作优先，丢弃旧偏差')
            if not 0 <= now - observation['created'] <= 5:
                return refuse('照片过期，保持位置')
            if not pose.get('fresh') or pose.get('moving') or any(a is None for a in pose.get('angles', [None]*3)):
                return refuse('等待F407新鲜且静止的位置上报')
            if now - pose.get('age_ms', 999999)/1000 <= self.sent_at.get(device, 0):
                return refuse('等待动作之后的位置上报')
            nx, ny = observation['face']['offset_normalized']
            roll = settings['follow_roll_deg']
            servos = {s['id']: s for s in profile['servos']}
            if settings['follow_with_roll']:
                roll += pose['angles'][2] - servos[3]['front']
            r = math.radians(roll)
            # 图像偏差右/下为正；补偿相机安装旋转后换成底座左右和上下方向。
            x, y = nx*math.cos(r)-ny*math.sin(r), nx*math.sin(r)+ny*math.cos(r)
            moves = []
            targets = []
            max_delta = 0
            for sid, offset, fov, sign in [(1, x, settings['horizontal_fov_deg'], settings['follow_yaw_sign']),
                                          (2, -y, settings['vertical_fov_deg'], settings['follow_pitch_sign'])]:
                servo = servos[sid]
                current = pose['angles'][sid-1]
                angle = max(servo['min'], min(servo['max'], int(round(current))))
                # 一张照片就尽量把目标转到位：先用完整几何修正，再受"单张最大转角"约束。
                # 死区只用于停止抖动，不是"每张只挪一两度"的理由。
                if abs(offset) > settings['follow_deadzone']:
                    correction = math.degrees(math.atan(offset*math.tan(math.radians(fov/2)))) * settings['follow_gain'] * sign
                    step = max(-settings['follow_step'], min(settings['follow_step'], correction))
                    angle = max(servo['min'], min(servo['max'], int(round(current + step))))
                speed = min(settings['follow_speed'], servo.get('speed', settings['follow_speed']) or settings['follow_speed'])
                targets.append(dict(id=sid, angle=angle, speed=speed))
                if angle != current:
                    max_delta = max(max_delta, abs(angle - current))
            # 三轴参数完整下发：单张照片只测出水平/垂直偏差，S3保持当前角度。
            # 没有视觉姿态估计时不凭空产生第三个旋转量。
            s3 = servos[3]
            s3_angle = max(s3['min'], min(s3['max'], int(round(pose['angles'][2]))))
            targets.append(dict(id=3, angle=s3_angle,
                                speed=min(settings['follow_speed'], s3.get('speed') or settings['follow_speed'])))
            if max_delta == 0:
                return refuse('人脸已居中或达到管理员限位', targets)
            # 三轴都写进同一次命令，S1/S2 是修正值，S3 是保持值。
            for target in targets:
                moves.append(dict(type='servo', id=target['id'], angle=target['angle'], speed=target['speed']))
            moves.sort(key=lambda m: m['id'])
            # 状态三：一次观察最多发送一次（可能多度）修正；必须有新照片才会再次修正。
            self.sent_at[device] = now
            # 保持时间按实际转角/速度计算，避免大角度尚未到位就被下一张照片覆盖。
            self.hold_until[device] = now + max(1.0, max_delta/float(settings['follow_speed']) + .3)
            self.states[device] = dict(reason='已发送居中小步动作，等待新照片', at=now,
                                       job=observation['job'], moves=copy.deepcopy(moves),
                                       servo_params=copy.deepcopy(targets))
            return dict(moves=moves, servo_params=targets,
                        note='camera_follow_latest', camera_job=observation['job'])
