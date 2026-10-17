"""run_checks.py：在临时副本运行服务回归，避免污染源码或用户运行数据。

使用：python tools/run_checks.py；--only test_name 可重复选择模块。
每个测试独立 Python 进程，服务源码复制到临时目录，网络/AI/模型由测试替身或本地数据代替。
安装 Robot_Server requirements 后运行；可选解码依赖检查会真实解码临时 WAV。
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', action='append', default=[])
    parser.add_argument('--face-assets', type=Path, help='Optional public opencv_lena.jpg/opencv_messi5.jpg fixture directory')
    parser.add_argument('--face-models', type=Path, help='Optional verified YuNet/SFace weights for real-model tests')
    args = parser.parse_args()
    tests = sorted((ROOT/'tests/server').glob('test_*.py'))
    if args.only:
        tests = [p for p in tests if p.stem in args.only]
        if len(tests) != len(set(args.only)):
            parser.error('Unknown or duplicate --only test name')
    environment = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONDONTWRITEBYTECODE='1',
                       HF_HUB_OFFLINE='1', ROBOT_PUBLIC_SOURCE=str(ROOT))
    if bool(args.face_assets) != bool(args.face_models):
        parser.error('--face-assets and --face-models must be provided together')
    environment.pop('ROBOT_FACE_TEST_ASSETS', None)
    if args.face_assets:
        environment['ROBOT_FACE_TEST_ASSETS'] = str(args.face_assets.resolve())
    failures = []
    with tempfile.TemporaryDirectory(prefix='robot-public-checks-') as temporary:
        directory = Path(temporary)
        service = directory/'Robot_Server'
        shutil.copytree(ROOT/'server/Robot_Server', service,
                        ignore=shutil.ignore_patterns('__pycache__', 'config.json', '.session_key',
                            '*.db*', '*.sqlite3*', '*.log', '*.onnx', 'models', 'recordings',
                            'camera_data', 'firmware', 'firmware_f407', 'logs'))
        environment['ASR_MODELS_DIR'] = str(service/'models/asr')
        for p in (ROOT/'tests/server').glob('test_*.py'):
            shutil.copy2(p, service/p.name)
        if args.face_models:
            (service/'face_local/models').mkdir(parents=True, exist_ok=True)
            for filename in ('face_detection_yunet_2023mar.onnx', 'face_recognition_sface_2021dec.onnx'):
                shutil.copy2(args.face_models/filename, service/'face_local/models'/filename)
        # Tests have explicit auth fixtures. A private test configuration keeps imports independent.
        subprocess.run([sys.executable, str(service/'start_server.py'), '--init-config'],
                       check=True, env=environment, cwd=service)
        for test in tests:
            print('\nRUN', test.stem, flush=True)
            result = subprocess.run([sys.executable, str(service/test.name)], cwd=service,
                                    env=environment, timeout=180)
            if result.returncode:
                failures.append(test.stem)
    print('\nChecks:', len(tests)-len(failures), 'passed,', len(failures), 'failed')
    if failures:
        raise SystemExit('Failed: ' + ', '.join(failures))


if __name__ == '__main__':
    main()
