"""Small gestures derived from the current administrator calibration."""
import copy
import math

SPECS = [('short_nod', '点头', 2, [0, 8, 0]),
         ('short_shake', '摇头', 1, [0, -12, 12, 0]),
         ('short_tilt_left', '向左歪头', 3, [0, -10, 0]),
         ('short_tilt_right', '向右歪头', 3, [0, 10, 0])]
IDS = {s[0] for s in SPECS}


def extend(profile):
    result = copy.deepcopy(profile)
    result['actions'] = [a for a in result['actions'] if a['id'] not in IDS]
    servos = {s['id']: s for s in result['servos']}
    for aid, name, sid, offsets in SPECS:
        action = dict(id=aid, name=name, custom=True, confirmed=True,
                      meaning=name+'后回到已保存的回正角', ready=False,
                      unavailable_reason='所需舵机未登记')
        s = servos.get(sid)
        if s:
            # Preserve explicit zero speed. Missing speed is filled at dispatch.
            speed = s.get('speed', 90)
            angles = [max(s['min'], min(s['max'], s['front']+d)) for d in offsets]
            steps, at = [], 0
            previous = None
            for angle in angles:
                move = dict(type='servo', id=sid, angle=angle)
                if 'speed' in s:
                    move['speed'] = speed
                steps.append(dict(at_ms=at, move=move))
                distance = (max(angle-s['min'], s['max']-angle)
                            if previous is None else abs(angle-previous))
                at += (math.ceil(distance*1000/speed) if speed else 0)+200
                previous = angle
            # Recipes too slow for the bounded board timer are reported unavailable.
            action.update(timeline=dict(steps=steps, duration_ms=at, repeat=1),
                          ready=at <= 60000 and len(set(angles)) > 1,
                          unavailable_reason='' if at <= 60000 and len(set(angles)) > 1 else
                          '当前限位或速度无法编排短动作')
        result['actions'].append(action)
    return result


def for_speed(profile, speed):
    result = copy.deepcopy(profile)
    for servo in result['servos']:
        if 'speed' not in servo:
            servo['speed'] = speed
    return extend(result)
