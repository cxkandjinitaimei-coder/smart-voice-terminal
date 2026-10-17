"""test_public_tools.py：SD格式、首次安装HEX和版本同步工具的边界回归。

使用：tools/run_checks.py；环境变量 ROBOT_PUBLIC_SOURCE 指向只读源码，数据均在临时目录。
覆盖：RGB565字节序/行序，HEX校验及越界拒绝，语义版本同时更新三端且保留ESP文件头。
不连接设备、不下载模型、不触碰用户原素材。
"""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

ROOT = Path(os.environ['ROBOT_PUBLIC_SOURCE'])


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT/'tools'/(name+'.py'))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class PublicToolsTests(unittest.TestCase):
    def test_sd_pixels_top_down_little_endian_and_sector_header(self):
        assets = module('sd_assets')
        image = Image.new('RGB', (160,160), 'black')
        image.putpixel((0,0),(255,0,0));image.putpixel((159,159),(0,0,255))
        converted, binary = assets.encode(image)
        self.assertEqual(len(binary),51712)
        self.assertEqual(struct.unpack_from('<4sIII',binary),(b'D565',160,160,51200))
        self.assertEqual(binary[16:512],bytes(496))
        self.assertEqual(binary[512:514],b'\x00\xf8')
        self.assertEqual(binary[-2:],b'\x1f\x00')

    def test_hex_boundary_checksum_and_eof_errors(self):
        helper = module('merge_bootstrap')
        with tempfile.TemporaryDirectory() as directory:
            file=Path(directory)/'test.hex'
            lines=[helper.record(4,0,b'\x08\x00'),helper.record(0,0x8000,b'\x10\x20'),helper.record(1,0,b'')]
            file.write_text('\n'.join(lines)+'\n',encoding='ascii')
            self.assertEqual(helper.read_hex(file,0x08008000,0x08040000),{0x08008000:16,0x08008001:32})
            with self.assertRaises(ValueError):helper.read_hex(file,0x08000000,0x08004000)
            file.write_text('\n'.join(lines[:-1])+'\n',encoding='ascii')
            with self.assertRaises(ValueError):helper.read_hex(file,0x08008000,0x08040000)
            lines[1]=lines[1][:-2]+'00';file.write_text('\n'.join(lines)+'\n',encoding='ascii')
            with self.assertRaises(ValueError):helper.read_hex(file,0x08008000,0x08040000)

    def test_version_update_is_portable_complete_and_preserves_comments(self):
        with tempfile.TemporaryDirectory(prefix='renamed source ') as directory:
            root=Path(directory)
            files=['VERSION','tools/set_version.py','firmware/ESP_Gateway/firmware_version.h',
                   'firmware/STM32_Controller/User/app_version.h','server/Robot_Server/release_version.py',
                   'server/Robot_Server/static/page-trainer.js']
            for name in files:
                target=root/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,target)
            before=(root/files[2]).read_text(encoding='utf-8').split('#ifndef')[0]
            (root/'VERSION').write_text('2.3.4\n',encoding='ascii')
            script=root/'tools/set_version.py'
            checked=subprocess.run([sys.executable,str(script),'--check'],capture_output=True)
            self.assertNotEqual(checked.returncode,0)
            subprocess.run([sys.executable,str(script)],check=True,capture_output=True)
            subprocess.run([sys.executable,str(script),'--check'],check=True,capture_output=True)
            for name in files[2:]:self.assertIn('2.3.4',(root/name).read_text(encoding='utf-8-sig'))
            self.assertEqual((root/files[2]).read_text(encoding='utf-8').split('#ifndef')[0],before)
            self.assertTrue((root/'firmware/STM32_Controller/User/app_version.h').read_bytes().startswith(b'\xef\xbb\xbf'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
