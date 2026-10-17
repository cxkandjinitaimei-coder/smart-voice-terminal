"""runtime_config.py：私有服务配置与会话签名密钥。

使用：start_server 在首次运行时创建 config.json；app 导入时调用 load_session_key。
配置不会从其他工程或历史交付自动复制，避免把私人密钥写入公开目录。
时序：用独占创建避免两个启动进程产生不同签名；读取已有文件不更换登录密钥。
"""
import json
import os
from pathlib import Path
import secrets


def load_session_key(directory):
    """优先环境变量，其次服务本地私有文件；返回前验证非空。"""
    configured = os.environ.get('ROBOT_SESSION_KEY')
    if configured:
        return configured
    path = Path(directory) / '.session_key'
    try:
        # O_EXCL makes the creator unique; no shared constant is distributed.
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        pass
    else:
        with os.fdopen(fd, 'w', encoding='ascii') as out:
            out.write(secrets.token_hex(32))
    value = path.read_text(encoding='ascii').strip()
    if len(value) < 32:
        raise RuntimeError('Invalid .session_key; stop other service processes before repairing it.')
    return value


def create_config(directory):
    """从公开模板生成私有配置；已有文件保留，生成的随机口令不打印到日志。"""
    directory = Path(directory)
    target = directory / 'config.json'
    if target.exists():
        return target
    config = json.loads((directory / 'config.example.json').read_text(encoding='utf-8'))
    config['token'] = secrets.token_urlsafe(24)
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8') as out:
        json.dump(config, out, ensure_ascii=False, indent=2)
        out.write('\n')
    return target
