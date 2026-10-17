"""test_public_config.py：独立公开源码的配置、命名、版本和网页入口回归。

使用：tools/run_checks.py 在临时服务副本运行，不读取真实配置或启动后台网络服务。
覆盖：随机私有口令、稳定会话密钥、管理员角色保留、默认角色命名和Jinja资源版本。
"""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import app as A
import ai_relay
from release_version import APP_VERSION
from runtime_config import create_config, load_session_key


class PublicConfigurationTests(unittest.TestCase):
    def test_initialization_is_private_and_does_not_overwrite_user_config(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            example = Path(__file__).parent/'config.example.json'
            (root/'config.example.json').write_bytes(example.read_bytes())
            target = create_config(root)
            first = target.read_bytes()
            cfg = json.loads(first)
            self.assertGreaterEqual(len(cfg['token']), 16)
            self.assertFalse(any(p['enabled'] or p['keys'] for p in cfg['providers'].values()))
            self.assertEqual(create_config(root), target)
            self.assertEqual(target.read_bytes(), first)
            self.assertEqual(json.loads(example.read_text(encoding='utf-8'))['token'], '')

    def test_session_key_is_stable_private_and_environment_override_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {}, clear=True):
            first = load_session_key(directory)
            self.assertEqual(len(first), 64)
            self.assertEqual(first, load_session_key(directory))
            with patch.dict(os.environ, {'ROBOT_SESSION_KEY': 'temporary-test-secret-value'}):
                self.assertEqual(load_session_key(directory), 'temporary-test-secret-value')
            self.assertEqual(first, load_session_key(directory))

    def test_persona_and_name_have_no_implicit_release_override(self):
        source = {'display_name':'自定义名字', 'persona':'管理员写入的角色',
                  'companion_persona_revision':'older-owner-revision'}
        self.assertEqual(ai_relay.companion_config(source), source)
        blank = {'display_name':'自定义名字'}
        result = ai_relay.companion_config(blank)
        self.assertIn('自定义名字', result['persona'])
        self.assertEqual(blank, {'display_name':'自定义名字'})
        self.assertNotIn('persona_before_20261016', result)

    def test_real_login_home_and_asset_versions_without_models(self):
        cfg = {'token':'public-config-fixture-only'}
        patches = [patch.object(A,'load_config',return_value=cfg),
                   patch.object(A,'start_auto_tick'), patch.object(A,'start_store'),
                   patch.object(A,'start_beacon'), patch.object(A.asr,'warmup')]
        for handle in patches:
            handle.start(); self.addCleanup(handle.stop)
        client = A.app.test_client()
        self.assertEqual(client.get('/').status_code, 302)
        self.assertEqual(client.post('/login',data={'token':'wrong'}).status_code, 200)
        self.assertEqual(client.post('/login',data=cfg).status_code, 302)
        response = client.get('/')
        self.assertEqual(response.status_code, 200)
        self.assertIn(APP_VERSION, response.text)
        self.assertIn('motion-trainer-frame', response.text)
        self.assertNotIn('{{ app_version }}', response.text)
        for path in ['/static/app.js','/static/page-test.js','/static/motion-trainer.html',
                     '/static/vendor/three.module.min.js','/static/servo-contract.json']:
            with client.get(path) as asset:
                self.assertEqual(asset.status_code, 200)


if __name__ == '__main__':
    unittest.main(verbosity=2)
