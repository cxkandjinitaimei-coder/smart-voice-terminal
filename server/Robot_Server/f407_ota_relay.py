# -*- coding: utf-8 -*-
"""LAN relay for the existing ESP8266 -> F407 OTA protocol.

The relay deliberately does not implement the F407 wire protocol.  It calls
the already verified ESP endpoints in order: request (K-UP gate), update
(multipart stream), and status (post-reboot evidence).
"""
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
        last_observed = None
        while time.monotonic() < deadline:
            time.sleep(2)
            elapsed = int(time.monotonic() - verify_start)
            _notify(progress, "verifying", elapsed_sec=elapsed, timeout_sec=180)
            try:
                response = session.get(base + "status", headers=headers, timeout=(3, 15))
                if response.status_code < 400:
                    final = response.json()
                    # 只在"板子答的内容变了"时报一次，好让任务日志能看见它在答什么。
                    observed = {
                        "online": bool(final.get("online")),
                        "base": int(final.get("base") or 0),
                        "slot_crc": int(final.get("slot_crc") or 0),
                        "state": final.get("state"),
                        "expected_crc": padded_crc,
                    }
                    if observed != last_observed:
                        last_observed = observed
                        _notify(progress, "verifying", elapsed_sec=elapsed, timeout_sec=180,
                                observed=observed)
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
        if final is None:
            detail = "重启后 ESP 的 /f407/status 一直没有可用应答（ESP 自己没起来，或被别的东西占着）"
        else:
            detail = ("最后一次状态 online=%s base=0x%08X slot_crc=0x%08X state=%s"
                      "（期望 slot_crc=0x%08X）"
                      % (bool(final.get("online")), int(final.get("base") or 0),
                         int(final.get("slot_crc") or 0), final.get("state"), padded_crc))
        raise RelayError(
            "镜像已提交，但重启后没读到 Slot A 全片 CRC=0x%08X；%s。"
            "online=false ⇒ ESP 没收到 F407 的 @IAP:INFO 回帧（抓 F407 的 PA2/PA3 看有没有"
            " [IAP] stage=…）；online=true 但 slot_crc 不等 ⇒ 槽内容与镜像不一致，"
            "别重试，先看 BOOT 日志。" % (padded_crc, detail))
    except requests.RequestException as exc:
        raise RelayError("无法连接 ESP %s: %s" % (host, exc)) from exc
    finally:
        if own_session:
            session.close()

