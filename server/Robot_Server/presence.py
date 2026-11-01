"""上位机待机存在感：1 分钟没声音左右观望，2 分钟没声音休眠，听到声音回正。

只在 PC/Flask 侧运行，不依赖 F407 的待机固件；命令仍走原网页队列，由 ESP
转发给 F407。任何用户声音、AI 回复、手动/动作库命令都会重置计时。
"""
import threading
import time


class Tracker:
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.lock = threading.RLock()
        self.state = {}

    def note(self, device, sound=False):
        """记录一次“有人”的活动；sound=True 时返回是否把机器从 look/sleep 唤醒。"""
        now = self.clock()
        with self.lock:
            item = self.state.setdefault(device, dict(last=now, stage=0))
            woke = sound and item['stage'] > 0
            item['last'] = now
            item['stage'] = 0
        return woke

    def tick(self, device, idle_seconds, sleep_seconds, busy):
        """返回 look/sleep/None；busy 时不在别人动作上叠加待机动作。"""
        now = self.clock()
        with self.lock:
            item = self.state.setdefault(device, dict(last=now, stage=0))
            if busy:
                # 醒着时遇到录音/OTA/动作，说明现场有人；重置等待。
                # look/sleep 阶段不重置，否则自己的 6 秒观望会把“再过一分钟休眠”顶后。
                if item['stage'] == 0:
                    item['last'] = now
                return None
            idle = now - item['last']
            if item['stage'] == 0 and idle >= idle_seconds:
                item['stage'] = 1
                return 'look'
            if item['stage'] == 1 and idle >= sleep_seconds:
                item['stage'] = 2
                return 'sleep'
            return None

    def status(self, device):
        with self.lock:
            return dict(self.state.get(device, dict(last=0, stage=0)))


TRACKER = Tracker()
