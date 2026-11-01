"""Start an isolated LAN web copy, reusing the owner's existing config/models."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import sys
import sqlite3
import uuid
from contextlib import closing


def find_models(directory):
    override = os.environ.get("ASR_MODELS_DIR")
    candidates = [Path(override)] if override else []
    seed_hint = directory / 'lan_seed.json'
    if seed_hint.is_file():
        hint = json.loads(seed_hint.read_text(encoding='utf-8'))
        if hint.get('source_models'):
            candidates.append((directory / hint['source_models']).resolve())
    candidates += [parent / "语音识别/models" for parent in [directory, *directory.parents]]
    for path in candidates:
        if path.is_dir() and any(path.rglob("model.bin")):
            return path.resolve()
    raise RuntimeError("找不到本机 small 模型缓存。请设置 ASR_MODELS_DIR 为已有模型目录，勿重新下载服务器模型。")


def setup_config(directory, source=None):
    target = directory / "config.json"
    if target.exists():
        config = json.loads(target.read_text(encoding="utf-8-sig"))
    else:
        candidates = [Path(source)] if source else []
        seed_hint = directory / "lan_seed.json"
        if not source and seed_hint.is_file():
            hint = json.loads(seed_hint.read_text(encoding="utf-8"))
            candidates.append((directory / hint["source_config"]).resolve())
            candidates.extend((directory / name).resolve() for name in hint.get("fallback_configs", []))
        if not source:
            candidates += [p / "flask_relay/config.json" for p in directory.parents]
        seed = next((p for p in candidates if p.is_file() and p.resolve() != target.resolve()), None)
        if seed is None:
            raise RuntimeError("找不到原项目 config.json。请把原配置复制到本文件夹后重新启动。")
        config = json.loads(seed.read_text(encoding="utf-8-sig"))
        # Copy only settings, never previous recordings/queues/database. Do not
        # print token or provider keys, and never rewrite the source config.
        for name in ["audio_state.json", "audio_retention.json", "device_controls.json", "admin_device_profile.json", "idle_pet_settings.json"]:
            old = seed.parent / name
            if old.is_file() and not (directory / name).exists():
                shutil.copy2(old, directory / name)
        print("首次启动：已复用原项目的登录/AI配置，原文件未改。", flush=True)
    if not config.get("token"):
        raise RuntimeError("config.json 缺少登录口令，需与 ESP 的 API_TOKEN 一致。")
    changed = not target.exists() or config.get("host") != "0.0.0.0" or config.get("port") != 5000
    config["host"] = "0.0.0.0"
    config["port"] = 5000
    if changed:
        target.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return config


def lan_addresses():
    addresses = []
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # UDP connect selects a default route without sending a packet.
        sock.connect(("192.0.2.1", 9))
        addresses.append(sock.getsockname()[0])
    except OSError:
        pass
    finally:
        sock.close()
    try:
        addresses.extend(socket.gethostbyname_ex(socket.gethostname())[2])
    except OSError:
        pass
    return list(dict.fromkeys(ip for ip in addresses if ipaddress.ip_address(ip).is_private and
                             not ipaddress.ip_address(ip).is_loopback and not ip.startswith("169.254.")))


def import_camera_data(directory):
    """Import a previous local delivery once; never overwrite an existing gallery."""
    target = directory / 'camera_data'
    hint = directory / 'lan_seed.json'
    if target.exists() or not hint.is_file():
        return
    hints = json.loads(hint.read_text(encoding='utf-8'))
    candidates = [hints.get('source_camera_data')] + hints.get('fallback_camera_data', [])
    source = next(((directory / name).resolve() for name in candidates
                   if name and (directory / name).is_dir() and (directory / name).resolve() != target.resolve()), None)
    if source is None:
        return
    temporary = directory / ('.camera_import_' + uuid.uuid4().hex)
    shutil.copytree(source, temporary, ignore=shutil.ignore_patterns('camera.sqlite3*'))
    database = source / 'camera.sqlite3'
    if database.is_file():
        with closing(sqlite3.connect(database.as_uri() + '?mode=ro', uri=True)) as old_db:
            with closing(sqlite3.connect(temporary / 'camera.sqlite3')) as new_db:
                old_db.backup(new_db)
    temporary.rename(target)
    print('首次升级：已复用上一网页目录的人员记录与人脸库，原数据保留。', flush=True)


def ensure_port_free():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as check:
        try:
            check.bind(("0.0.0.0", 5000))
        except OSError as exc:
            raise RuntimeError("5000端口已有服务或被占用。请关闭之前的本地网页窗口再启动；没有停止其他进程。") from exc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--source-config")
    parser.add_argument("--camera-only", action="store_true", help="仅测试相机：跳过ASR模型预热，不自动下载模型")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    if not args.prepare_only:
        ensure_port_free()
    setup_config(here, args.source_config)
    import_camera_data(here)
    os.environ["HF_HUB_OFFLINE"] = "1"  # Cache is required; never download 460MB silently.
    if args.camera_only:
        os.environ['RELAY_SKIP_ASR_WARMUP'] = '1'
        # 仅跳过启动时预热；原来的录音/AI功能和已保存配置不被改写。
        # ASR若缺少本地依赖/模型，会在用户主动识别时明确报错。
        print("相机测试模式：跳过ASR预热，不下载语音模型。", flush=True)
    else:
        models = find_models(here)
        os.environ["ASR_MODELS_DIR"] = str(models)
        print("局域网模式：音频上传和识别在本机，未连接公网网页。", flush=True)
        print("small模型缓存：" + str(models), flush=True)
        import asr_check
        if asr_check.prepare(str(here)) != 0:
            return 1
        print("LAN_ASR_READY：本机 small / CPU / int8 解码与VAD检查通过。", flush=True)
    if args.prepare_only:
        return 0
    print("本机网页：http://127.0.0.1:5000/#test", flush=True)
    for ip in lan_addresses():
        print("同Wi-Fi网页：http://%s:5000/#test" % ip, flush=True)
    print("TCP5000网页/音频/OTA；UDP5001自动发现。先启动本窗口，再重启ESP。", flush=True)
    print("保持窗口打开；Ctrl+C停止服务。AI对话仍使用原配置的在线模型API。", flush=True)
    import app as relay
    import companion_runtime
    companion_runtime.prepare(relay)
    relay.start_auto_tick()
    relay.start_store()
    relay.start_beacon()
    relay.app.run(host="0.0.0.0", port=5000, threaded=True, use_reloader=False)
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, ValueError) as exc:
        print("LAN_START_FAILED: " + str(exc), flush=True)
        raise SystemExit(1)
