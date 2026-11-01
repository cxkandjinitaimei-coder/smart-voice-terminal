"""兼容旧启动入口：启动局域网文字/动作服务，不启动语音合成进程。

保留文件名以免已有快捷方式失效；环境检查、ASR 和网页启动统一交给
同目录 start_lan.ps1。运行：python -B start_local_tts.py。
"""
from pathlib import Path
import subprocess


def main():
    here = Path(__file__).resolve().parent
    return subprocess.call(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                            "-File", str(here / "start_lan.ps1")], cwd=here)


if __name__ == "__main__":
    raise SystemExit(main())
