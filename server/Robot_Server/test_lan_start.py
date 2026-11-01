"""LAN config isolation and startup guards, with real loopback UDP discovery.

Does not use port 5000/5001, contact boards, download models, or call AI.
"""
import contextlib
import io
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

HERE = Path(__file__).resolve().parent
WEB = HERE.parent / "网页局域网"
sys.path.insert(0, str(WEB if WEB.is_dir() else HERE))
import lan_start as L


class LANStartTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lan-start-test-")
        self.root = Path(self.temp.name)
        self.web = self.root / "交付" / "LAN" / "网页局域网"
        self.web.mkdir(parents=True)
        self.seed = self.root / "flask_relay" / "config.json"
        self.seed.parent.mkdir()
        self.seed.write_text(json.dumps({"token": "test-login-secret", "host": "127.0.0.1",
                                         "port": 7777, "providers": {"local-test": {"key": "test-key-secret"}}}),
                             encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def test_camera_upgrade_imports_once_without_changing_previous_gallery(self):
        import sqlite3
        source=self.root/'old-camera'
        (source/'face_library').mkdir(parents=True)
        (source/'face_library/people.json').write_text('["reference"]',encoding='utf-8')
        (source/'photo.jpg').write_bytes(b'example-frame')
        with contextlib.closing(sqlite3.connect(source/'camera.sqlite3')) as db, db:
            db.execute('CREATE TABLE sample(value TEXT)')
            db.execute("INSERT INTO sample VALUES ('old-record')")
        (self.web/'lan_seed.json').write_text(json.dumps({'source_camera_data':str(source)}),encoding='utf-8')
        L.import_camera_data(self.web)
        self.assertEqual((self.web/'camera_data/photo.jpg').read_bytes(),b'example-frame')
        with contextlib.closing(sqlite3.connect(self.web/'camera_data/camera.sqlite3')) as db:
            self.assertEqual(db.execute('SELECT value FROM sample').fetchone()[0],'old-record')
        (self.web/'camera_data/face_library/people.json').write_text('["new"]',encoding='utf-8')
        L.import_camera_data(self.web)
        self.assertEqual((self.web/'camera_data/face_library/people.json').read_text(),'["new"]')
        self.assertEqual((source/'face_library/people.json').read_text(),'["reference"]')

    def test_seed_is_preserved_and_history_not_copied(self):
        original = self.seed.read_bytes()
        (self.seed.parent / "audio_state.json").write_text('{"mode":"confirm"}', encoding="utf-8")
        (self.seed.parent / "relay.db").write_bytes(b"do-not-copy")
        (self.seed.parent / "recordings").mkdir()
        (self.seed.parent / "recordings" / "old.wav").write_bytes(b"history")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            config = L.setup_config(self.web)
        self.assertEqual(self.seed.read_bytes(), original)
        self.assertEqual((config["host"], config["port"]), ("0.0.0.0", 5000))
        self.assertEqual(config["token"], "test-login-secret")
        self.assertTrue((self.web / "audio_state.json").is_file())
        self.assertFalse((self.web / "relay.db").exists())
        self.assertFalse((self.web / "recordings").exists())
        self.assertNotIn("test-login-secret", output.getvalue())
        self.assertNotIn("test-key-secret", output.getvalue())

    def test_camera_upgrade_falls_back_when_previous_release_was_never_started(self):
        older=self.root/'started-05'
        older.mkdir();(older/'photo.jpg').write_bytes(b'previous-user-data')
        (self.web/'lan_seed.json').write_text(json.dumps({'source_camera_data':'missing-06',
            'fallback_camera_data':[str(older)]}),encoding='utf-8')
        L.import_camera_data(self.web)
        self.assertEqual((self.web/'camera_data/photo.jpg').read_bytes(),b'previous-user-data')

    def test_existing_config_and_audio_settings_are_kept(self):
        target = self.web / "config.json"
        target.write_text('{"token":"own-settings","host":"0.0.0.0","port":5000}', encoding="utf-8")
        audio = self.web / "audio_state.json"
        audio.write_text('{"mode":"own-mode"}', encoding="utf-8")
        original = target.read_bytes()
        L.setup_config(self.web)
        self.assertEqual(target.read_bytes(), original)
        self.assertEqual(audio.read_text(), '{"mode":"own-mode"}')

    def test_delivery_hint_reuses_last_lan_settings_without_overwriting(self):
        old = self.root / "last-lan" / "config.json"
        old.parent.mkdir()
        old.write_text('{"token":"last-lan-settings","host":"0.0.0.0","port":5000}', encoding="utf-8")
        (old.parent / "device_controls.json").write_text('{"minimum":600}', encoding="utf-8")
        (self.web / "lan_seed.json").write_text(json.dumps({"source_config": str(old)}), encoding="utf-8")
        before = old.read_bytes()
        config = L.setup_config(self.web)
        self.assertEqual(config["token"], "last-lan-settings")
        self.assertEqual(old.read_bytes(), before)
        self.assertEqual((self.web / "device_controls.json").read_text(), '{"minimum":600}')

    def test_missing_config_or_login_fails_clearly(self):
        with self.assertRaisesRegex(RuntimeError, "config.json"):
            L.setup_config(self.web, self.root / "absent.json")
        (self.web / "config.json").write_text('{"host":"127.0.0.1"}', encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "口令"):
            L.setup_config(self.web)

    def test_reuses_model_cache_and_honors_override(self):
        cache = self.root / "语音识别" / "models" / "snapshot"
        cache.mkdir(parents=True)
        (cache / "model.bin").write_bytes(b"model-fixture")
        override = self.root / "external-models"
        override.mkdir()
        (override / "model.bin").write_bytes(b"external-fixture")
        with patch.dict(os.environ, {"ASR_MODELS_DIR": ""}):
            self.assertEqual(L.find_models(self.web), cache.parent.resolve())
        with patch.dict(os.environ, {"ASR_MODELS_DIR": str(override)}):
            self.assertEqual(L.find_models(self.web), override.resolve())
        (cache / "model.bin").unlink()
        with patch.dict(os.environ, {"ASR_MODELS_DIR": ""}):
            with self.assertRaisesRegex(RuntimeError, "模型缓存"):
                L.find_models(self.root / "unrelated")

    def run_main(self, result, prepare_only):
        probe = SimpleNamespace(prepare=Mock(return_value=result))
        relay = SimpleNamespace(start_auto_tick=Mock(), start_store=Mock(), start_beacon=Mock(),
                                app=SimpleNamespace(run=Mock()))
        args = ["lan_start.py"] + (["--prepare-only"] if prepare_only else [])
        with patch.object(L, "__file__", str(self.web / "lan_start.py")), \
             patch.object(sys, "argv", args), patch.object(L, "setup_config"), \
             patch.object(L, "find_models", return_value=self.root / "cached-models"), \
             patch.object(L, "ensure_port_free") as ports, patch.object(L, "lan_addresses", return_value=[]), \
             patch.dict(sys.modules, {"asr_check": probe, "app": relay}), patch.dict(os.environ):
            code = L.main()
            self.assertEqual(os.environ["HF_HUB_OFFLINE"], "1")
            self.assertEqual(os.environ["ASR_MODELS_DIR"], str(self.root / "cached-models"))
        return code, relay, ports

    def test_prepare_only_does_not_bind_start_services_or_send_commands(self):
        code, relay, ports = self.run_main(0, True)
        self.assertEqual(code, 0)
        ports.assert_not_called()
        relay.app.run.assert_not_called()
        relay.start_beacon.assert_not_called()
        relay.start_store.assert_not_called()
        relay.start_auto_tick.assert_not_called()

    def test_camera_only_preflight_needs_no_voice_model(self):
        with patch.object(sys, 'argv', ['lan_start.py', '--camera-only', '--prepare-only']), \
             patch.object(L, 'setup_config'), patch.object(L, 'import_camera_data'), \
             patch.object(L, 'find_models') as models, patch.dict(os.environ):
            self.assertEqual(L.main(), 0)
            models.assert_not_called()
            self.assertEqual(os.environ['RELAY_SKIP_ASR_WARMUP'], '1')
            self.assertEqual(os.environ['HF_HUB_OFFLINE'], '1')

    def test_decode_or_model_failure_never_starts_service(self):
        code, relay, ports = self.run_main(1, False)
        self.assertEqual(code, 1)
        ports.assert_called_once()
        relay.app.run.assert_not_called()
        relay.start_beacon.assert_not_called()
        relay.start_store.assert_not_called()
        relay.start_auto_tick.assert_not_called()

    def test_port_conflict_is_checked_before_model_loading(self):
        with patch.object(sys, "argv", ["lan_start.py"]), \
             patch.object(L, "ensure_port_free", side_effect=RuntimeError("occupied")), \
             patch.object(L, "setup_config") as settings, patch.object(L, "find_models") as models:
            with self.assertRaisesRegex(RuntimeError, "occupied"):
                L.main()
        settings.assert_not_called()
        models.assert_not_called()

    def test_real_production_udp_reply_on_random_loopback_port(self):
        import app as relay
        ready = threading.Event()
        addresses = []
        created = []
        class BeaconSocket(socket.socket):
            sent = False
            def bind(inner, _):
                super().bind(("127.0.0.1", 0))
                inner.settimeout(3)
                addresses.append(inner.getsockname())
                ready.set()
            def sendto(inner, data, addr):
                result = super().sendto(data, addr)
                inner.sent = True
                return result
            def recvfrom(inner, *args):
                if inner.sent:
                    inner.close()
                    raise SystemExit()
                return super().recvfrom(*args)
        def spawn(*args, **kwargs):
            worker = threading.Thread(*args, **kwargs)
            created.append(worker)
            return worker
        sockets = SimpleNamespace(socket=BeaconSocket, AF_INET=socket.AF_INET, SOCK_DGRAM=socket.SOCK_DGRAM,
                                  SOL_SOCKET=socket.SOL_SOCKET, SO_REUSEADDR=socket.SO_REUSEADDR,
                                  SO_BROADCAST=socket.SO_BROADCAST)
        with patch.object(relay, "socket", sockets), \
             patch.object(relay, "threading", SimpleNamespace(Thread=spawn)), \
             patch.object(relay, "load_config", return_value={"port": 5000}), \
             patch.object(relay, "_beacon_started", False):
            self.assertTrue(relay.start_beacon())
            self.assertFalse(relay.start_beacon())
            self.assertTrue(ready.wait(3))
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
                peer.settimeout(3)
                peer.sendto(b"ARDP_DISCOVER", addresses[0])
                self.assertEqual(peer.recvfrom(256)[0], b"ARDP_IP:127.0.0.1:5000")
            created[0].join(3)
            self.assertFalse(created[0].is_alive())


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    unittest.main()
