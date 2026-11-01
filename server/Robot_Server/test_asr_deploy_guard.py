"""Run the actual remote Bash preflight with isolated command substitutes."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

from test_deploy_web import BASH, HERE, run_case


def write_script(path, source):
    path.write_text("#!/bin/sh\n" + source, encoding="utf-8", newline="\n")
    path.chmod(0o755)


def main():
    with tempfile.TemporaryDirectory(prefix="asr-deploy-guard-", dir=HERE) as temporary:
        root = Path(temporary).resolve()
        source = (HERE / "deploy_web.ps1").read_bytes()
        result, spy = run_case(root, source, "capture")
        assert result.returncode == 0
        payload = (spy / "payload.sh").read_text(encoding="utf-8")
        for name, imports, initial_decoder, installed_decoder, pip_exit, expect_pip, expected_exit in [
                ("same_requirements_missing_dependency", 1, 0, 0, 0, True, 27),
                ("same_requirements_broken_decoder_repaired", 0, 1, 0, 0, True, 27),
                ("same_requirements_model_failure", 0, 0, 0, 0, False, 27),
                ("pip_failure", 1, 0, 0, 19, True, 19),
                ("decoder_still_broken_after_install", 0, 1, 1, 0, True, 28)]:
            base = root / name
            drop, target, backup, venv, commands = [base / part for part in ("drop", "target", "backup", "venv", "commands")]
            for path in (drop, target, backup, venv / "bin", commands):
                path.mkdir(parents=True)
                assert path.resolve().is_relative_to(root), "All shell file operations must stay inside this test directory"
            filenames = re.search(r'^FILES="([^"]+)"', payload, re.M).group(1).split()
            for file in filenames:
                (drop / file).write_text("# uploaded fixture\n")
                (target / file).write_text("# original service\n")
            for path in (drop, target):
                for part in ("static", "templates"):
                    (path / part).mkdir()
                    (path / part / "keep.txt").write_text("keep")
            requirements = "flask\nrequests\nfaster-whisper\n"
            (drop / "requirements.txt").write_text(requirements)
            (target / "requirements.txt").write_text(requirements)
            originals = {p.relative_to(target): p.read_bytes() for p in target.rglob("*") if p.is_file()}
            write_script(commands / "chown", "exit 0\n")
            write_script(commands / "sudo", 'test "$1" = "-u" && shift 2\nexec "$@"\n')
            write_script(venv / "bin" / "python", '''case "$1" in
-m) if test "$2" = pip; then
      echo pip >> "$CALL_LOG"
      case "$*" in *--only-binary=av*) ;; *) exit 99;; esac
      exit "$PIP_EXIT"
    fi
    exit 0;;
-c) exit "$IMPORT_EXIT";;
esac
if test "$2" = --dependencies; then
    echo decoder >> "$CALL_LOG"
    if grep -q '^pip$' "$CALL_LOG"; then exit "$DECODER_AFTER"; fi
    exit "$DECODER_INITIAL"
fi
echo prepare >> "$CALL_LOG"
exit 1
''')
            adjusted = payload
            for key, path in [("DROP", drop), ("TARGET", target), ("BACKUP", backup), ("VENV", venv)]:
                adjusted = re.sub(r'^' + key + r'="[^"]+"', lambda m: key + '="' + path.as_posix() + '"', adjusted, count=1, flags=re.M)
            script = base / "deploy.sh"
            shell_commands = "/" + commands.drive[0].lower() + commands.as_posix()[2:]
            script.write_text('PATH="' + shell_commands + ':$PATH"\nexport PATH\n' + adjusted, encoding="utf-8", newline="\n")
            log = base / "calls.txt"
            env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"],
                       IMPORT_EXIT=str(imports), PIP_EXIT=str(pip_exit),
                       DECODER_INITIAL=str(initial_decoder), DECODER_AFTER=str(installed_decoder),
                       CALL_LOG=log.as_posix())
            run = subprocess.run([str(BASH), "--noprofile", "--norc", str(script)],
                                 cwd=base, env=env, capture_output=True, timeout=20)
            assert run.returncode == expected_exit, run.stdout.decode(errors="replace") + run.stderr.decode(errors="replace")
            assert originals == {p.relative_to(target): p.read_bytes() for p in target.rglob("*") if p.is_file()}
            calls = log.read_text().splitlines() if log.exists() else []
            assert ("pip" in calls) == expect_pip, calls
            assert ("prepare" in calls) == (not pip_exit and not installed_decoder), calls
            assert b"DEPLOY_OK" not in run.stdout
            print("PASS", name, "actual remote preflight exits before replacing/stopping service")


if __name__ == "__main__":
    main()
