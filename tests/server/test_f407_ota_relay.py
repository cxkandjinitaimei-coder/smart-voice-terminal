"""test_f407_ota_relay.py：使用真实服务模块的离线回归。

使用：由 tools/run_checks.py 在临时服务副本中单独运行；不连接硬件、不调用真实 AI。
范围：断言涵盖本文件列出的状态/边界；测试通过不代表飞线信号、供电和公网时延已验证。
依赖：server/Robot_Server/requirements.txt；数据由测试临时创建并清理。
"""
# -*- coding: utf-8 -*-
import io
import unittest
import zlib
from unittest.mock import patch

import app
import f407_ota_relay


def valid_bin(size=64):
    b = bytearray([0xFF] * size)
    b[0:4] = (0x20010000).to_bytes(4, "little")
    b[4:8] = (0x08008009).to_bytes(4, "little")
    return bytes(b)


class F407RelayTests(unittest.TestCase):
    def setUp(self):
        app.app.config["TESTING"] = True
        self.client = app.app.test_client()
        # Authentication is a fixture, never the owner's live config token.
        for name in ('start_auto_tick', 'start_store', 'start_beacon'):
            handle = patch.object(app, name); handle.start(); self.addCleanup(handle.stop)
        handle = patch.object(app.asr, 'warmup'); handle.start(); self.addCleanup(handle.stop)
        handle = patch.object(app, 'load_config', return_value={'token':'ota-fixture-only'})
        handle.start(); self.addCleanup(handle.stop)

    def test_invalid_vector_is_rejected(self):
        with self.assertRaises(f407_ota_relay.RelayError):
            f407_ota_relay.validate_image(b"\0" * 64)

    def test_expanded_size_and_padded_crc(self):
        for size in (98305, 131073, 229376):
            data = valid_bin(size)
            raw, padded = f407_ota_relay.validate_image(data)
            self.assertEqual(raw, zlib.crc32(data))
            self.assertEqual(padded, zlib.crc32(data + b"\xff" * (229376-size)))
        with self.assertRaises(f407_ota_relay.RelayError):
            f407_ota_relay.validate_image(valid_bin(229377))

    def test_old_layout_rejected_before_request_or_upload(self):
        from unittest.mock import Mock
        for state in ({"online": True, "base": 0x08008000},
                      {"online": True, "base": 0x08008000, "capacity": 98304, "layout_id": 0},
                      {"online": True, "base": 0x08008000, "capacity": 229376, "layout_id": 0}):
            session = Mock()
            session.get.return_value.status_code = 200
            session.get.return_value.json.return_value = state
            with self.assertRaises(f407_ota_relay.RelayError):
                f407_ota_relay.perform_lan_ota("192.168.1.9", valid_bin(), "test", session=session)
            session.post.assert_not_called()

    def test_token_is_required(self):
        response = self.client.post("/api/f407/ota", data={"esp_ip": "127.0.0.1"})
        self.assertEqual(response.status_code, 401)

    def test_relay_success(self):
        expected = {"ok": True, "esp_ip": "192.168.1.9", "bytes": 64,
                    "crc32": "11111111", "slot_a_crc32": "22222222",
                    "message": "PASS"}
        with patch.object(f407_ota_relay, "perform_lan_ota", return_value=expected) as run:
            response = self.client.post(
                "/api/f407/ota",
                headers={"Authorization": "Bearer ota-fixture-only"},
                data={"esp_ip": "192.168.1.9",
                      "firmware": (io.BytesIO(valid_bin()), "app.bin")},
                content_type="multipart/form-data")
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.get_json()["ok"])
        run.assert_called_once()

    def test_relay_failure_is_not_pass(self):
        with patch.object(f407_ota_relay, "perform_lan_ota",
                          side_effect=f407_ota_relay.RelayError("未按 K-UP")):
            response = self.client.post(
                "/api/f407/ota",
                headers={"Authorization": "Bearer ota-fixture-only"},
                data={"esp_ip": "192.168.1.9",
                      "firmware": (io.BytesIO(valid_bin()), "app.bin")},
                content_type="multipart/form-data")
        self.assertEqual(response.status_code, 409)
        self.assertFalse(response.get_json()["ok"])


if __name__ == "__main__":
    unittest.main(verbosity=2)

