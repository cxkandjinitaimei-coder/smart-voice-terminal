# -*- coding: utf-8 -*-
"""最小 CDP WebSocket 客户端（只依赖标准库）。
环境里没有 websocket / websockets 包，所以手写握手 + 帧编解码。

用法:
    br = Browser(9333)
    sid, tid = br.new_page("about:blank")
    br.send("Runtime.evaluate", {"expression": "1+1"}, sid)
"""
import base64
import hashlib
import json
import os
import socket
import struct
import time
import urllib.request

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WsError(Exception):
    pass


class Ws(object):
    """发起一个 WebSocket 客户端连接（ws:// 明文）。"""

    def __init__(self, url, timeout=30):
        assert url.startswith("ws://"), url
        rest = url[5:]
        hostport, _, path = rest.partition("/")
        path = "/" + path
        host, _, port = hostport.partition(":")
        port = int(port or 80)
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)

        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n" % (path, hostport, key)
        )
        self.sock.sendall(req.encode())

        # 读到空行为止
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise WsError("握手中连接被关闭")
            buf += chunk
        head, _, tail = buf.partition(b"\r\n\r\n")
        first = head.split(b"\r\n")[0].decode("latin-1")
        if "101" not in first:
            raise WsError("握手失败: %s" % first)
        want = base64.b64encode(
            hashlib.sha1((key + GUID).encode()).digest()).decode()
        if ("sec-websocket-accept: " + want).lower() not in head.decode("latin-1").lower():
            raise WsError("Sec-WebSocket-Accept 不对")
        self._buf = tail          # 服务器可能已经把第一帧一起发来了

    # ---------- 读 ----------

    def _read(self, n):
        while len(self._buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise WsError("连接已关闭")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def recv(self):
        """返回一条完整的文本消息（自动处理分片、ping/pong）。"""
        data = b""
        while True:
            b0, b1 = self._read(2)
            fin = b0 & 0x80
            op = b0 & 0x0F
            masked = b1 & 0x80
            ln = b1 & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._read(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._read(8))[0]
            mask = self._read(4) if masked else None
            payload = self._read(ln) if ln else b""
            if mask:
                payload = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))

            if op == 0x9:                       # ping -> pong
                self._send_frame(0xA, payload)
                continue
            if op == 0xA:                       # pong
                continue
            if op == 0x8:                       # close
                raise WsError("对端要求关闭")
            data += payload
            if fin:
                return data.decode("utf-8", "replace")

    # ---------- 写 ----------

    def _send_frame(self, op, payload):
        n = len(payload)
        head = bytearray([0x80 | op])
        if n < 126:
            head.append(0x80 | n)
        elif n < 65536:
            head.append(0x80 | 126)
            head += struct.pack(">H", n)
        else:
            head.append(0x80 | 127)
            head += struct.pack(">Q", n)
        mask = os.urandom(4)
        head += mask                       # 客户端发出的帧**必须**加掩码
        masked = bytes(payload[i] ^ mask[i % 4] for i in range(n))
        self.sock.sendall(bytes(head) + masked)

    def send_text(self, s):
        self._send_frame(0x1, s.encode("utf-8"))

    def close(self):
        try:
            self._send_frame(0x8, b"")
            self.sock.close()
        except Exception:
            pass


class Browser(object):
    """连浏览器级 CDP，能开页面、拿到页面 session。"""

    def __init__(self, port=9333, timeout=30):
        self.port = port
        info = self._http_get("http://127.0.0.1:%d/json/version" % port)
        self.ws_url = info["webSocketDebuggerUrl"]
        self.ws = Ws(self.ws_url, timeout=timeout)
        self.msg_id = 0
        self.session = None
        self.events = []          # [(method, params)] 只留最近若干条

    @staticmethod
    def _http_get(url, timeout=10):
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))

    # ---------- 收发 ----------

    def _next_id(self):
        self.msg_id += 1
        return self.msg_id

    def call(self, method, params=None, session=None, timeout=30):
        """发一条命令并**等到它的回复**；期间收到的其他消息（事件）存进 self.events。"""
        mid = self._next_id()
        msg = {"id": mid, "method": method, "params": params or {}}
        if session:
            msg["sessionId"] = session
        self.ws.send_text(json.dumps(msg))

        deadline = time.time() + timeout
        while True:
            if time.time() > deadline:
                raise WsError("命令超时: %s" % method)
            try:
                m = json.loads(self.ws.recv())
            except socket.timeout:
                raise WsError("等 %s 回复超时（socket）" % method)
            if m.get("id") == mid:
                if "error" in m:
                    raise WsError("%s 失败: %s" % (method, m["error"]))
                return m.get("result", {})
            if "method" in m:
                self.events.append(m)
                if len(self.events) > 400:
                    del self.events[:200]

    def pump(self, seconds=1.0):
        """空转一会儿，把事件收进来。"""
        end = time.time() + seconds
        old = self.ws.sock.gettimeout()
        try:
            while time.time() < end:
                self.ws.sock.settimeout(max(0.05, end - time.time()))
                try:
                    m = json.loads(self.ws.recv())
                except (socket.timeout, WsError):
                    break
                if "method" in m:
                    self.events.append(m)
        finally:
            self.ws.sock.settimeout(old)

    # ---------- 页面 ----------

    def new_page(self, url="about:blank"):
        r = self.call("Target.createTarget", {"url": url})
        tid = r["targetId"]
        a = self.call("Target.attachToTarget", {"targetId": tid, "flatten": True})
        sid = a["sessionId"]
        return sid, tid

    def close_page(self, tid):
        try:
            self.call("Target.closeTarget", {"targetId": tid})
        except Exception:
            pass

    def evaluate(self, expr, session, await_promise=True):
        r = self.call("Runtime.evaluate", {
            "expression": expr,
            "returnByValue": True,
            "awaitPromise": await_promise,
        }, session=session)
        if "exceptionDetails" in r:
            d = r["exceptionDetails"]
            txt = d.get("exception", {}).get("description") or d.get("text")
            raise WsError("页面里抛异常: %s" % txt)
        return r.get("result", {}).get("value")

    def screenshot(self, session, path):
        r = self.call("Page.captureScreenshot", {"format": "png"}, session=session)
        raw = base64.b64decode(r["data"])
        with open(path, "wb") as f:
            f.write(raw)
        return len(raw)

    def console_errors(self):
        out = []
        for m in self.events:
            if m.get("method") == "Runtime.exceptionThrown":
                d = m["params"].get("exceptionDetails", {})
                out.append("EXCEPTION " + str(
                    d.get("exception", {}).get("description") or d.get("text")))
            elif m.get("method") == "Runtime.consoleAPICalled":
                p = m["params"]
                if p.get("type") in ("error", "warning"):
                    txt = " ".join(str(a.get("value", a.get("description", "")))
                                   for a in p.get("args", []))
                    out.append(p["type"].upper() + " " + txt)
            elif m.get("method") == "Log.entryAdded":
                e = m["params"]["entry"]
                if e.get("level") in ("error", "warning"):
                    out.append("LOG-" + e["level"].upper() + " " + e.get("text", "")
                               + "  <" + str(e.get("url", "")) + ">")
        return out


def wait_port(port, seconds=25):
    end = time.time() + seconds
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.25)
    return False
