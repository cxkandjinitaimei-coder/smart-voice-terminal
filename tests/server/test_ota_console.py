"""test_ota_console.py：使用真实服务模块的离线回归。

使用：由 tools/run_checks.py 在临时服务副本中单独运行；不连接硬件、不调用真实 AI。
范围：断言涵盖本文件列出的状态/边界；测试通过不代表飞线信号、供电和公网时延已验证。
依赖：server/Robot_Server/requirements.txt；数据由测试临时创建并清理。
"""
# -*- coding: utf-8 -*-
import functools
import io
import os
from datetime import datetime
import shutil
import struct
import sys
import tempfile
import time
import unittest
import zlib
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import app                      # noqa: E402
import f407_ota_relay           # noqa: E402
import ota_console              # noqa: E402

SLOT_BASE = f407_ota_relay.SLOT_A_BASE


def valid_bin(size=512):
    """一个能过体检的 Slot A 镜像（首向量合法、入口落在镜像内）。"""
    b = bytearray([0xFF] * size)
    b[0:4] = (0x2001CBB0).to_bytes(4, "little")
    b[4:8] = (SLOT_BASE + 0x1A1).to_bytes(4, "little")   # 0x080081A1，Thumb 位置 1
    return bytes(b)


def padded_crc(data):
    return zlib.crc32(data + b"\xff" * (f407_ota_relay.MAX_APP - len(data))) & 0xffffffff


class _Resp(object):
    def __init__(self, body, status=200):
        self._body = body
        self.status_code = status

    def json(self):
        if self._body is None:
            raise ValueError("not json")
        return self._body


class ScriptedSession(object):
    """按顺序回放应答的假 `requests.Session`。

    脚本是 `[(kind, body)]`，kind ∈ {"status","request","update"}；
    脚本用完后一律回放 `tail`（用来模拟"一直等不到 K-UP"）。
    """

    def __init__(self, script, tail=None):
        self.script = list(script)
        self.tail = tail
        self.trust_env = True
        self.calls = []

    def _next(self, kind):
        if not self.script:
            if self.tail is None:
                raise AssertionError("脚本用完了，还来要 %s" % kind)
            self.calls.append(kind)
            return _Resp(self.tail)
        want, body = self.script.pop(0)
        assert want == kind, "脚本顺序不符：期望 %s，实得 %s" % (want, kind)
        self.calls.append(kind)
        return _Resp(body)

    def get(self, url, headers=None, timeout=None):
        return self._next("status")

    def post(self, url, params=None, headers=None, files=None, timeout=None):
        return self._next("update" if "/update" in url else "request")

    def close(self):
        pass


def _online_state(approval="none", slot_crc=None):
    body = {"online": True, "base": SLOT_BASE, "approval": approval,
            "capacity": f407_ota_relay.MAX_APP, "layout_id": f407_ota_relay.LAYOUT_ID}
    if slot_crc is not None:
        body["slot_crc"] = slot_crc
    return body


class OtaConsoleTestCase(unittest.TestCase):
    def setUp(self):
        app.app.config["TESTING"] = True
        self.client = app.app.test_client()
        self.tmp = tempfile.mkdtemp(prefix="hz_ota_test_")
        self._fw_dir = ota_console.FW407_DIR
        ota_console.FW407_DIR = os.path.join(self.tmp, "firmware_f407")
        # 任务表：清干净，否则上个用例的 _active_id 残留会让这里全变 409
        ota_console._jobs.clear()
        del ota_console._job_order[:]
        ota_console._active_id = ""
        self._device_ip = dict(app._device_ip)
        self._device_last_seen = dict(app._device_last_seen)

    def tearDown(self):
        ota_console.FW407_DIR = self._fw_dir
        app._device_ip.clear()
        app._device_ip.update(self._device_ip)
        app._device_last_seen.clear()
        app._device_last_seen.update(self._device_last_seen)
        ota_console._jobs.clear()
        del ota_console._job_order[:]
        ota_console._active_id = ""
        shutil.rmtree(self.tmp, ignore_errors=True)

    def login(self):
        with self.client.session_transaction() as sess:
            sess["authed"] = True


