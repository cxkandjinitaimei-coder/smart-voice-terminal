"""运行时私有配置工具。

本模块只负责首次运行时创建本地配置和会话密钥，不携带任何真实凭据。
启动入口通过 :func:`create_config` 生成被 .gitignore 排除的 config.json；
服务导入时可通过 :func:`load_session_key` 获取本机随机会话签名密钥。
"""

import json
import os
import secrets
from pathlib import Path


def load_session_key(directory):
    """优先使用环境变量，否则在服务目录创建并读取本地随机密钥。"""
    configured = os.environ.get("ROBOT_SESSION_KEY")
    if configured:
        return configured

    path = Path(directory) / ".session_key"
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        pass
    else:
        with os.fdopen(fd, "w", encoding="ascii") as output:
            output.write(secrets.token_hex(32))

    value = path.read_text(encoding="ascii").strip()
    if len(value) < 32:
        raise RuntimeError("Invalid .session_key; repair it before starting the service.")
    return value


def create_config(directory):
    """从公开模板创建本地配置；已有配置保持不变。"""
    directory = Path(directory)
    target = directory / "config.json"
    if target.exists():
        return target

    template = directory / "config.example.json"
    config = json.loads(template.read_text(encoding="utf-8"))
    config["token"] = secrets.token_urlsafe(24)
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as output:
        json.dump(config, output, ensure_ascii=False, indent=2)
        output.write("\n")
    return target
