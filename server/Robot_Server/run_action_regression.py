"""Affected web regressions, isolated from real boards and cloud deployment."""
from pathlib import Path
import subprocess
import sys

ROOT=next(p for p in Path(__file__).resolve().parents if (p / 'AGENTS.md').is_file())
EVIDENCE=ROOT/'flask_relay/action_validation'
sys.stdout.reconfigure(encoding='utf-8')
EVIDENCE.mkdir(exist_ok=True)
for label,name in [('动作库实际接口','test_action_library.py'),('管理员档案','test_device_profile.py'),
        ('AI与时间表','test_motion_timeline.py'),('互动页浏览器','test_action_browser.py'),
        ('归位与时间表浏览器','test_timeline_browser.py'),('对话投递','test_chat_deliver.py'),
        ('局域网启动','test_lan_start.py'),('网页部署','test_deploy_web.py')]:
    result=subprocess.run([sys.executable,str(ROOT/'flask_relay'/name)],cwd=ROOT,
                          stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
    (EVIDENCE/(label+'.log')).write_bytes(result.stdout)
    print(label+': '+('PASS' if not result.returncode else 'FAILED'),flush=True)
    if result.returncode:
        print(result.stdout.decode('utf-8',errors='replace'));raise SystemExit(result.returncode)
