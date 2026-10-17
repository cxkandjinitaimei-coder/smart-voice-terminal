"""f407_ota_relay.py：F407 OTA 镜像边界/向量校验及 ESP 直连传输适配。

使用：ota_console 校验固件，局域网时可调用 ESP HTTP OTA；跨网使用 iap_pull。
时序与边界：容量与 iap_flash.h 保持一致；CRC 是完整性检查，不是密码签名。未实体确认不得擦除设备。
配置/数据：私有配置及运行产物不随公开源码分发；模块路径以 __file__ 定位。
"""
# -*- coding: utf-8 -*-
import ipaddress
import time
import zlib

import requests

MAX_APP = 224 * 1024
LAYOUT_ID = 0x00020001
SLOT_A_BASE = 0x08008000


class RelayError(RuntimeError):
    pass


def validate_image(data):
    """Return raw and padded CRCs after checking an OTA Slot-A BIN."""
    if not 8 <= len(data) <= MAX_APP:
        raise RelayError("固件必须是 8..229376 字节的 Slot A BIN（不能上传 HEX 或 bootstrap）")
    sp = int.from_bytes(data[0:4], "little")
    pc = int.from_bytes(data[4:8], "little")
    if sp & 7 or not (0x20000000 < sp <= 0x20020000 or
                       0x10000000 < sp <= 0x10010000):
        raise RelayError("固件初始栈指针无效")
    if not (pc & 1) or not SLOT_A_BASE + 8 <= (pc & ~1) < SLOT_A_BASE + len(data):
        raise RelayError("固件不是链接到 0x08008000 的应用 BIN")
    raw = zlib.crc32(data) & 0xffffffff
    padded = zlib.crc32(data + b"\xff" * (MAX_APP - len(data))) & 0xffffffff
    return raw, padded


def _host(value):
    try:
        # Reject DNS names and URL fragments: this endpoint is intentionally LAN-only.
        return str(ipaddress.ip_address(str(value).strip()))
    except ValueError as exc:
        raise RelayError("esp_ip 必须是局域网 IPv4 地址") from exc


def _json(response, label):
    try:
        body = response.json()
    except ValueError as exc:
        raise RelayError("ESP %s 返回了非 JSON（HTTP %s）" % (label, response.status_code)) from exc
    if response.status_code >= 400:
        msg = body.get("message") or body.get("err") or body.get("error") or body
        raise RelayError("ESP %s 失败（HTTP %s）：%s" % (label, response.status_code, msg))
    return body


def _notify(progress, phase, **info):
    """Fire the optional progress callback.

    A broken callback must never abort a flash that is already in progress, so
    every exception is swallowed on purpose.  ``progress=None`` (the default)
    makes this a no-op, so callers that do not pass one see identical timing.
    """
    if progress is None:
        return
    try:
        progress(phase, info)
    except Exception:
        pass


def perform_lan_ota(esp_ip, data, token, approval_timeout=120, session=None,
                    poll_interval=1.0, progress=None):
    """Run one complete LAN OTA and return evidence suitable for the API.

    Raises RelayError before upload if the ESP/F407 is not ready or the user
    does not press K-UP.  No flash is erased by the request phase.

    ``progress`` is an optional ``callable(phase, info)``.  Phases, in order:
    ``probing`` -> ``waiting_user`` (carries ``remaining_sec``) -> ``uploading``
    -> ``verifying`` (carries ``elapsed_sec``).  There is deliberately no
    byte-level progress: the image travels as one multipart POST, so a
    percentage would be invented.
    """
    host = _host(esp_ip)
    raw_crc, padded_crc = validate_image(data)
    own_session = session is None
    session = session or requests.Session()
    session.trust_env = False
    headers = {"Authorization": "Bearer " + str(token)}
    base = "http://%s:8080/f407/" % host
    try:
        _notify(progress, "probing", esp_ip=host, bytes=len(data),
                crc32="%08X" % raw_crc)
        state = _json(session.get(base + "status", headers=headers, timeout=(3, 15)), "status")
        if not state.get("online") or state.get("base") != SLOT_A_BASE or state.get("capacity") != MAX_APP or state.get("layout_id") != LAYOUT_ID:
            raise RelayError("F407 未处于 OTA bootstrap 就绪状态（请先串口安装扩容完整HEX，并更新配套ESP（A=224KiB/B=256KiB））")

        request_state = _json(session.post(
            base + "request", params={"total": len(data), "crc": raw_crc},
            headers=headers, timeout=(5, 10)), "request")
        approval = request_state.get("approval")
        if approval == "approved":
            pass
        elif approval == "waiting_user":
            deadline = time.monotonic() + approval_timeout
            while time.monotonic() < deadline:
                _notify(progress, "waiting_user", timeout_sec=approval_timeout,
                        remaining_sec=max(0, int(round(deadline - time.monotonic()))))
                time.sleep(poll_interval)
                state = _json(session.get(base + "status", headers=headers,
                                           timeout=(3, 15)), "status")
                approval = state.get("approval")
                if approval == "approved":
                    break
                if approval in ("none", "cancelled", "expired"):
                    raise RelayError("F407 取消或拒绝了 K-UP 确认")
            else:
                raise RelayError("等待 F407 K-UP 确认超时；未上传、未擦除固件")
        else:
            raise RelayError("F407 没有进入用户确认状态：%s" % request_state)

        _notify(progress, "uploading", bytes=len(data), crc32="%08X" % raw_crc)
        upload = _json(session.post(
            base + "update", params={"total": len(data), "crc": raw_crc},
            headers=headers,
            files={"firmware": ("application.bin", data, "application/octet-stream")},
            timeout=(5, 240)), "update")
        if not upload.get("ok"):
            raise RelayError("F407 上传未确认成功：%s" % upload)

        # The ESP is rebooting immediately after COMMIT/JUMP.  Keep asking until
        # its INFO response reports the new full Slot-A CRC.
        deadline = time.monotonic() + 180
        verify_start = time.monotonic()
        _notify(progress, "verifying", elapsed_sec=0, timeout_sec=180)
        final = None
        while time.monotonic() < deadline:
            time.sleep(2)
            _notify(progress, "verifying",
                    elapsed_sec=int(time.monotonic() - verify_start), timeout_sec=180)
            try:
                response = session.get(base + "status", headers=headers, timeout=(3, 15))
                if response.status_code < 400:
                    final = response.json()
                    if (final.get("online") and final.get("base") == SLOT_A_BASE and
                            final.get("slot_crc") == padded_crc):
                        return {
                            "ok": True, "esp_ip": host, "bytes": len(data),
                            "crc32": "%08X" % raw_crc,
                            "slot_a_crc32": "%08X" % padded_crc,
                            "approval": "approved", "upload": upload, "status": final,
                            "message": "PASS: new application is running; full Slot A CRC matches %08X" % padded_crc,
                        }
            except (requests.RequestException, ValueError):
                # Expected during the reboot window.
                continue
        raise RelayError("镜像已提交但重启后未观察到 Slot A CRC=%08X；请保存 F407 BOOT/IAP 日志" % padded_crc)
    except requests.RequestException as exc:
        raise RelayError("无法连接 ESP %s: %s" % (host, exc)) from exc
    finally:
        if own_session:
            session.close()

