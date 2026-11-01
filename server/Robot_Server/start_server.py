"""可迁移的局域网服务启动入口。

首次运行：``python start_server.py --init-config``，编辑生成的 config.json；
随后运行：``python start_server.py``。真实配置、模型和运行数据都留在本机。
"""

import argparse
import json
import os
from pathlib import Path

from runtime_config import create_config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--init-config",
        action="store_true",
        help="创建本地私有配置后退出，不启动服务",
    )
    args = parser.parse_args()
    here = Path(__file__).resolve().parent

    if args.init_config:
        path = create_config(here)
        print("本地配置已准备：", path)
        print("请在本机编辑 token 和服务参数，不要提交 config.json。")
        return 0

    config_path = here / "config.json"
    if not config_path.is_file():
        parser.error("请先运行 python start_server.py --init-config")
    config = json.loads(config_path.read_text(encoding="utf-8-sig"))
    if not isinstance(config.get("token"), str) or len(config["token"]) < 16:
        parser.error("config.json 的 token 至少需要 16 个字符")

    # 启动时只读取本地模型；模型下载必须由用户显式执行。
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    import app as service

    service.start_auto_tick()
    service.start_store()
    service.start_beacon()
    host = config.get("host", "0.0.0.0")
    port = int(config.get("port", 5000))
    print("Robot_Server", service.APP_VERSION, f"http://127.0.0.1:{port}/")
    print("局域网客户端使用本机局域网 IP；按 Ctrl+C 停止服务。")
    service.app.run(host=host, port=port, threaded=True, use_reloader=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
