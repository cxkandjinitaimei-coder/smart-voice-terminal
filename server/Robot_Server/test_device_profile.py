# -*- coding: utf-8 -*-
"""管理员设备档案 / 动作库的无硬件回归。"""
import os
import json
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ai_relay
import app
import device_profile


class DeviceProfileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.old_path = device_profile.PROFILE_PATH
        device_profile.PROFILE_PATH = os.path.join(self.tmp.name, "profile.json")
        # 测试独立生成最小配置，完整交付目录不携带用户运行中的config.json。
        fixture_config = os.path.join(self.tmp.name, 'config.json')
        with open(fixture_config, 'w', encoding='utf-8') as stream:
            json.dump({'token':'test-only', 'providers':{}}, stream)
        # 测试独立生成最小配置，完整交付目录不携带用户运行中的config.json。
        fixture_config = os.path.join(self.tmp.name, 'config.json')
        with open(fixture_config, 'w', encoding='utf-8') as stream:
            json.dump({'token':'test-only', 'providers':{}}, stream)
        app._pending_commands.clear()
        self.patches = [patch.object(app,name) for name in ["start_auto_tick", "start_store", "start_beacon", "append_line"]]
        self.patches += [patch.object(app.asr,"warmup"), patch.object(app.store,"record_command")]
        self.patches += [patch.object(app, 'CONFIG_PATH', fixture_config)]
        self.patches += [patch.object(app, 'CONFIG_PATH', fixture_config)]
        for p in self.patches: p.start()

    def tearDown(self):
        for p in reversed(self.patches): p.stop()
        device_profile.PROFILE_PATH = self.old_path
        self.tmp.cleanup()

    def test_default_registers_servo_three_and_short_face_file(self):
        profile = device_profile.load()
        self.assertEqual([x["id"] for x in profile["servos"]], [1, 2, 3])
        eyes = next(x for x in profile["expressions"] if x["id"] == "eyesclose")
        self.assertEqual(eyes["file"], "EYESCLOS.BMP")

    def test_prompt_contains_third_servo_direction_and_timeline_truth(self):
        cfg = device_profile.apply_to_config(app.load_config())
        prompt = ai_relay._build_system_prompt(cfg)
        self.assertIn("id=3", prompt)
        self.assertIn("动作语义库", prompt)
        self.assertIn("同一类问候不必每次用相同动作", prompt)
        self.assertIn("F407 已支持非阻塞时间序列", prompt)
        self.assertIn("eyesclose", prompt)

    def test_frame_uses_fat83_alias_and_third_servo_limit(self):
        self.assertEqual(app.decision_to_raw({"moves": [
            {"type": "expression", "face": "eyesclose"}]}),
            "@MOVE:expression,eyesclos")
        clamped = []
        self.assertEqual(app.decision_to_raw({"moves": [
            {"type": "servo", "id": 3, "angle": 300}]}, clamped),
            "@MOVE:servo,3,180")
        self.assertTrue(clamped)

    def test_admin_and_action_endpoints(self):
        client = app.app.test_client()
        with client.session_transaction() as sess:
            sess["authed"] = True
        data = client.get("/api/admin/device-profile").get_json()
        self.assertTrue(data["ok"])
        self.assertEqual(data["profile"]["servos"][2]["id"], 3)
        r = client.post("/api/actions", json={"action_id": "nod"})
        data = r.get_json()
        self.assertTrue(data["ok"])
        self.assertTrue(data["raw"].startswith("@TIMELINE:"))
        self.assertEqual(len(data["action"]["plan"]["steps"]), 4)
        r = client.post("/api/actions", json={"action_id": "nod_twice_timeline"})
        self.assertEqual(r.status_code, 200)
        self.assertTrue(r.get_json()["raw"].startswith("@TIMELINE:"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
