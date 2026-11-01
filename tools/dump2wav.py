#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dump2wav.py：把 F407 /dump 的十六进制 PCM 还原成 WAV。

使用：python tools/dump2wav.py <串口日志.txt>；可选 pyserial 支持直接串口录制。
格式：[DUMP] BEGIN 给出样本/字节数，数据为十六进制文本，END 结束；长度不齐报告失败。
依赖：文件模式仅 Python 标准库；串口模式需 pyserial，默认 USART2/115200。
边界：这是短录音调试工具；网络流式音频使用 audio_rx，不经本脚本。
"""

import argparse
import array
import datetime
import sys
import time
import wave

# Windows 控制台默认不是 UTF-8，中文会直接 UnicodeEncodeError
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

MARK = b"[DUMP] BEGIN "
HEXSET = frozenset(b"0123456789abcdefABCDEF")
BLANK = frozenset(b" \t\r\n")

# 解析失败时留一句"卡在哪"，好让人知道怎么修
LAST_DIAG = ""


# ==================================================================
# 找 dump：从 buf 里找**最后一个完整**的
# ==================================================================
def find_dump(buf):
    """返回 (样本数, 字节数, 原始PCM字节) ；没找到返回 None。

    取"最后一个完整的"是为了：一次会话里多发了几个 /dump 时，
    拿到的是最新的那份，而不是最老的。
    """
    global LAST_DIAG
    LAST_DIAG = ""

    pos = 0
    found = None
    while True:
        i = buf.find(MARK, pos)
        if i < 0:
            if not LAST_DIAG:
                LAST_DIAG = "整个日志里没有 [DUMP] BEGIN 这一行"
            break

        j = buf.find(b"\n", i)
        if j < 0:
            LAST_DIAG = "[DUMP] BEGIN 那一行还没收完（没见到行尾）"
            break

        head = buf[i + len(MARK):j].split()
        try:
            n_samples = int(head[0])
            n_bytes = int(head[1])
        except (IndexError, ValueError):
            pos = i + len(MARK)          # 不是我们认得的头，跳过继续找
            continue

        start = j + 1
        need = n_bytes * 2               # 要收多少个十六进制字符
        digits = bytearray()
        k = start
        bad = None
        while k < len(buf) and len(digits) < need:
            c = buf[k]
            if c in HEXSET:
                digits.append(c)
            elif c in BLANK:
                pass
            else:
                bad = c
                break                     # 混进了非十六进制、非空白
            k += 1

        if bad is not None:
            LAST_DIAG = ("第 %d 个十六进制字符那里出现了 %r —— "
                         "日志里混进了别的东西（时间戳？十六进制显示？）"
                         % (len(digits), bytes([bad])))
            pos = start
            continue

        if len(digits) < need:
            LAST_DIAG = ("数据没收完：要 %d 个十六进制字符，只收到 %d 个 "
                         "（约 %.0f%%）—— /dump 是不是被打断了？"
                         % (need, len(digits), 100.0 * len(digits) / need))
            pos = start
            continue

        found = (n_samples, n_bytes, bytes.fromhex(digits.decode("ascii")))
        pos = start

    return found


def echo_text(chunk):
    """把收到的字节"像终端那样"显示：中文正常显示，其余控制字符显示成点。"""
    s = chunk.decode("utf-8", "replace")
    s = "".join(c if (c.isprintable() or c in "\r\n") else "." for c in s)
    sys.stdout.write(s)
    sys.stdout.flush()


# ==================================================================
# ① 离线：读串口助手存下来的日志（默认这条路）
# ==================================================================
def capture_file(path):
    with open(path, "rb") as f:
        return f.read()


# ==================================================================
# ② 实时：自己开串口收发（需要 pyserial）
# ==================================================================
def capture_live(port, baud, rec_sec, echo):
    try:
        import serial
    except ImportError:
        print("★ 没装 pyserial。两条路：")
        print("     pip install pyserial")
        print("     或者用串口助手存成日志，再跑： dump2wav.py 日志.txt")
        sys.exit(2)

    buf = bytearray()
    with serial.Serial(port, baud, timeout=0.2) as ser:
        ser.reset_input_buffer()

        if rec_sec:
            print(">>> 发 /rec %d —— 现在对着麦克风说话！" % rec_sec)
            ser.write(("/rec %d\n" % rec_sec).encode())
            # 纯按时间等：录音正好 rec_sec 秒，留 2 秒余量收尾巴。
            # （不去匹配中文提示串：那种匹配一改文案就失效，不值得。）
            t_end = time.time() + rec_sec + 2.0
            while time.time() < t_end:
                chunk = ser.read(4096)
                if chunk:
                    buf += chunk
                    if echo:
                        echo_text(chunk)

        print(">>> 发 /dump（要吐一会儿，别急）")
        ser.write(b"/dump\n")
        t_end = time.time() + 120.0
        found = None
        while time.time() < t_end:
            chunk = ser.read(8192)
            if chunk:
                buf += chunk
                if echo:
                    echo_text(chunk)
            found = find_dump(buf)
            if found:
                break

    return bytes(buf)


# ==================================================================
# 拼 wav
# ==================================================================
def build_wav(buf, found, freq, raw_level, out_path):
    n_samples, n_bytes, raw = found

    if len(raw) != n_bytes:
        print("★ 数据长度不对：声明 %d 字节，解出来 %d 字节" % (n_bytes, len(raw)))
        sys.exit(3)

    samples = array.array("h")
    samples.frombytes(raw[: n_samples * 2])
    if sys.byteorder == "big":
        samples.byteswap()

    n = len(samples)
    dur = n / float(freq)

    dc = sum(samples) / float(n)
    raw_pp = max(samples) - min(samples)

    # ---- 去直流 + 归一化 ----------------------------------------
    # 去直流一定要做：INMP441 有个约 -3355 的固定偏置（≈ 满量程的 10%），
    # 留在里面等于白占一成动态范围。归一化是为了"听得见"——
    # 正常说话录到的峰峰值也就几千，按原样放几乎听不见。
    # ★ 全部用浮点做，所以不会削顶（固件那边一个字节都没改，是原始数据）。
    peak = max(abs(v - dc) for v in samples)
    gain = 1.0
    if (not raw_level) and peak > 0:
        gain = 32000.0 / peak
        if gain > 200.0:
            gain = 200.0        # 上限：否则"根本没拾到音"会被放大成一片噪声，反而像有声音
    gain = max(1.0, gain)       # 本来就很响就不要再衰减

    out = array.array("h")
    for v in samples:
        x = (v - dc) * gain
        if x > 32767:
            x = 32767
        elif x < -32768:
            x = -32768
        out.append(int(x))

    with wave.open(out_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(freq)
        w.writeframes(out.tobytes())

    # ---- 报告 ----------------------------------------------------
    print()
    print("=" * 62)
    print("样本 %d 个 / %.3f 秒（%d Hz 单声道 16bit）" % (n, dur, freq))
    print("直流 %+.0f   原始峰峰值 %d   %s" %
          (dc, raw_pp, "（未归一化）" if raw_level else "应用增益 %.2fx" % gain))

    if raw_pp < 200:
        print("★ 原始峰峰值不到 200 —— 基本只有底噪，这次多半没拾到音。")
        print("  对着麦说句话再录一次；一直这样就看固件打的 [MIC] 那几行。")
    elif raw_pp < 1500:
        print("  峰峰值偏小：可能离麦远 / 说话轻。能听，但底噪会被一起放大。")

    # 分段峰峰：和固件 /rec 完成后打的那一行**逐个数应该一致**。
    # 对得上 = 从麦克风到 PC 这一整条路一个字节都没错。
    seg = max(1, n // 8)
    env = []
    for k in range(8):
        a = k * seg
        b = min(n, a + seg)
        if a >= n:
            env.append("-")
        else:
            s = samples[a:b]
            env.append(str(max(s) - min(s)))
    print("分段峰峰（原始）：" + " ".join(env))
    print("  ↑ 和固件 [REC] 那一行对一下；对不上就是传输路上被改过")

    print("已写出：%s" % out_path)
    print("=" * 62)
    return out_path


def play(path):
    try:
        import winsound
        print("播放中……（Ctrl+C 可中断）")
        winsound.PlaySound(path, winsound.SND_FILENAME)
    except Exception as e:
        print("自动播放失败（%s）—— 直接双击那个 wav 文件放吧" % e)


# ==================================================================
def looks_like_port(s):
    s = s.upper()
    return s.startswith("COM") and s[3:].isdigit()


def main():
    ap = argparse.ArgumentParser(
        description="把 F407 串口吐出的十六进制音频拼成 wav",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="例：\n"
               "  python dump2wav.py log.txt          # 串口助手存的日志（不用装任何东西）\n"
               "  python dump2wav.py COM5 --rec 2     # 自己开串口收（要先 pip install pyserial）")
    ap.add_argument("target", nargs="?",
                    help="串口号（COM5）或串口助手存下来的日志文件")
    ap.add_argument("--file", help="同上，显式指定是文件")
    ap.add_argument("--rec", type=int, default=0, metavar="N",
                    help="先让板子录 N 秒再收（1~2）。不给就只收最近一次录音")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--freq", type=int, default=16000, help="采样率，默认 16000")
    ap.add_argument("--out", help="输出 wav 路径（默认按时间戳命名）")
    ap.add_argument("--raw-level", action="store_true",
                    help="保持原始电平，不做归一化")
    ap.add_argument("--no-play", action="store_true", help="不要自动播放")
    ap.add_argument("--quiet", action="store_true", help="实时模式不回显板子的输出")
    args = ap.parse_args()

    tgt = args.file or args.target
    if not tgt:
        ap.error("要给它一个串口号（COM5）或者一个日志文件路径")

    if args.file or not looks_like_port(tgt):
        print("读日志文件：%s" % tgt)
        buf = capture_file(tgt)
    else:
        print("实时收：%s @ %d" % (tgt, args.baud))
        buf = capture_live(tgt, args.baud, args.rec, not args.quiet)

    found = find_dump(buf)
    if not found:
        print()
        print("★ 没解析出音频数据。%s" % LAST_DIAG)
        print("  收到 %d 字节。检查：" % len(buf))
        print("    日志里到底有没有 /dump 之后的 [DUMP] BEGIN 那一行？")
        print("    板子上先发过 /rec 吗？（/dump 只能吐「最近一次」录音）")
        sys.exit(1)

    n_samples, n_bytes, _ = found
    print(">>> 解析成功：%d 样本 / %d 字节" % (n_samples, n_bytes))

    out = args.out or ("micrec_%s.wav" %
                       datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    path = build_wav(buf, found, args.freq, args.raw_level, out)

    if not args.no_play:
        play(path)


if __name__ == "__main__":
    main()


