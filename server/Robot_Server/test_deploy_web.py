"""Exercise Windows PowerShell deployment with native tool spies, never a server.

Actual Git Bash parses the exact script bytes. The spies never run the payload.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'AGENTS.md').is_file())
BASH = Path(r"D:\AAAA粤嵌科技\Git\Git\bin\bash.exe")

SPY = r"""
import json, os
from pathlib import Path
import subprocess, sys
base = Path(os.environ["DEPLOY_SPY"])
kind = sys.argv[1]
args = sys.argv[2:]
if kind == "scp":
    assert args[:2] == ["-q", "-r"], args
    stage = Path(args[-2])
    script = stage / "deploy_remote.sh"
    (base / "scp.json").write_text(json.dumps(args), encoding="utf-8")
    if script.exists():
        (base / "payload.sh").write_bytes(script.read_bytes())
    sys.exit(int(os.environ.get("FAKE_SCP_EXIT", "0")))
assert kind == "ssh"
(base / "ssh.json").write_text(json.dumps(args), encoding="utf-8")
incoming = sys.stdin.buffer.read()
(base / "stdin.bin").write_bytes(incoming)
if args[-1] == "bash -s":
    (base / "payload.sh").write_bytes(incoming)
else:
    assert not incoming, "Deployment must not use the PowerShell stdin pipeline"
    assert args[-1].startswith("bash -n '/tmp/ArdProject_web_"), args
    assert "' && bash '" in args[-1] and args[-1].endswith("/deploy_remote.sh'"), args
payload = base / "payload.sh"
assert payload.exists()
data = payload.read_bytes()
result = subprocess.run([os.environ["DEPLOY_BASH"], "--noprofile", "--norc", "+o", "igncr", "-n"], input=data,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(base / "bash.txt").write_bytes(result.stdout)
if result.returncode:
    sys.stdout.buffer.write(result.stdout)
    sys.exit(result.returncode)
assert b"\r" not in data and not data.startswith(b"\xef\xbb\xbf"), "Bash script must be UTF-8/LF without BOM"
assert data.startswith(b"set -eu\n") and b"device_controls.py" in data and b"audio_archive.py" in data and b"idle_pet.py" in data and b"board_pose.py" in data
sys.exit(int(os.environ.get("FAKE_SSH_EXIT", "0")))
"""


def run_case(directory, source, name, scp_exit=0, ssh_exit=0):
    case = directory / name
    source_dir = case / "网页 更新包"
    spy_dir = case / "spy"
    for path in (source_dir, spy_dir):
        path.mkdir(parents=True)
    (source_dir / "deploy_web.ps1").write_bytes(source)
    for filename in ("app.py", "camera_workspace.py", "local_faces.py", "ui_skins.py", "ai_relay.py", "analytics.py", "asr.py", "asr_check.py", "audio_rx.py",
                     "audio_archive.py", "audio_retention.py", "device_controls.py", "device_profile.py", "servo_contract.py", "motion_timeline.py", "idle_pet.py", "board_pose.py",
                     "lan_discovery.py", "f407_ota_relay.py", "iap_pull.py", "ota_console.py", "store.py", "requirements.txt"):
        (source_dir / filename).write_text("# isolated deployment fixture\n", encoding="ascii")
    for part in ("static", "templates", "face_local"):
        (source_dir / part).mkdir()
        (source_dir / part / "fixture.txt").write_text("fixture", encoding="ascii")
    (source_dir / "face_local/models").mkdir()
    helper = spy_dir / "native_spy.py"
    helper.write_text(SPY, encoding="utf-8")
    for kind in ("scp", "ssh"):
        (spy_dir / (kind + ".cmd")).write_text(
            '@echo off\n"%s" "%s" %s %%*\nexit /b %%ERRORLEVEL%%\n' % (sys.executable, helper, kind),
            encoding="ascii", newline="\r\n")
    env = dict(os.environ, PATH=str(spy_dir) + os.pathsep + os.environ["PATH"],
               DEPLOY_SPY=str(spy_dir), DEPLOY_BASH=str(BASH),
               FAKE_SCP_EXIT=str(scp_exit), FAKE_SSH_EXIT=str(ssh_exit))
    result = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                             "-File", str(source_dir / "deploy_web.ps1")],
                            env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=30)
    (case / "powershell.log").write_bytes(result.stdout)
    stage = json.loads((spy_dir / "scp.json").read_text(encoding="utf-8"))[-2]
    assert not Path(stage).exists(), "Local staging was not cleaned"
    return result, spy_dir


def main(evidence):
    assert BASH.exists(), "Git Bash needed for the actual parser regression"
    source = (HERE / "deploy_web.ps1").read_text(encoding="utf-8-sig")
    with tempfile.TemporaryDirectory(prefix="deploy-web-test-") as temporary:
        directory = Path(temporary)
        archived = ROOT / "_归档/网页部署脚本/20261007_04_CRLF原始/活工程_deploy_web.ps1"
        old = archived.read_text(encoding="utf-8-sig")
        result, spy = run_case(directory, b"\xef\xbb\xbf" + old.replace("\n", "\r\n").encode(), "old_crlf")
        assert result.returncode != 0 and b"\r" in (spy / "stdin.bin").read_bytes()
        print("PASS negative baseline: Windows PowerShell sends CRLF stdin; strict byte check rejects it")
        for name, newline in (("fixed_crlf", "\r\n"), ("fixed_lf", "\n")):
            result, spy = run_case(directory, b"\xef\xbb\xbf" + source.replace("\n", newline).encode(), name)
            assert result.returncode == 0, result.stdout.decode(errors="replace")
            assert not (spy / "stdin.bin").read_bytes()
            print("PASS", name, "Windows PowerShell 5.1, exact LF/no-BOM script uploaded, actual Bash -n")
        result, spy = run_case(directory, b"\xef\xbb\xbf" + source.encode(), "scp_failure", scp_exit=17)
        assert result.returncode != 0 and not (spy / "ssh.json").exists()
        result, spy = run_case(directory, b"\xef\xbb\xbf" + source.encode(), "ssh_failure", ssh_exit=24)
        assert result.returncode != 0 and b"Cloud update failed (ssh exit 24)" in result.stdout
        print("PASS SCP failure skips SSH; SSH failure returns error and never reports success; local staging cleanup")
        if evidence:
            destination = Path(evidence)
            destination.mkdir(parents=True, exist_ok=True)
            for name in ("old_crlf", "fixed_crlf", "fixed_lf"):
                for filename in ("bash.txt", "stdin.bin", "payload.sh", "scp.json", "ssh.json"):
                    shutil.copy2(directory / name / "spy" / filename,
                                 destination / (name + "_" + filename))
            (destination / "README.txt").write_text(
                "Native Windows PowerShell + native SCP/SSH spies + actual Git Bash syntax parser.\n"
                "Old code sends CRLF stdin and fails the byte guard. New code uploads LF bytes and passes with CRLF or LF PS1.\n"
                "Spies only capture arguments and run bash -n; no server access or payload execution.\n",
                encoding="ascii")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence")
    main(parser.parse_args().evidence)
