"""Capture meaningful regressions without interacting with any installed boards."""
from pathlib import Path
import subprocess
import sys
sys.stdout.reconfigure(encoding="utf-8")
ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'AGENTS.md').is_file())
EVIDENCE = Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT / "flask_relay/timeline_validation"
tests = [
    ("上电归位与ESP缓存", "flask_relay/test_startup_home.py"),
    ("ESP真实转发", "ST32/AIAssistant/Tools/timeline_test/test_esp_raw.py"),
    ("调度与编码", "ST32/AIAssistant/Tools/timeline_test/run_tests.py"),
    ("MCU真实串口", "ST32/AIAssistant/Tools/esp_link_test/run_tests.py"),
    ("PWM平滑", "ST32/AIAssistant/Tools/servo_pwm_test/run_tests.py"),
    ("AI时间表", "flask_relay/test_motion_timeline.py"),
    ("管理员参数", "flask_relay/test_device_profile.py"),
    ("音频实际传输", "ST32/AIAssistant/Tools/audio_acceptance/test_esp_transport.py"),
    ("音频协议", "ST32/AIAssistant/Tools/audio_acceptance/run_tests.py"),
    ("OTA保护", "ST32/AIAssistant/Tools/ota/run_tests.py"),
    ("录音队列", "flask_relay/test_audio_queue.py"),
    ("录音上传状态", "flask_relay/test_audio_transport_status.py"),
    ("局域网启动", "flask_relay/test_lan_start.py"),
    ("网页部署", "flask_relay/test_deploy_web.py"),
    ("对话投递", "flask_relay/test_chat_deliver.py"),
    ("时间表浏览器", "flask_relay/test_timeline_browser.py"),
]
EVIDENCE.mkdir(exist_ok=True)
for label, script in tests:
    r=subprocess.run([sys.executable,str(ROOT/script)],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
    (EVIDENCE/(label+".log")).write_bytes(r.stdout)
    print(label+": "+("PASS" if r.returncode==0 else "FAILED"),flush=True)
    if r.returncode:
        print(r.stdout.decode("utf-8",errors="replace"))
        raise SystemExit(r.returncode)
