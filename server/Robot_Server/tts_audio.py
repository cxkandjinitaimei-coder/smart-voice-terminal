"""TTS 音频格式适配：将 PCM WAV 统一为 F407 所需的格式。

用法：调用 normalize_wav(wav_bytes)，取得带标准 44 字节头的
16 kHz / 16 bit / 单声道、小端 PCM WAV。只依赖现有 numpy；不使用
Python 3.13 起已移除的 audioop，不安装解码器，不下载模型。
"""
import io
import math
import wave


def resample(samples, source_rate, target_rate=16000):
    """有理数重采样，先低通再抽取，避免直接丢样造成高频混叠。"""
    import numpy as np

    samples = np.asarray(samples, dtype=np.float64)
    if source_rate == target_rate:
        return samples
    if not 8000 <= source_rate <= 96000:
        raise ValueError("TTS 采样率需在 8000~96000Hz")
    divisor = math.gcd(source_rate, target_rate)
    up, down = target_rate // divisor, source_rate // divisor
    scale = max(up, down)
    radius = 16 * scale
    positions = np.arange(-radius, radius + 1, dtype=np.float64)
    kernel = np.sinc(positions / scale) * np.hanning(len(positions))
    kernel *= up / kernel.sum()
    # 按相位计算，避免上采样临时数组在44.1kHz等比例下成百倍膨胀。
    count = (len(samples) * up + down - 1) // down
    output = np.zeros(count, dtype=np.float64)
    indices = np.arange(count, dtype=np.int64) * down
    for phase in range(up):
        selected = np.flatnonzero(indices % up == phase)
        if not len(selected):
            continue
        taps = kernel[(radius + phase) % up::up]
        # kernel 的下标与卷积下标保持一致，补偿对称滤波器的群延时。
        first = (radius + phase) % up
        conv = np.convolve(samples, taps, mode="full")
        source_indices = (indices[selected] + radius - first) // up
        output[selected] = conv[source_indices]
    return output


def pcm_wav(samples, source_rate):
    """浮点单声道波形→硬件标准WAV；削峰防止16位转换溢出。"""
    import numpy as np

    samples = np.asarray(samples, dtype=np.float64)
    if samples.ndim != 1 or not len(samples) or not np.isfinite(samples).all():
        raise ValueError("TTS 返回空音频或无效样本")
    if len(samples) / source_rate > 90:
        raise ValueError("单次TTS音频不能超过90秒")
    audio = resample(samples, int(source_rate))
    pcm = np.rint(np.clip(audio, -1, 1) * 32767).astype("<i2").tobytes()
    out = io.BytesIO()
    with wave.open(out, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(pcm)
    return out.getvalue()


def normalize_wav(data):
    """解析RIFF各chunk并重写标准头；支持8/16/24/32位整数PCM。"""
    import numpy as np

    if len(data) > 35 * 1024 * 1024:
        raise ValueError("TTS WAV过大")
    with wave.open(io.BytesIO(data), "rb") as wav:
        channels, width, rate = wav.getnchannels(), wav.getsampwidth(), wav.getframerate()
        if channels not in (1, 2) or wav.getcomptype() != "NONE":
            raise ValueError("TTS需要单/双声道整数PCM WAV")
        frames = wav.getnframes()
        if frames / rate > 90:
            raise ValueError("单次TTS音频不能超过90秒")
        raw = wav.readframes(frames)
        if len(raw) != frames * channels * width:
            raise ValueError("TTS WAV数据不完整")
    if width == 1:
        audio = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128) / 128
    elif width in (2, 4):
        audio = np.frombuffer(raw, dtype="<i%d" % width).astype(np.float64) / (2 ** (8 * width - 1))
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        audio = ((v ^ 0x800000) - 0x800000).astype(np.float64) / 8388608
    else:
        raise ValueError("不支持的TTS PCM位宽")
    if channels == 2:
        audio = audio.reshape(-1, 2).mean(axis=1)
    return pcm_wav(audio, rate)
