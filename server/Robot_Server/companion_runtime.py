"""One-time local upgrade and visible controls for short gestures / photo location."""
import json
import os
from pathlib import Path
from flask import jsonify, request
import short_actions
from photo_locate import Locator

LOCATOR = Locator()

REVISION = 'short-locate-20261029-07'


def prepare(relay):
    """Run after importing the previous camera data; never reset later user choices."""
    marker = Path(relay.BASE_DIR)/'companion_setup.json'
    try:
        if json.loads(marker.read_text(encoding='utf-8')).get('revision') == REVISION:
            return
    except (OSError, ValueError):
        pass
    device = relay.DEFAULT_DEVICE_ID
    profile = relay.device_profile.load()
    speed = relay.device_controls.get(device)['servo_speed']
    cfg = relay.load_config()
    # F407 runs the short idle clips. Disable the competing PC presence scheduler.
    cfg.setdefault('presence', {})['enabled'] = False
    cfg['web_version'] = 'W2026.10.29-07'
    relay.save_config(cfg)
    settings = relay.camera.current_settings(device)
    settings.update(follow_enabled=True)
    if settings['capture_mode']=='continuous':
        settings['capture_mode']='sound'
    relay.camera.save(device, settings)
    relay.idle_pet.save(device, dict(enabled=True, idle_seconds=30, sleep_seconds=180,
                                    gap_seconds=30, awake_actions=['short_nod','short_tilt_left'],
                                    sleep_action=''), profile, speed)
    temporary = marker.with_suffix('.tmp')
    temporary.write_text(json.dumps(dict(revision=REVISION),ensure_ascii=False),encoding='utf-8')
    os.replace(temporary, marker)


def status(relay, device):
    settings = relay.camera.current_settings(device)
    pose = relay.board_pose.get(device)
    age = relay.device_age_sec(device)
    online = age is not None and age <= relay.DEVICE_ONLINE_SEC
    camera_age = relay.camera.clock()-relay.camera.seen.get(device, 0)
    blockers = relay.camera_blockers(device)
    follow = relay.follow.status(device)
    once = LOCATOR.status(device)
    if not online:
        reason = '执行板离线，等待 ESP8266 重新上报'
    elif once['phase']=='waiting_pose' and not pose.get('fresh'):
        reason = '等待 F407 新鲜角度上报'
    elif once['phase']=='waiting_photo' and camera_age > 20:
        reason = '等待 ESP32-S3 相机联网并上传新照片'
    else:
        reason = once['reason']
    return dict(ok=True, device_id=device, device_online=online,
                camera_online=camera_age <= 20, pose=pose,
                follow_enabled=settings['follow_enabled'], locate=once, reason=reason,
                capture_mode=settings['capture_mode'],
                idle=relay.idle_pet.get(device),
                idle_applied=relay.idle_pet.snapshot(device,relay.device_profile.load(),
                                relay.device_controls.get(device)['servo_speed'])['applied'])


def observe_photo(relay,device,job,created,analysis,settings,source):
    if str(job).startswith('live-'):
        return
    with LOCATOR.lock:
        if (source=='device' and settings['follow_enabled'] and
                not LOCATOR.pending(device)):
            if not LOCATOR.start(device,job):
                return
        LOCATOR.observe(device,job,created,analysis,settings,source)


def install(relay):
    def observe(*args):
        observe_photo(relay,*args)
    relay.camera.on_analysis=observe

    @relay.app.errorhandler(relay.camera_workspace.CameraError)
    def camera_error(error):
        return jsonify(error=str(error)),error.code

    def device():
        return relay.camera_workspace.device_id((request.get_json(silent=True) or {}).get(
            'device_id') or request.args.get('device_id') or relay.DEFAULT_DEVICE_ID)

    @relay.app.route('/api/companion/status')
    def companion_status():
        return jsonify(status(relay, device()))

    @relay.app.route('/api/companion/locate', methods=['POST'])
    def companion_locate():
        body = request.get_json(silent=True) or {}
        did = device()
        if body.get('cancel') is True:
            job=LOCATOR.status(did).get('job')
            LOCATOR.cancel(did)
            if job:
                with relay.camera.lock,relay.camera.db() as db:
                    db.execute("UPDATE jobs SET state='cancelled' WHERE id=? AND device=? AND state IN ('queued','capturing')",(job,did))
            return jsonify(status(relay,did))
        if LOCATOR.pending(did):
            return jsonify(error='已有一次定位正在等待，请完成或取消后重试'),409
        age=relay.device_age_sec(did)
        if age is None or age>relay.DEVICE_ONLINE_SEC:
            return jsonify(error='执行板离线，请联网后再定位'),409
        settings = relay.camera.current_settings(did)
        if settings['capture_mode']=='continuous':
            settings.update(capture_mode='sound',follow_enabled=False)
            relay.camera.save(did,settings)
        job=relay.camera.queue_device(did)
        LOCATOR.start(did,job['job_id'])
        return jsonify(ok=True,job_id=job['job_id'],status=status(relay,did))

    @relay.app.route('/api/companion/idle', methods=['POST'])
    def companion_idle():
        body = request.get_json(silent=True) or {}
        if type(body.get('enabled')) is not bool:
            return jsonify(error='enabled 必须为开关'),400
        did = device()
        settings = dict(relay.idle_pet.get(did), enabled=body['enabled'],
                        awake_actions=['short_nod','short_tilt_left'],sleep_action='')
        try:
            relay.idle_pet.save(did,settings,relay.device_profile.load(),
                               relay.device_controls.get(did)['servo_speed'])
            if body['enabled']:
                cfg=relay.load_config()
                cfg.setdefault('presence',{})['enabled']=False
                relay.save_config(cfg)
            return jsonify(status(relay,did))
        except (ValueError,TypeError) as error:
            return jsonify(error=str(error)),400

    @relay.app.route('/api/companion/actions', methods=['GET','POST'])
    def companion_actions():
        did=device()
        speed=relay.device_controls.get(did)['servo_speed']
        profile=short_actions.for_speed(relay.device_profile.load(),speed)
        actions=[relay._action_plan(a,profile,speed)[0] for a in profile['actions'] if a['id'] in short_actions.IDS]
        if request.method=='GET':
            return jsonify(ok=True,actions=actions)
        aid=(request.get_json(silent=True) or {}).get('action_id')
        action=next((a for a in actions if a['id']==aid),None)
        if action is None:
            return jsonify(error='请选择点头、摇头或歪头短动作'),400
        age=relay.device_age_sec(did)
        if age is None or age>relay.DEVICE_ONLINE_SEC:
            return jsonify(error='执行板离线，请联网后再发送短动作'),409
        if any(relay.camera_blockers(did).values()):
            return jsonify(error='录音、OTA 或动作执行中，请稍后重试'),409
        action,raw=relay._action_plan(action,profile,speed)
        if not action.get('ready') or not raw:
            return jsonify(error=action.get('unavailable_reason') or '当前校准无法编排短动作'),409
        cmd=dict(device_id=did,raw=raw,note='short_action:'+aid,time=relay.now_str())
        relay.enqueue_command(did,cmd)
        return jsonify(ok=True,action=action,queue_len=relay._queue_len(did),
                       message='短动作已入队，等待执行板领取')
