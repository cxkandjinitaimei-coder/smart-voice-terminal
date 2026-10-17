"""start_server.py：可迁移的局域网服务启动入口。

使用：python start_server.py --init-config；编辑 config.json 后 python start_server.py。
依赖：requirements.txt；音频识别模型需显式下载，人脸模型也由独立工具下载。
边界：不读取旧工程、不部署云端、不打开串口；首次缺少模型不会在后台偷偷下载。
"""
import argparse
import json
import os
from pathlib import Path

from runtime_config import create_config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--init-config', action='store_true', help='Create private configuration without starting the service')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    if args.init_config:
        path = create_config(here)
        print('Private configuration ready:', path)
        print('Read the token locally and set the SAME token in ESP gateway_config.h. Do not commit config.json.')
        return 0
    config_path = here / 'config.json'
    if not config_path.is_file():
        parser.error('Run --init-config and edit config.json first.')
    config = json.loads(config_path.read_text(encoding='utf-8-sig'))
    if not isinstance(config.get('token'), str) or len(config['token']) < 16:
        parser.error('Set a non-empty private token (at least 16 characters).')
    # 启动时只读取本地模型；需要权重时先显式运行download_asr_model.py。
    os.environ.setdefault('HF_HUB_OFFLINE', '1')
    import app as service
    service.start_auto_tick()
    service.start_store()
    service.start_beacon()
    host, port = config.get('host', '0.0.0.0'), int(config.get('port', 5000))
    print('Robot_Server', service.APP_VERSION, 'http://127.0.0.1:%d/' % port)
    print('LAN clients use this PC\'s LAN IP. Keep this process running; Ctrl+C stops it.')
    service.app.run(host=host, port=port, threaded=True, use_reloader=False)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
