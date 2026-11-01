"""记录每次时间轴从入队到板端回执的状态，供录制页查询。

register/dispatched/report分别由队列、设备领取、真实ESP上报调用；浏览器
只读查询。最多保留256条，服务重启后返回unknown，不伪造完成或自动重放。
"""
from collections import OrderedDict
from threading import RLock
import time
import uuid

LOCK = RLock()
RECORDS = OrderedDict()


def register(device, cmd):
    raw = cmd.get('raw', '')
    if not raw.startswith('@TIMELINE:') or raw.startswith('@TIMELINE:STOP'):
        return
    with LOCK:
        ident = uuid.uuid4().hex
        cmd['motion_id'] = ident
        RECORDS[ident] = dict(id=ident, device_id=device, phase='queued', attempts=0,
                              sequence=0, board_state='', queued_at=time.time(), dispatched_at=0)
        while len(RECORDS)>256:
            RECORDS.popitem(last=False)


def dispatched(cmd):
    with LOCK:
        item = RECORDS.get(cmd.get('motion_id'))
        if item:
            item.update(phase='dispatched', dispatched_at=time.time())


def report(device, value):
    if not isinstance(value, dict):
        return
    with LOCK:
        item = RECORDS.get(value.get('id'))
        if not item or item['device_id'] != device or not item['dispatched_at']:
            return
        if item['phase'] in ('done', 'invalid', 'limits_error', 'cancelled', 'stopped'):
            return
        if value.get('phase') not in ('waiting_ack', 'running', 'paused', 'done', 'invalid',
                                     'limits_error', 'timeout', 'cancelled', 'stopped', 'stopping'):
            return
        for name, maximum in (('sequence',65535),('attempts',8)):
            v=value.get(name)
            if isinstance(v,bool) or not isinstance(v,int) or not 0<=v<=maximum:
                return
        item.update(phase=value['phase'], sequence=value['sequence'], attempts=value['attempts'],
                    board_state=str(value.get('board_state',''))[:100], reported_at=time.time())


def view(ident):
    with LOCK:
        item = dict(RECORDS.get(ident) or {'id': ident, 'phase': 'unknown'})
    if item['phase']=='dispatched' and time.time()-item['dispatched_at']>15:
        item['phase']='no_gateway_report'
    return item
