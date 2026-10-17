"""servo_contract.py：读取唯一舵机物理默认契约。

使用：从本模块同目录 static/servo-contract.json 加载，供档案迁移、动作编译和网页一致使用。
时序与边界：S1 15~165/90，S2 0~90/30，S3 0~180/90；全部为绝对角，外层目录改名不改变定位。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
import copy
import json
from pathlib import Path

CONTRACT = json.loads((Path(__file__).parent / 'static/servo-contract.json').read_text(encoding='utf-8'))
REVISION = CONTRACT['revision']
SERVOS = {s['id']: s for s in CONTRACT['servos']}
LOCKED = ('role', 'min', 'mid', 'max', 'front')


def migrate(profile):
    result = copy.deepcopy(profile)
    if result.get('servo_contract_revision') == REVISION:
        return result
    items = {s['id']: s for s in result.get('servos', [])}
    for sid, fixed in SERVOS.items():
        # Preserve speeds and all non-servo libraries. Old identity/angles are obsolete.
        items.setdefault(sid, {}).update(copy.deepcopy(fixed))
    result['servos'] = [items[sid] for sid in sorted(items)]
    result['servo_contract_revision'] = REVISION
    return result


def check(servos):
    indexed = {s['id']: s for s in servos}
    for sid, fixed in SERVOS.items():
        if sid not in indexed or any(indexed[sid].get(k) != fixed[k] for k in LOCKED):
            raise ValueError('舵机%d编号/作用/限位已按用户要求固定：%s %d~%d°，默认%d°' %
                             (sid, fixed['name'], fixed['min'], fixed['max'], fixed['front']))
