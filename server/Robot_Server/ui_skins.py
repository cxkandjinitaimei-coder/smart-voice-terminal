"""The skin manifest is the only registry; appearance never owns device logic."""
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
