"""Factory defaults and fixed axis identities; administrator calibration overrides angles.

The JSON remains the factory source of truth. Existing revisioned profiles retain
their explicit calibration; only pre-contract legacy profiles migrate once.
"""
import copy
import json
from pathlib import Path

CONTRACT = json.loads((Path(__file__).parent / 'static/servo-contract.json').read_text(encoding='utf-8'))
REVISION = CONTRACT['revision']
SERVOS = {s['id']: s for s in CONTRACT['servos']}
LOCKED = ('role',)


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
            raise ValueError('舵机%d编号和作用固定为%s；限位和回正角可在管理员参数中校准' %
                             (sid, fixed['name']))
