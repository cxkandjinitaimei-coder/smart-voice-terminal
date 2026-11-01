# -*- coding: utf-8 -*-
"""公网 OTA（device_pull）状态机回归，不接真板也不访问 ESP:8080。"""
import os
import shutil
import tempfile
import zlib

import f407_ota_relay
import iap_pull
import ota_console


def image(size=512):
    out = bytearray(b"\xff" * size)
    out[:4] = (0x2001CBB0).to_bytes(4, "little")
    out[4:8] = (f407_ota_relay.SLOT_A_BASE + 0x101).to_bytes(4, "little")
    return bytes(out)


def reset():
    iap_pull._jobs.clear()
    iap_pull._job_order[:] = []
    iap_pull._device_seen.clear()
    iap_pull._active_id = ""


def main():
    tmp = tempfile.mkdtemp(prefix="iap_pull_public_")
    old_fw = ota_console.FW407_DIR
    try:
        ota_console.FW407_DIR = tmp
        data = image()
        name = "F407_PUBLIC_TEST.bin"
        with open(os.path.join(tmp, name), "wb") as fh:
            fh.write(data)
        reset()

        job = iap_pull.create_job(name, "esp-test")
        assert job["transport"] == "device_pull"
        assert job["phase"] == "waiting_device"
        assert job["target_device"] == "esp-test"
        assert job["crc32"] == "%08X" % (zlib.crc32(data) & 0xffffffff)

        # 浏览器不能抢任务；只有 ESP 自己的轮询才能领取。
        assert iap_pull.announce("esp-bridge", True) is None
        assert iap_pull.announce("another-esp", False) is None
        notice = iap_pull.announce("esp-test", False)
        assert notice and notice["id"] == job["id"]
        assert notice["capacity"] == 229376 and notice["layout_id"] == 0x00020001
        assert iap_pull.get_job(job["id"])["claimed_by"] == "esp-test"

        chunk, eof, err = iap_pull.read_chunk("esp-test", 0)
        assert not eof and not err and chunk == data
        progressed = iap_pull.get_job(job["id"])
        assert progressed["got_blocks"] == 1

        assert iap_pull.report("esp-test", {"phase": "waiting_user"})["ok"]
        waiting = iap_pull.get_job(job["id"])
        assert waiting["phase"] == "waiting_user" and waiting["state"] == "running"

        # K-UP 前取消只让云端任务作废，不会调用任何网络或 Flash 接口。
        cancelled = iap_pull.abort_job(job["id"])
        assert cancelled["state"] == "failed"
        assert "未开始写 Flash" in cancelled["result"]["message"]
        print("PASS public device_pull: target lock -> claimed -> K-UP gate -> cancel")
    finally:
        reset()
        ota_console.FW407_DIR = old_fw
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