# ----------------------------------------------------------------------
# A. ota_console 单元
# ----------------------------------------------------------------------
class OtaConsoleUnitTests(OtaConsoleTestCase):
    def test_image_check_reasons_are_actionable(self):
        self.assertEqual(ota_console._check(valid_bin()), (True, ""))
        # 没置 Thumb 位 ⇒ 点破"这可能是 ESP 固件"
        ok, why = ota_console._check(struct.pack("<II", 0x2001CBB0, 0x080081A0) + b"\xff" * 504)
        self.assertFalse(ok)
        self.assertIn("ESP8266", why)
        # 链接到 0x08000000 的普通（非 OTA）工程要单独认出来
        ok, why = ota_console._check(struct.pack("<II", 0x2001CBB0, 0x08000101) + b"\xff" * 504)
        self.assertFalse(ok)
        self.assertIn("0x08000000", why)
        self.assertIn("OTA", why)
        # 超过槽位容量
        ok, why = ota_console._check(valid_bin(f407_ota_relay.MAX_APP + 8))
        self.assertFalse(ok)
        self.assertIn("容量", why)

    def test_filename_whitelist_blocks_traversal(self):
        for bad in ("../config.json", "..\\config.json", "a/b.bin", "a\\b.bin",
                    ".hidden.bin", "x.hex", "", None):
            with self.assertRaises(ota_console.OtaError):
                ota_console._safe_name(bad)
        self.assertEqual(ota_console._safe_name("F407_x.bin"), "F407_x.bin")

    def test_library_roundtrip_and_no_junk_on_disk(self):
        good = ota_console.save_firmware("F407_DHT11_RELAY.bin", valid_bin())
        self.assertTrue(good["ok"] and good["valid"] and good["size"] == 512)
        # 体检不过的**不落盘**
        bad = ota_console.save_firmware(
            "ESP_bridge.bin", struct.pack("<II", 0x2001CBB0, 0x40000101) + b"\xff" * 504)
        self.assertFalse(bad["ok"])
        self.assertFalse(bad["valid"])
        self.assertTrue(bad["reason"])
        items = ota_console.list_firmware()
        self.assertEqual([i["name"] for i in items], ["F407_DHT11_RELAY.bin"])
        self.assertTrue(items[0]["crc32"])
        with self.assertRaises(ota_console.OtaError):
            ota_console.delete_firmware("nope.bin")
        self.assertEqual(ota_console.delete_firmware("F407_DHT11_RELAY.bin"),
                         "F407_DHT11_RELAY.bin")
        self.assertEqual(ota_console.list_firmware(), [])

    def test_resolve_target_speaks_to_the_user(self):
        self.assertEqual(ota_console.resolve_target("192.168.1.9", {}), ("192.168.1.9", ""))
        self.assertEqual(ota_console.resolve_target(" esp-bridge ", {"esp-bridge": "10.0.0.5"}),
                         ("10.0.0.5", "esp-bridge"))
        with self.assertRaises(ota_console.OtaError) as ctx:
            ota_console.resolve_target("esp-bridge", {})
        self.assertIn("确认设备在线", str(ctx.exception))
        # URL / 域名 / 写错的 IP 不该掉进"等它上报就好"那句
        for bad in ("http://a.b", "esp.bridge", "10.0.0.999", "::1"):
            with self.assertRaises(ota_console.OtaError) as ctx:
                ota_console.resolve_target(bad, {})
            self.assertIn("IPv4", str(ctx.exception), bad)


