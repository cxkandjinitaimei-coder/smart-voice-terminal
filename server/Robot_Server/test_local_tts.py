"""旧TTS启动入口兼容测试：只启动局域网服务，保留进程退出码。

运行python -B -m unittest test_local_tts；子进程用桩，不启动服务。
停用合成接口另见test_text_only.py。
"""
import unittest
from pathlib import Path
from unittest.mock import patch
import start_local_tts

class LegacyLauncherTests(unittest.TestCase):
    def test_normal_lan_launcher_and_exit_status(self):
        here = Path(start_local_tts.__file__).resolve().parent
        for status in (0, 7):
            with self.subTest(status=status), patch.object(
                    start_local_tts.subprocess, 'call', return_value=status) as launch:
                self.assertEqual(start_local_tts.main(), status)
                launch.assert_called_once_with([
                    'powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                    '-File', str(here / 'start_lan.ps1')], cwd=here)

if __name__ == '__main__':
    unittest.main()
