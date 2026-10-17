"""set_version.py：从根 VERSION 同步 F407、ESP 和网页的公开版本。

使用：编辑 VERSION 后在任意目录运行 python <本脚本路径>；--check 仅检查不修改。
版本必须是 major.minor.patch。只更新显示/发布标记，不重新分区、不提交 Git、不烧录。
目录定位基于脚本路径，项目根可改名；Arduino 草图文件与其目录仍须同名。
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def expected():
    """列出由单一版本源产生的内容；与业务配置及设备 ID 分离。"""
    version = (ROOT / 'VERSION').read_text(encoding='ascii').strip()
    if not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)', version):
        raise ValueError('VERSION must be major.minor.patch')
    return version, {
        'firmware/STM32_Controller/User/app_version.h': '''/**
 * @file app_version.h
 * 用途：F407 屏幕和串口共用的应用版本；由根 VERSION 生成。
 * 使用：先改 VERSION，再运行 tools/set_version.py；不要单独修改本文件。
 * 边界：版本文本不改变 Flash 布局，也不代表实板验证已经完成。
 */
#ifndef APP_VERSION_H
#define APP_VERSION_H
#define APP_FW_VERSION "%s"
#endif
''' % version,
        'server/Robot_Server/release_version.py': '''"""release_version.py：网页及识别模块的公开版本常量。

使用：从根 VERSION 经 tools/set_version.py 生成，模板由 app 的 context_processor 注入。
版本用于展示和静态资源缓存，不包含产品名、私有服务地址或设备身份。
"""
APP_VERSION = "%s"
''' % version,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    version, files = expected()
    esp = ROOT / 'firmware/ESP_Gateway/firmware_version.h'
    content = esp.read_text(encoding='utf-8')
    # Preserve the detailed file header already reviewed for ESP8266.
    content, n = re.subn(r'(#define\s+ESP_FW_VERSION\s+)"[^"]+"', r'\g<1>"'+version+'"', content)
    if n != 1:
        raise ValueError('Expected exactly one ESP_FW_VERSION')
    content, n = re.subn(r'(#define\s+ESP_BUILD_ID\s+)"[^"]+"', r'\g<1>"ESP_Gateway-'+version+'"', content)
    if n != 1:
        raise ValueError('Expected exactly one ESP_BUILD_ID')
    files[str(esp.relative_to(ROOT))] = content
    trainer = ROOT / 'server/Robot_Server/static/page-trainer.js'
    js = trainer.read_text(encoding='utf-8')
    files[str(trainer.relative_to(ROOT))] = re.sub(r'(motion-trainer\.html\?v=)[\d.]+', r'\g<1>'+version, js)
    changed = []
    for name, wanted in files.items():
        path = ROOT / name
        actual = path.read_text(encoding='utf-8-sig') if path.exists() else ''
        if actual != wanted:
            changed.append(name)
            if not args.check:
                encoding = 'utf-8-sig' if path.suffix == '.h' and 'STM32_Controller' in path.parts else 'utf-8'
                path.write_text(wanted, encoding=encoding, newline='\n')
    if args.check and changed:
        raise SystemExit('Version files need synchronization: ' + ', '.join(changed))
    print('Version', version, 'checked' if args.check else 'synchronized')


if __name__ == '__main__':
    main()