# ----------------------------------------------------------------------
# B. HTTP 层
# ----------------------------------------------------------------------
class OtaConsoleHttpTests(OtaConsoleTestCase):
    def test_otactl_must_go_through_login(self):
        """★ 本页最容易踩的坑：端点绝不能落在 query-token 免登录里。"""
        self.assertEqual(self.client.get("/api/otactl/fw").status_code, 401)
        # 拿真 token 当 query 打，**必须仍然 401**（前缀不是 /api/ota/ 才这样）
        resp = self.client.get("/api/otactl/fw?token=" + app.check_token())
        self.assertEqual(resp.status_code, 401)
        # 登录后放行
        self.login()
        self.assertEqual(self.client.get("/api/otactl/fw").status_code, 200)

    def test_upload_list_delete_over_http(self):
        self.login()
        resp = self.client.post(
            "/api/otactl/fw",
            data={"firmware": (io.BytesIO(valid_bin()), "F407_TEST.bin")},
            content_type="multipart/form-data")
        self.assertEqual(resp.status_code, 200)
        body = resp.get_json()
        self.assertTrue(body["ok"] and body["valid"], body)
        self.assertEqual(body["size"], 512)

        listed = self.client.get("/api/otactl/fw").get_json()
        self.assertEqual([i["name"] for i in listed["items"]], ["F407_TEST.bin"])
        self.assertEqual(listed["max_bytes"], f407_ota_relay.MAX_APP)

        # 非法镜像：200 + valid:false + 人话原因（不是 400，页面要显示原因）
        resp = self.client.post(
            "/api/otactl/fw",
            data={"firmware": (io.BytesIO(b"\0" * 128), "junk.bin")},
            content_type="multipart/form-data")
        self.assertEqual(resp.status_code, 200)
        self.assertFalse(resp.get_json()["valid"])
        self.assertTrue(resp.get_json()["reason"])

        # 逗号名字/穿越名：400
        resp = self.client.post(
            "/api/otactl/fw",
            data={"firmware": (io.BytesIO(valid_bin()), "../config.json")},
            content_type="multipart/form-data")
        self.assertEqual(resp.status_code, 400)

        self.assertEqual(self.client.delete("/api/otactl/fw/nope.bin").status_code, 404)
        self.assertEqual(self.client.delete("/api/otactl/fw/F407_TEST.bin").status_code, 200)

    def test_flash_and_single_job_lock(self):
        self.login()
        ota_console.save_firmware("ok.bin", valid_bin())

        # 未知名字 ⇒ 400 且文案可执行
        resp = self.client.post("/api/otactl/flash", json={"target": "who", "name": "ok.bin"})
        self.assertEqual(resp.status_code, 400)
        self.assertIn("确认设备在线", resp.get_json()["error"])

        def slow(esp_ip, data, token, session=None, progress=None):
            progress("probing", {})
            progress("waiting_user", {"remaining_sec": 9})
            time.sleep(0.4)
            return {"ok": True, "esp_ip": esp_ip, "bytes": len(data), "crc32": "11111111",
                    "slot_a_crc32": "22222222", "message": "PASS: stub"}

        with patch.object(f407_ota_relay, "perform_lan_ota", side_effect=slow):
            resp = self.client.post("/api/otactl/flash",
                                    json={"target": "10.0.0.5", "name": "ok.bin"})
            self.assertEqual(resp.status_code, 200, resp.get_json())
            job_id = resp.get_json()["job_id"]
            self.assertEqual(resp.get_json()["esp_ip"], "10.0.0.5")
            # 同时只允许一个任务
            again = self.client.post("/api/otactl/flash",
                                     json={"target": "10.0.0.5", "name": "ok.bin"})
            self.assertEqual(again.status_code, 409)

        job = None
        for _ in range(200):
            job = self.client.get("/api/otactl/job/" + job_id).get_json()["job"]
            if job["state"] != "running":
                break
            time.sleep(0.02)
        self.assertEqual(job["state"], "done", job)
        self.assertTrue(job["result"]["ok"])
        self.assertNotIn("_started_mono", job)

    def test_preflight(self):
        self.login()
        app._device_ip["esp-bridge"] = "127.0.0.1"
        app._device_last_seen["esp-bridge"] = datetime.now()
        body = self.client.get("/api/otactl/preflight?target=esp-bridge").get_json()
        self.assertTrue(body["ok"], body)
        self.assertEqual(body["esp_ip"], "127.0.0.1")
        self.assertEqual(body["known_device"], "esp-bridge")
        self.assertTrue(body["same_subnet"])
        self.assertEqual(body["warn"], "")
        # 跨网段 ⇒ 必须给出"到不了"的诚实诊断，而不是留空
        body = self.client.get("/api/otactl/preflight?target=10.99.99.99").get_json()
        self.assertFalse(body["same_subnet"])
        self.assertTrue(body["warn"])
        self.assertIn("10.99.99.99", body["warn"])
        self.assertEqual(self.client.get("/api/otactl/preflight?target=who").status_code, 400)


