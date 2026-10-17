"""ui_skins.py：网页皮肤注册表与静态资源路径检查。

使用：页面读取 registry，由 skin-loader 载入已登记皮肤；不在这里存设备运行配置。
时序与边界：允许的资源限定在 static/skins；保留原界面和选项，不在整理源码时重做 UI。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent / 'static' / 'skins'


def registry():
    skins = [dict(id='original', name='原色 · 暖色陪伴', styles=[], bootstrap='',
                  storage_key='ui_theme_v1', default_mode='light')]
    for path in sorted(ROOT.glob('*/skin.json')):
        try:
            value = json.loads(path.read_text(encoding='utf-8-sig'))
            if not re.fullmatch(r'[a-z][a-z0-9-]{0,39}', value['id']) or value['id'] == 'original':
                continue
            item = {key: value[key] for key in ['id', 'name', 'storage_key', 'default_mode']}
            if item['default_mode'] not in ('dark', 'light'):
                continue
            for key in ['styles', 'bootstrap']:
                name = value[key]
                if not re.fullmatch(r'[A-Za-z0-9_-]+\.(css|js)', name) or not (path.parent / name).is_file():
                    raise ValueError('missing skin asset')
                item[key] = '/static/skins/' + path.parent.name + '/' + name
            item['styles'] = [item['styles']]
            if value.get('model_styles'):
                name = value['model_styles']
                if not re.fullmatch(r'[A-Za-z0-9_-]+\.css', name) or not (path.parent / name).is_file():
                    raise ValueError('missing model stylesheet')
                item['model_styles'] = '/static/skins/' + path.parent.name + '/' + name
            if re.fullmatch(r'#[0-9a-fA-F]{6}', value.get('model_background', '')):
                item['model_background'] = value['model_background']
            if not any(s['id'] == item['id'] for s in skins):
                skins.append(item)
        except (ValueError, KeyError, TypeError, OSError):
            continue
    return skins
