"""build_stm32.py：依次重编译 Keil 的普通应用、OTA 应用与 Bootloader。

使用：python tools/build_stm32.py --keil <UV4.exe> --fromelf <fromelf.exe>。
可用 KEIL_UV4/KEIL_FROMELF 环境变量。依赖 ARMCC5.06u7 和 STM32F4xx DFP。
仅编译并导出 BIN/HEX，不启动下载器、不擦写设备；普通应用不得当 OTA 镜像。
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/STM32_Controller'
PROJECTS = ['User/STM32_Controller.uvprojx', 'User/STM32_Controller_OTA.uvprojx',
            'Bootloader/STM32_Bootloader.uvprojx']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--keil', default=os.environ.get('KEIL_UV4') or shutil.which('UV4.exe'))
    parser.add_argument('--fromelf', default=os.environ.get('KEIL_FROMELF') or shutil.which('fromelf.exe'))
    args = parser.parse_args()
    if not args.keil or not Path(args.keil).is_file():
        parser.error('Provide --keil or KEIL_UV4 pointing to UV4.exe')
    if not args.fromelf or not Path(args.fromelf).is_file():
        parser.error('Provide --fromelf or KEIL_FROMELF pointing to fromelf.exe')
    subprocess.run([os.sys.executable, str(ROOT/'tools/set_version.py'), '--check'], check=True)
    logs = ROOT / '.build'
    logs.mkdir(exist_ok=True)
    for name in PROJECTS:
        project = FIRMWARE / name
        tree = ET.parse(project)
        target = tree.find('.//TargetName').text
        log = logs / (target + '.log')
        # uVision return 1 means warnings; validate both process status and final log.
        result = subprocess.run([args.keil, '-r', str(project), '-t', target, '-o', str(log)],
            timeout=600, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        text = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
        print(text)
        if result.returncode not in (0, 1) or '0 Error(s)' not in text:
            raise SystemExit('Build failed: ' + target)
        directory = tree.find('.//OutputDirectory').text.replace('\\', '/')
        output = project.parent / directory / tree.find('.//OutputName').text
        subprocess.run([args.fromelf, '--bin', '--output', str(output.with_suffix('.bin')),
                        str(output.with_suffix('.axf'))], check=True, timeout=60)
        print('BIN:', output.with_suffix('.bin'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
