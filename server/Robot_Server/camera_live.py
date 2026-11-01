"""S3 连续图像接入：由 camera_workspace 创建，复用其锁、配置及人脸引擎。

设备先领取短期会话，再顺序 POST JPEG；一帧确认后才发送下一帧。
每设备只保留最新预览、一帧待分析及最近分析结果，不写照片/任务数据库。
分析慢时覆盖尚未处理的旧帧；停止、改配置、超时后不能再下发旧结果。
浏览器通过已登录的 live/status 与 live/image 查看，设备 Bearer 仅准许上传。
"""
import secrets
import threading
import time


class LiveCamera:
    def __init__(self, workspace):
        self.ws = workspace
        self.sessions = {}

    def close(self, device):
        # 工作线程持有旧对象也无妨：发布前必须确认它仍是当前会话。
        with self.ws.lock:
            self.sessions.pop(device, None)

    def policy(self, device, settings):
        if settings['capture_mode'] != 'continuous' or settings['camera_transport'] != 's3-wifi' or settings['source'] != 'device':
            return '连续模式未启用'
        return self.ws.gate(device, settings)

    def grant(self, device, settings):
        with self.ws.lock:
            reason = self.policy(device, settings)
            if reason:
                self.close(device)
                return dict(accepted=False, reason=reason)
            now = self.ws.clock()
            # 限制设备数量，避免任意设备名创建无界内存与工作线程。
            for key, old in list(self.sessions.items()):
                if now > old['expires']:
                    self.sessions.pop(key)
            state = self.sessions.get(device)
            if state is None:
                if len(self.sessions) >= 4:
                    return dict(accepted=False, reason='连续相机数量已达上限（4）')
                state = dict(token=secrets.token_urlsafe(24), sequence=0, received=0, analyzed=0,
                             dropped=0, frame=None, pending=None, working=False, result=None,
                             error='', analysis_sequence=0, analysis_at=0, at=0, next_at=0, result_frame=None)
                self.sessions[device] = state
            state['expires'] = now + 15
            return dict(accepted=True, mode='continuous', ticket=state['token'],
                        frame_interval_ms=round(1000/settings['continuous_fps']), expires_in=15)

    def check(self, device, token, settings):
        from camera_workspace import CameraError
        state = self.sessions.get(device)
        if not state or not secrets.compare_digest(state['token'], token) or self.ws.clock() > state['expires']:
            raise CameraError('连续会话已过期，请重新轮询', 409)
        reason = self.policy(device, settings)
        if reason:
            self.close(device)
            raise CameraError(reason, 409)
        return state

    def receive(self, device, raw, token, sequence, captured_at=None):
        from camera_workspace import CameraError, normalize_image
        # 在解码前、解码后都检查租约；停止操作可以使正在接收的帧失效。
        with self.ws.lock:
            settings = self.ws.current_settings(device)
            state = self.check(device, token, settings)
            if type(sequence) is not int or not 1 <= sequence <= 2147483647:
                raise CameraError('帧序号无效')
            if sequence <= state['sequence']:
                raise CameraError('旧帧或重复帧，请发送下一帧', 409)
            if self.ws.clock() < state['next_at']:
                raise CameraError('超过设定帧率，请减速', 429)
            captured_at = self.ws.clock() if captured_at is None else captured_at
        data, width, height = normalize_image(raw)
        if width > 640 or height > 640:
            raise CameraError('连续帧最多 VGA，请降低相机分辨率')
        with self.ws.lock:
            settings = self.ws.current_settings(device)
            if self.check(device, token, settings) is not state:
                raise CameraError('连续会话已更换', 409)
            if sequence <= state['sequence']:
                raise CameraError('帧序号已使用', 409)
            if state['pending'] is not None:
                state['dropped'] += 1
            state.update(sequence=sequence, received=state['received']+1, frame=data,
                         at=self.ws.clock(), pending=(sequence, captured_at, data, width, height, dict(settings)),
                         next_at=self.ws.clock()+1/settings['continuous_fps'], expires=self.ws.clock()+15)
            self.ws.seen[device] = self.ws.clock()
            if not state['working']:
                state['working'] = True
                threading.Thread(target=self.analyze, args=(device, state), daemon=True,
                                 name='camera-live-analysis').start()
            return dict(ok=True, active=True, sequence=sequence,
                        frame_interval_ms=round(1000/settings['continuous_fps']))

    def analyze(self, device, state):
        from local_faces import add_targeting
        try:
            while True:
                # 与单拍共用两个分析名额；最多4个会话各等一个，不建立无限任务队列。
                with self.ws.slots:
                    with self.ws.lock:
                        if self.sessions.get(device) is not state or state['pending'] is None:
                            return
                        frame = state['pending']; state['pending'] = None
                    sequence, created, raw, width, height, settings = frame
                    start = time.monotonic()
                    result, error = None, ''
                    try:
                        result = add_targeting(self.ws.faces.analyze(raw), (width, height), settings)
                    except Exception as exc:
                        error = '连续识别失败：' + str(exc)[:160]
                    with self.ws.lock:
                        if self.sessions.get(device) is not state:
                            return
                        current = self.ws.current_settings(device)
                        if self.policy(device, current) or current != settings or self.ws.clock() > state['expires']:
                            return
                        state.update(result=result, result_frame=raw, error=error, analysis_sequence=sequence,
                                     analysis_at=self.ws.clock(), elapsed_ms=round((time.monotonic()-start)*1000),
                                     analyzed=state['analyzed']+1)
                        # 回调沿用现有最新观测跟随器；5秒过期、姿态反馈、管理员限位仍生效。
                        if result is not None and self.ws.on_analysis and self.ws.clock()-created <= 5:
                            self.ws.on_analysis(device, 'live-'+str(sequence), created, result, settings, 'device')
                        if state['pending'] is None:
                            return
        finally:
            with self.ws.lock:
                state['working'] = False
                # 新帧可能恰好在退出前进入；在同一把锁里接棒，不能漏唤醒或启动双工人。
                if (self.sessions.get(device) is state and state['pending'] is not None
                        and self.ws.clock() <= state['expires']
                        and not self.policy(device, self.ws.current_settings(device))):
                    state['working'] = True
                    threading.Thread(target=self.analyze, args=(device, state), daemon=True,
                                     name='camera-live-analysis').start()

    def status(self, device):
        with self.ws.lock:
            state = self.sessions.get(device)
            now = self.ws.clock()
            if not state:
                return dict(active=False, received=0, analyzed=0, dropped=0)
            return dict(active=now <= state['expires'], sequence=state['sequence'], at=state['at'],
                        age_sec=round(now-state['at'], 2) if state['at'] else None,
                        received=state['received'], analyzed=state['analyzed'], dropped=state['dropped'],
                        analysis_sequence=state['analysis_sequence'], analysis_at=state['analysis_at'],
                        elapsed_ms=state.get('elapsed_ms', 0), analysis=state['result'], error=state['error'])

    def image(self, device, analyzed=False):
        with self.ws.lock:
            state = self.sessions.get(device)
            if not state or not state['frame'] or self.ws.clock()-state['at'] > 5:
                return None
            return state['result_frame'] if analyzed else state['frame']
