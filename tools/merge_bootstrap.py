"""merge_bootstrap.py：合并 Bootloader 和 Slot A 应用 HEX，供首次有线写入。

使用：python tools/merge_bootstrap.py <Bootloader.hex> <OTA应用.hex> <输出.hex>。
仅接受 Boot 在 S0、应用在 Slot A 的数据记录；逐条校验 Intel HEX，不填充元数据。
本工具不连接设备；已有旧元数据的板子首次迁移前必须按 docs/flash-and-ota.md 清理。
"""
import argparse
from pathlib import Path


def read_hex(path, low, high):
    """校验校验和、扩展地址和范围；保留稀疏地址，不合成未知内容。"""
    result, base, eof = {}, 0, False
    for line in path.read_text(encoding='ascii').splitlines():
        if not line:
            continue
        if eof or not line.startswith(':'):
            raise ValueError('Unexpected Intel HEX line')
        raw = bytes.fromhex(line[1:])
        if len(raw) < 5 or len(raw) != raw[0] + 5 or sum(raw) & 255:
            raise ValueError('Invalid Intel HEX length/checksum')
        address, kind, data = int.from_bytes(raw[1:3], 'big'), raw[3], raw[4:-1]
        if kind == 0:
            for offset, byte in enumerate(data):
                absolute = base + address + offset
                if not low <= absolute < high or (absolute in result and result[absolute] != byte):
                    raise ValueError('Out of range or conflicting HEX data')
                result[absolute] = byte
        elif kind == 1 and len(data) == 0:
            eof = True
        elif kind == 4 and len(data) == 2:
            base = int.from_bytes(data, 'big') << 16
        elif kind == 2 and len(data) == 2:
            base = int.from_bytes(data, 'big') << 4
        elif kind == 5 and len(data) == 4:
            pass  # CPU starts through the hardware reset vector, not an Intel start record.
        else:
            raise ValueError('Unsupported Intel HEX record')
    if not eof or not result:
        raise ValueError('Missing EOF or image data')
    return result


def record(kind, address, data):
    raw = bytes([len(data)]) + address.to_bytes(2, 'big') + bytes([kind]) + data
    return ':' + (raw + bytes([-sum(raw) & 255])).hex().upper()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('boot', type=Path)
    parser.add_argument('app', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.boot.resolve(), args.app.resolve()):
        parser.error('Output must not overwrite an input HEX.')
    boot = read_hex(args.boot, 0x08000000, 0x08004000)
    application = read_hex(args.app, 0x08008000, 0x08040000)
    for image, base in [(boot, 0x08000000), (application, 0x08008000)]:
        if any(base+i not in image for i in range(8)):
            parser.error('Missing vector table at expected base.')
    combined = boot | application
    addresses, lines, index, segment = sorted(combined), [], 0, None
    while index < len(addresses):
        address = addresses[index]
        if address >> 16 != segment:
            segment = address >> 16
            lines.append(record(4, 0, segment.to_bytes(2, 'big')))
        data = bytearray([combined[address]])
        index += 1
        while index < len(addresses) and len(data) < 16 and addresses[index] == address+len(data) and addresses[index] >> 16 == segment:
            data.append(combined[addresses[index]])
            index += 1
        lines.append(record(0, address & 65535, bytes(data)))
    lines.append(record(1, 0, b''))
    args.output.write_text('\n'.join(lines)+'\n', encoding='ascii')
    print('Merged Boot + Slot A:', args.output, '(metadata NOT included)')


if __name__ == '__main__':
    main()
