"""Real compiler, AI parse/prompt and authenticated Flask command delivery."""
import copy
import json
from pathlib import Path
import struct
import tempfile
from unittest.mock import patch
import app as A
import ai_relay
import device_profile as P
import motion_timeline as T

profile = P.default_profile()
faces = {"repeat": 3, "duration_ms": 6000, "steps": [
    {"at_ms": i*2000, "move": {"type": "expression", "face": f}}
    for i,f in enumerate(["happy", "think", "wink"])]}
wire, normal = T.compile_timeline(faces, profile)
packet = bytes.fromhex(wire.split(":",1)[1])
assert T.HEADER.unpack_from(packet) == (1,3,3,0,6000)
assert [T.STEP.unpack_from(packet,8+i*18)[0] for i in range(3)] == [0,2000,4000]
assert len(packet)==62
changed = copy.deepcopy(profile)
changed["servos"][1].update(min=20, max=40, mid=30, front=32)
gesture = {"repeat": 2, "steps": [{"at_ms": 0, "move": {"type":"gesture", "name":"nod", "speed":30}}]}
_, old = T.compile_timeline(gesture, profile)
_, new = T.compile_timeline(gesture, changed)
assert [s["move"]["angle"] for s in new["steps"]]==[20,40,20,32]
assert new["duration_ms"] < old["duration_ms"]
for bad in [dict(faces, repeat=21), dict(faces, duration_ms=4000), dict(faces, repeat=True),
            {"steps":[{"at_ms":0,"move":{"type":"servo","id":1,"angle":14}}]},
            {"steps":[{"at_ms":0,"move":{"type":"expression","face":"nonexistent"}}]},
            {"steps":[{"at_ms":0,"move":{"type":"servo","id":1,"angle":90}},
                      {"at_ms":0,"move":{"type":"servo","id":1,"angle":95}}]},
            {"steps":[{"at_ms":0,"move":{"type":"gesture","name":"nod"}},
                      {"at_ms":10,"move":{"type":"gesture","name":"nod"}}]}]:
    try: T.compile_timeline(bad, profile)
    except ValueError: pass
    else: raise AssertionError(bad)
decision = ai_relay.parse_decision(json.dumps({"message":"轮流切换", "timeline":faces}), [])
assert decision["timeline"] == faces
with tempfile.TemporaryDirectory(prefix="timeline-web-") as temp:
    with patch.object(P, "PROFILE_PATH", str(Path(temp)/"profile.json")), \
         patch.object(A, "start_auto_tick"), patch.object(A, "start_store"), patch.object(A, "start_beacon"), \
         patch.object(A.asr, "warmup"), patch.object(A, "append_line"), \
         patch.object(A, "load_config", return_value={"token":"timeline-test-only"}), \
         patch.object(A.device_controls, "get", return_value={"servo_speed":90}), \
         patch.object(A.store, "record_command"):
        P.save(profile)
        client = A.app.test_client()
        assert client.post("/api/command",json={"timeline":faces}).status_code==401
        with client.session_transaction() as session: session["authed"]=True
        r=client.post("/api/timeline/preview",json={"timeline":faces})
        assert r.status_code==200 and A._queue_len("esp-bridge")==0
        r=client.post("/api/command",json={"timeline":faces})
        assert r.status_code==200
        cmd=A.pop_command("esp-bridge")
        assert cmd["raw"]==wire+"#" and "timeline" not in cmd
        assert client.post("/api/command",json={"raw":wire+"#"}).status_code==400
        assert client.post("/api/command",json={"timeline":dict(faces,repeat=99)}).status_code==400
        assert A._queue_len("esp-bridge")==0
        assert client.post("/api/command",json={"timeline":{"stop":True}}).status_code==200
        assert A.pop_command("esp-bridge")["raw"]=="@TIMELINE:STOP#"
        P.save(changed)
        assert P.load()['servos'][1]['front']==32
        snapshot=P.motion_snapshot()
        data=bytes.fromhex(snapshot["wire"])
        assert struct.unpack_from("<6B", data,6)==(1,15,165,90,2,0)
        assert struct.unpack_from("<6B", data,12)==(2,20,40,32,1,0)
        assert P.motion_snapshot(snapshot["revision"])["applied"]
        cfg=P.apply_to_config({})
        prompt=ai_relay._build_system_prompt(cfg)
        assert "15~165" in prompt and "正面角度 32" in prompt and "已支持非阻塞时间序列" in prompt
        assert "当前没有定时动作队列" not in prompt
        raw=A.decision_to_raw(decision)
        assert raw.startswith("@TIMELINE:") and raw.endswith("@TEXT:轮流切换")
        bad={"message":"参数有误", "timeline":{"steps":[]}, "moves":[{"type":"servo","id":1,"angle":90}]}
        assert A.decision_to_raw(bad)=="@TEXT:参数有误" and "timeline_error" in bad
print("PASS timeline compiler, gestures from edited limits/front, strict validation, AI context/parse, auth, atomic enqueue, preview, cancel, current limits packet and invalid-AI no burst")
