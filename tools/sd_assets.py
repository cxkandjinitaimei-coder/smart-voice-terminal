"""sd_assets.py：把用户自备表情帧转换为板端 SD 播放格式。

使用：python tools/sd_assets.py <包含 BLINK/HAPPY 等目录的源目录> <输出目录>。
输入每个目录按自然数字顺序排列，输出 FACE/<名称>/0000.BMP、0000.R16 等。
依赖 Pillow；R16 为 512 字节头 + 160x160 小端 RGB565。静态听/普通表情可另指定。
仅写所选输出目录，不复制到 SD 卡、不修改驱动帧数、不分发原始美术素材。
"""
import argparse
from pathlib import Path
import re
import struct

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
IMAGE_SUFFIXES = {'.bmp', '.png', '.jpg', '.jpeg', '.webp'}


def encode(image):
    """上到下行序，小端 RGB565；与 face_anim 的 D565/512 偏移一致。"""
    image = image.convert('RGB').resize((160, 160), Image.Resampling.LANCZOS)
    raw = image.tobytes()
    pixels = [((raw[i] >> 3) << 11) | ((raw[i+1] >> 2) << 5) | (raw[i+2] >> 3)
              for i in range(0, len(raw), 3)]
    header = struct.pack('<4sIII', b'D565', 160, 160, 51200).ljust(512, b'\0')
    return image, header + struct.pack('<25600H', *pixels)


def natural(path):
    """把 2 排在 10 前；源目录文件顺序不依赖操作系统枚举顺序。"""
    return [int(s) if s.isdigit() else s.lower() for s in re.split(r'(\d+)', path.name)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--normal', type=Path, help='Optional default GUI face image')
    parser.add_argument('--listen', type=Path, help='Optional listening GUI face image')
    args = parser.parse_args()
    source, destination = args.source.resolve(), args.destination.resolve()
    if source == destination or source in destination.parents or destination in source.parents:
        parser.error('Source and destination must be separate, non-nested directories.')
    driver = (ROOT/'firmware/STM32_Controller/App/face_anim.c').read_text(encoding='utf-8')
    clips = re.findall(r'\{\s*"([A-Z]+)"\s*,\s*(\d+)u\s*\}', driver)
    exported = 0
    for name, count in clips:
        directory = source / name
        if not directory.is_dir():
            continue
        files = sorted((p for p in directory.iterdir() if p.suffix.lower() in IMAGE_SUFFIXES), key=natural)
        if len(files) != int(count):
            parser.error('%s requires %s frames, found %d; update clip metadata explicitly if needed.' % (name, count, len(files)))
        target = destination / 'FACE' / name
        target.mkdir(parents=True, exist_ok=True)
        for number, path in enumerate(files):
            with Image.open(path) as original:
                image, binary = encode(original)
            image.save(target / ('%04d.BMP' % number), format='BMP')
            (target / ('%04d.R16' % number)).write_bytes(binary)
            exported += 1
    for path, name in [(args.normal, 'GUINORM'), (args.listen, 'GUILIST')]:
        if path:
            with Image.open(path) as original:
                image, _ = encode(original)
            target = destination / 'FACE'
            target.mkdir(parents=True, exist_ok=True)
            image.save(target/(name+'.BMP'), format='BMP')
    if not exported and not (args.normal or args.listen):
        parser.error('No supported clip folders found; see docs/sd-assets.md.')
    print('Exported', exported, 'frames to', destination)


if __name__ == '__main__':
    main()