# ----------------------------------------------------------------------
# C. 进度链路：桩 session 喂真的 perform_lan_ota
# ----------------------------------------------------------------------
class OtaProgressTests(OtaConsoleTestCase):
    def _run(self, session, **kw):
        runner = functools.partial(f407_ota_relay.perform_lan_ota, **kw)
        snap = ota_console.start_flash_job("192.168.1.9", "ok.bin", "TK", {},
                                           runner=runner, session=session)
        for _ in range(400):
            job = ota_console.get_job(snap["id"])
            if job["state"] != "running":
                return job
            time.sleep(0.02)
        raise AssertionError("任务没在 8 秒内结束")

    def setUp(self):
        super(OtaProgressTests, self).setUp()
        ota_console.save_firmware("ok.bin", valid_bin())

    def test_phase_sequence_and_pass(self):
        data = valid_bin()
        good = _online_state(slot_crc=padded_crc(data))
        session = ScriptedSession([
            ("status", _online_state()),        # 探测
            ("request", {"approval": "waiting_user"}),
            ("status", {"approval": "waiting_user"}),
            ("status", {"approval": "approved"}),
            ("update", {"ok": True}),
            ("status", good),                   # 重启后 CRC 对上
        ])
        job = self._run(session, poll_interval=0.01)
        self.assertEqual(job["state"], "done", job)
        self.assertEqual(job["phase"], "done")
        self.assertTrue(job["result"]["ok"])
        self.assertEqual(job["result"]["slot_a_crc32"], "%08X" % padded_crc(data))
        # 四个阶段都真的来过，且顺序正确
        texts = [x["text"] for x in job["log"]]
        for phase in ("probing", "waiting_user", "uploading", "verifying"):
            self.assertTrue(any(ota_console.PHASE_START[phase] == t for t in texts),
                            "缺阶段 %s：%s" % (phase, texts))
        self.assertTrue(any(x["level"] == "warn" for x in job["log"]))
        self.assertEqual(ota_console.active_job_id(), "")

    def test_kup_timeout_is_failed_not_done(self):
        session = ScriptedSession(
            [("status", _online_state()), ("request", {"approval": "waiting_user"})],
            tail={"approval": "waiting_user"})          # 一直没人按 K-UP
        job = self._run(session, approval_timeout=0.05, poll_interval=0.01)
        self.assertEqual(job["state"], "failed", job)
        self.assertEqual(job["phase"], "failed")
        self.assertFalse(job["result"]["ok"])
        self.assertIn("未上传、未擦除固件", job["result"]["message"])
        self.assertNotIn("update", session.calls)       # ★ 关键：根本没走到上传
        self.assertEqual(ota_console.active_job_id(), "")

    def test_not_ready_device_fails_before_any_upload(self):
        session = ScriptedSession([("status", {"online": False, "base": 0})])
        job = self._run(session, poll_interval=0.01)
        self.assertEqual(job["state"], "failed")
        self.assertNotIn("update", session.calls)
        self.assertNotIn("request", session.calls)      # 探测不过就不该请求确认


if __name__ == "__main__":
    unittest.main(verbosity=2)
