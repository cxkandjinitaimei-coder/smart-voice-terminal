"""download_face_models.py：下载并按固定 SHA256 校验 YuNet/SFace 权重。

使用：python tools/download_face_models.py；需要网络，目标目录从脚本所在项目定位。
模型出处、提交及许可保留在 face_local/model_source.json 与 models 目录。
只下载模型，不打开摄像头、不上传人员照片、不调用 AI 或控制机器人。
"""
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    directory = ROOT / 'server/Robot_Server/face_local'
    meta = json.loads((directory/'model_source.json').read_text(encoding='utf-8'))
    for item in (meta, meta['recognition']):
        target = directory / 'models' / item['model']
        if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == item['model_sha256']:
            print('Verified cached model:', target.name)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix('.download')
        # The final name is published only after byte count and digest match.
        digest, size = hashlib.sha256(), 0
        with urllib.request.urlopen(item['download_url'], timeout=120) as response, temporary.open('wb') as out:
            while chunk := response.read(1024*1024):
                out.write(chunk)
                digest.update(chunk)
                size += len(chunk)
        if size != item['model_bytes'] or digest.hexdigest() != item['model_sha256']:
            temporary.unlink(missing_ok=True)
            raise RuntimeError('Model verification failed: ' + target.name)
        temporary.replace(target)
        print('Downloaded and verified:', target.name)


if __name__ == '__main__':
    main()
