# -*- coding: utf-8 -*-
"""「远程OTA」页浏览器端到端验收 —— headless Edge + 手写 CDP（cdp_min.py）。

跑法：  cd d:\\ArduinoIDE\\ArdProject\\flask_relay
        D:\\Python\\python.exe test_ota_panel.py
        （**必须用 D:\\Python\\python.exe** —— PowerShell 会把无 BOM 的 UTF-8 按 GBK
          读，中文断言全烂）

它验的是「网页状态机」：真浏览器 → 真 Flask → 真 `/api/otactl/*` → （桩掉的烧录器）。
真板、真局域网、真 ESP 的 :8080 都不在这里（那些由用户按交付包上板兜）。

★ 为什么自己起一个桩服务、而不打用户正在跑的 5000（前四条同 test_interact_panel.py）：
  1. **模板缓存**：Jinja 改了必须重启 Flask。打 5000 的话"那台重启过没有"不由我决定
     ⇒ 断言会为一个与本次改动无关的原因红/绿（最坏是**假绿**：旧模板里正好也有
     `#ota-phase-title`）。自己起，模板一定是当前这份。
  2. **不污染**：`decision_log.txt` 是 AI 的**记忆源**，`relay.db` 是统计源 ⇒ 全部指到
     %TEMP%，测完即弃。
  3. `config.json` **只读**（合作表 §2.3 冻结）：token 从里面读出来用，不改一个字节。
  4. 页面自己的 5 秒轮询、1 秒任务轮询都由**它自己**的进程承担，不干扰用户的调试现场。
  ★ 本页特有的两条：
  5. **绝不能让 `perform_lan_ota` 真的去连设备**：它会往局域网发 HTTP、让板子停在
     PRESS K-UP 上等 120 秒。桩服务把它换成"按阶段睡够时间再返回"的假实现 ⇒ 页面状态机
     （queued→probing→waiting_user→uploading→verifying→done/failed）在没有板子的情况下
     被**完整驱动**，而上传/擦写一个字节都不会发生。
  6. **固件库必须指到 %TEMP%**：`ota_console.FW407_DIR` 默认是 `flask_relay/firmware_f407/`，
     那是运行态数据目录 —— 测试绝不能往真库里丢文件（下面专门断言真库没被动过）。
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

import requests

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cdp_min import Browser, wait_port                    # noqa: E402

APP = os.path.dirname(os.path.abspath(__file__))
PY = r'D:\Python\python.exe'
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
PORT = 5098
BASE = 'http://127.0.0.1:%d' % PORT
CDP_PORT = 9357
TMP = os.path.join(tempfile.gettempdir(), 'ota_panel_verify')
os.makedirs(TMP, exist_ok=True)
PROFILE = os.path.join(TMP, 'edge_profile')
FAIL_MARK = os.path.join(TMP, 'ota_fail.marker')
FW_TMP = os.path.join(TMP, 'firmware_f407')
SLOT_A_BASE = 0x08008000
WAIT_SEC = 5              # 桩实现里「等 K-UP」持续几秒（要够浏览器采样到倒计时在走）

RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok), detail))
    print(('  [OK]   ' if ok else '  [FAIL] ') + name + (('  -> ' + str(detail)) if detail else ''))
    return ok


def token():
    with open(os.path.join(APP, 'config.json'), 'r', encoding='utf-8') as f:
        return json.load(f).get('token', '')


# ==================================================================
# 两个测试用镜像（都是 512 字节，够过/够不过体检）
# ==================================================================
def slot_a_bin(size=512):
    """能过体检的 Slot A 镜像（SP 合法、PC 在镜像内且置了 Thumb 位）。"""
    b = bytearray([0xFF] * size)
    b[0:4] = (0x2001CBB0).to_bytes(4, 'little')
    b[4:8] = (SLOT_A_BASE + 0x1A1).to_bytes(4, 'little')
    return bytes(b)


def esp_bin(size=512):
    """一眼就是 ESP8266 固件（入口 0x40000400 不在 STM32 flash 里）。"""
    return struct.pack('<II', 0x3FFE8000, 0x40000401) + b'\xff' * (size - 8)


def crc32_hex(data):
    return '%08X' % (zlib.crc32(data) & 0xffffffff)


# ==================================================================
# 桩服务
# ==================================================================
STUB = r'''# -*- coding: utf-8 -*-
import os, sys, time
HERE = r"__APP__"
sys.path.insert(0, HERE)
TMP = r"__TMP__"
import app, store, ai_relay, ota_console, f407_ota_relay

# ① 固件库指到 %TEMP% —— 真 flask_relay/firmware_f407/ 一个文件都不许进
FW = os.path.join(TMP, "firmware_f407")
ota_console.FW407_DIR = FW
os.makedirs(FW, exist_ok=True)

# ② 落盘全部改到 %TEMP%：真 decision_log.txt（AI 的记忆）与 relay.db 一个字节都不碰
app.DATA_HISTORY = os.path.join(TMP, "data_history.txt")
app.DECISION_LOG = os.path.join(TMP, "decision_log.txt")
app.DEBUG_LOG = os.path.join(TMP, "debug_chat_log.txt")
# store 里另有一套同名常量（backfill() 读的是 store 的，不跟 app 走）
store.DB_PATH = os.path.join(TMP, "relay.db")
store.DATA_HISTORY = app.DATA_HISTORY
store.DECISION_LOG = app.DECISION_LOG
for _suffix in ("", "-wal", "-shm"):
    try:
        os.remove(store.DB_PATH + _suffix)
    except OSError:
        pass
store._initialized = False
with open(app.DATA_HISTORY, "w", encoding="utf-8"):
    pass
with open(app.DECISION_LOG, "w", encoding="utf-8"):
    pass

# ③ 定时唤醒必须先关掉（before_request 会在**第一个请求**里把它拉起来）
_real_load_config = app.load_config
def load_config():
    cfg = _real_load_config()
    tk = dict(cfg.get("auto_tick") or {})
    tk["enabled"] = False
    cfg = dict(cfg)
    cfg["auto_tick"] = tk
    return cfg
app.load_config = load_config

# ④ 把真正的烧录器换成"按阶段睡够时间 + 可被标记切成失败"的假实现。
#    ★ 不 patch 的话，页面一点「开始烧录」就会真的往局域网发 HTTP。
WAIT_SEC = __WAIT__
FAIL_MARK = os.path.join(TMP, "ota_fail.marker")

def fake_perform_lan_ota(esp_ip, data, token=None, session=None, progress=None, **kw):
    def P(phase, **info):
        if progress is not None:
            progress(phase, dict(info))
    P("probing", esp_ip=esp_ip, bytes=len(data))
    time.sleep(0.6)
    if os.path.exists(FAIL_MARK):
        # 与真实现同一条文案口径：没按 K-UP 之前**没有**上传、没有擦写
        raise f407_ota_relay.RelayError(
            "K-UP 超时（" + str(WAIT_SEC) + " 秒）：设备没有在屏幕上得到确认，"
            "未上传、未擦除固件。（桩实现，没碰任何真设备）")
    P("waiting_user", remaining_sec=WAIT_SEC, timeout_sec=WAIT_SEC)
    for left in range(WAIT_SEC - 1, 0, -1):
        time.sleep(1.0)
        P("waiting_user", remaining_sec=left, timeout_sec=WAIT_SEC)
    P("uploading", bytes=len(data), crc32="STUB")
    time.sleep(2.5)
    P("verifying", elapsed_sec=0, timeout_sec=180)
    for i in (2, 4):
        time.sleep(1.0)
        P("verifying", elapsed_sec=i, timeout_sec=180)
    return {"ok": True, "esp_ip": esp_ip, "bytes": len(data),
            "crc32": "DEADBEEF", "slot_a_crc32": "CAFEBABE",
            "message": "PASS: 桩实现（没连任何真设备）"}

f407_ota_relay.perform_lan_ota = fake_perform_lan_ota

app.app.run(host="127.0.0.1", port=__PORT__, threaded=True, use_reloader=False)
'''


def start_stub():
    src = (STUB.replace('__APP__', APP).replace('__TMP__', TMP)
           .replace('__WAIT__', str(WAIT_SEC)).replace('__PORT__', str(PORT)))
    path = os.path.join(TMP, 'stub_server.py')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    logf = open(os.path.join(TMP, 'stub_out.log'), 'wb')
    p = subprocess.Popen([PY, path], cwd=APP, stdout=logf, stderr=subprocess.STDOUT)
    if not wait_port(PORT, 30):
        raise SystemExit('桩服务起不来，看 ' + os.path.join(TMP, 'stub_out.log'))
    print('  桩服务已起 pid=%d @ %s' % (p.pid, BASE))
    return p


def start_edge():
    if os.path.isdir(PROFILE):
        shutil.rmtree(PROFILE, ignore_errors=True)
    p = subprocess.Popen([EDGE, '--headless=new',
                          '--remote-debugging-port=%d' % CDP_PORT,
                          '--user-data-dir=' + PROFILE,
                          '--no-first-run', '--no-default-browser-check',
                          '--disable-extensions', '--disable-sync',
                          '--window-size=1280,1000', 'about:blank'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_port(CDP_PORT, 30):
        raise SystemExit('Edge 的调试端口没起来')
    for _ in range(40):
        try:
            br = Browser(CDP_PORT)
            print('  Edge 已起 pid=%d' % p.pid)
            return p, br
        except Exception:
            time.sleep(0.5)
    raise SystemExit('连不上 Edge 的 CDP')


def js(br, sid, expr):
    return br.evaluate(expr, sid)


def open_page(br, cookie, hashname):
    sid, tid = br.new_page('about:blank')
    for m in ('Page.enable', 'Runtime.enable', 'Log.enable', 'Network.enable', 'DOM.enable'):
        br.call(m, {}, session=sid)
    br.call('Network.setCookie', {'name': 'session', 'value': cookie,
                                  'domain': '127.0.0.1', 'path': '/', 'httpOnly': True},
            session=sid)
    mark = len(br.events)      # ⚠️ br.events 跨页共享：不记 mark 会被上一页的 loadEventFired 骗
    br.call('Page.navigate', {'url': BASE + '/#' + hashname}, session=sid)
    for _ in range(120):
        if any(m.get('method') == 'Page.loadEventFired' for m in br.events[mark:]):
            break
        br.pump(0.25)
    br.pump(1.5)
    return sid


def set_file(br, sid, selector, path):
    """把真文件塞进 <input type=file>（CDP 的 DOM.setFileInputFiles）。"""
    doc = br.call('DOM.getDocument', {'depth': 1}, session=sid)
    node = br.call('DOM.querySelector',
                   {'nodeId': doc['root']['nodeId'], 'selector': selector}, session=sid)
    nid = node.get('nodeId')
    if not nid:
        raise SystemExit('找不到文件输入框 ' + selector)
    br.call('DOM.setFileInputFiles', {'files': [path], 'nodeId': nid}, session=sid)


SNAP_JS = """(() => {
  const el = (id) => document.getElementById(id);
  const t = (id) => { const e = el(id); return e ? e.textContent : null; };
  const res = el('ota-result'), card = el('ota-phase-card'), fwbox = el('ota-fw-list');
  return {
    tabs: Array.from(document.querySelectorAll('.proj-tab')).map(b => b.textContent),
    pane: !!document.querySelector('#pane-ota'),
    hasPage: !!(window.PAGES && window.PAGES.ota),
    title: t('ota-phase-title'),
    sub: t('ota-phase-sub'),
    cardCls: card ? card.className : '',
    steps: Array.from(document.querySelectorAll('#ota-steps .ota-step')).map(s => s.className),
    stepText: (el('ota-steps') || {}).textContent || '',
    log: t('ota-log'),
    resultVisible: !!(res && !res.classList.contains('hidden')),
    resultCls: res ? res.className : '',
    resultText: res ? res.textContent : '',
    flashBtn: (el('btn-ota-flash') || {}).textContent || '',
    flashOff: !!(el('btn-ota-flash') || {}).disabled,
    fwEmpty: !!(fwbox && fwbox.className === 'empty-box'),
    fwText: fwbox ? fwbox.textContent : '',
    fwCrcs: Array.from(document.querySelectorAll('#ota-fw-list tbody tr td code'))
              .map(c => c.textContent),
    uploadText: t('ota-upload-result'),
    uploadCls: (el('ota-upload-result') || {}).className || '',
    maxBytes: t('ota-max-bytes'),
    devices: Array.from(document.querySelectorAll('#ota-devices option')).map(o => o.textContent),
    preflight: t('ota-preflight'),
    preflightCls: (el('ota-preflight') || {}).className || '',
    alerts: window.__alerts || []
  };
})()"""

PATCH_JS = """(() => {
  window.__alerts = [];
  window.alert = function (m) { window.__alerts.push(String(m)); };
  window.confirm = function () { return true; };   // 无头浏览器里 confirm 会把 JS 顶住
})()"""


def snap(br, sid):
    return js(br, sid, SNAP_JS)


def run_timeline(br, sid, timeout=90):
    """边跑边采样，直到任务出结果。返回整条时间线（比"抢某一瞬间"稳得多）。"""
    tl = []
    end = time.time() + timeout
    while time.time() < end:
        s = snap(br, sid)
        tl.append(s)
        if s['resultVisible'] and (s['cardCls'].endswith('ota-phase-done')
                                   or s['cardCls'].endswith('ota-phase-failed')):
            break
        time.sleep(0.25)
    return tl


def seen_phase(tl, phase):
    return [s for s in tl if ('ota-phase-' + phase) in s['cardCls']]


def countdowns(tl):
    """采样到的「还剩 N 秒」序列（去掉重复，用来看倒计时有没有真的在走）。"""
    out = []
    for s in tl:
        m = re.search(r'还剩 (\d+) 秒', s['sub'] or '')
        if m and m.group(1) not in out:
            out.append(m.group(1))
    return out


def main():
    if os.path.exists(FAIL_MARK):
        os.remove(FAIL_MARK)
    # ★ 固件库（在 %TEMP%）必须每次从**空**开始：上一轮留下的 BIN 会让
    #   「刚进页面时库是空的」和「体检不过的文件不在库里」两条断言假红，
    #   而且会让 renderFwList 自动选中它 ⇒「没选固件时按钮该禁用」也一起假红。
    shutil.rmtree(FW_TMP, ignore_errors=True)
    flask = start_stub()
    edge = None
    try:
        print('\n=== 1. 登录 + 打开「远程OTA」页 ===')
        s = requests.Session()
        s.post(BASE + '/login', data={'token': token()}, allow_redirects=False)
        cookie = s.cookies.get('session')
        if not cookie:
            raise SystemExit('登录没拿到 session cookie')
        edge, br = start_edge()
        sid = open_page(br, cookie, 'ota')
        js(br, sid, PATCH_JS)

        s0 = snap(br, sid)
        check('★ 页签变成 10 个，且多了「远程OTA」',
              len(s0['tabs']) == 10 and '远程OTA' in s0['tabs'], s0['tabs'])
        check('★ pane 与 page-ota.js 都挂上了（不然是空壳页签）',
              s0['pane'] and s0['hasPage'], {'pane': s0['pane'], 'page': s0['hasPage']})
        check('默认是公网「设备主动拉」：显示测试设备选择、不显示局域网 IP 输入，也不写 ESP:8080',
              js(br, sid, "document.querySelector('input[value=device_pull]').checked")
              and js(br, sid, "!!document.getElementById('ota-devices')")
              and js(br, sid, "document.querySelector('.ota-lan-only').classList.contains('hidden')")
              and 'ESP :8080' not in (s0['title'] or ''), s0['title'])
        check('刚进页面时固件库是空的，并清楚说明需要 Slot A BIN',
              s0['fwEmpty'] and 'Slot A' in s0['fwText'], s0['fwText'][:80])
        check('上限文案来自服务端的 MAX_APP（96 KB）', '96.0 KB' in (s0['maxBytes'] or ''),
              s0['maxBytes'])
        check('还没选固件 ⇒ 「开始烧录」是禁用的（别让用户点了才发现没选）',
              s0['flashOff'] is True, s0['flashOff'])

        # 以下保留原有 LAN 回归：主动切到局域网模式，确保新默认不破坏已验收路径。
        js(br, sid, """(() => { const e = document.querySelector('input[value=lan_direct]');
          e.checked = true; e.dispatchEvent(new Event('change', {bubbles:true})); })()""")
        br.pump(0.3)

        print('\n=== 2. 传一个 ESP 的 bin：必须被当场指出来，且**不落盘** ===')
        bad = os.path.join(TMP, 'ESP_bridge_1382.bin')
        with open(bad, 'wb') as f:
            f.write(esp_bin())
        set_file(br, sid, '#ota-fw-file', bad)
        js(br, sid, "document.getElementById('btn-ota-upload').click()")
        got = ''
        for _ in range(60):
            br.pump(0.25)
            got = js(br, sid, "document.getElementById('ota-upload-result').textContent")
            if got and '入库' in got:
                break
        check('★ 体检不过的镜像：页面直说「没有入库」', '没有入库' in got, got[:120])
        check('★ 原因点破了"这是 ESP8266 固件"（不是干巴巴的「非法向量」）',
              'ESP8266' in got, got[:160])
        s2 = snap(br, sid)
        check('★ 体检不过的文件**不在库里**（以后不会误烧）',
              s2['fwEmpty'] and s2['fwCrcs'] == [], s2['fwText'][:60])

        print('\n=== 3. 传一个合法 Slot A 镜像 → 入库并带上 CRC32 ===')
        good = slot_a_bin()
        okf = os.path.join(TMP, 'F407_PANEL_TEST.bin')
        with open(okf, 'wb') as f:
            f.write(good)
        set_file(br, sid, '#ota-fw-file', okf)
        js(br, sid, "document.getElementById('btn-ota-upload').click()")
        for _ in range(60):
            br.pump(0.25)
            if js(br, sid, "document.querySelectorAll('#ota-fw-list tbody tr').length") >= 1:
                break
        br.pump(0.5)
        s3 = snap(br, sid)
        check('★ 入库成功（页面明说已入库）', '已入库' in (s3['uploadText'] or ''),
              s3['uploadText'])
        check('★ 列表里出现该文件，且 CRC32 是服务端算的、跟本地一致',
              crc32_hex(good) in s3['fwCrcs'] and 'F407_PANEL_TEST.bin' in s3['fwText'],
              s3['fwCrcs'])
        check('大小显示成 512 B（不是裸字节数）', '512 B' in s3['fwText'], s3['fwText'][:120])
        check('★ 库里有了合法固件 ⇒ 「开始烧录」自动解除禁用', s3['flashOff'] is False)
        check('固件库确实落在 %TEMP%（说明重定向生效，真库没被碰）',
              os.path.isfile(os.path.join(FW_TMP, 'F407_PANEL_TEST.bin')))

        print('\n=== 4. 预检：够得着吗 ===')
        js(br, sid, """(() => { document.getElementById('ota-target').value = '127.0.0.1';
          document.getElementById('btn-ota-preflight').click(); })()""")
        br.pump(1.2)
        p1 = js(br, sid, "document.getElementById('ota-preflight').textContent")
        check('★ 同网段时给出肯定结论（本机 127.0.0.1 ⇒ 同 /24）',
              '同 /24 网段' in (p1 or ''), p1)
        js(br, sid, """(() => { document.getElementById('ota-target').value = 'who';
          document.getElementById('btn-ota-preflight').click(); })()""")
        br.pump(1.2)
        p2 = js(br, sid, "document.getElementById('ota-preflight').textContent")
        check('★ 未知名字的预检要说清「必须在线并上报过至少一次」',
              '确认设备在线' in (p2 or ''), p2)

        print('\n=== 5. 服务端学设备 IP：只认非浏览器上报 ===')
        # 浏览器来的请求（deliver=true ⇒ from_web）**不许**把用户 PC 的地址记成设备 IP
        s.post(BASE + '/api/data', json={'token': token(), 'device_id': 'browser-ghost',
                                        'sensor': '', 'user': '', 'deliver': True})
        # ESP 的轮询（没有 deliver/enqueue_decision/no_pop ⇒ from_web=False）
        r = s.post(BASE + '/api/data', json={'token': token(), 'device_id': 'esp-bridge',
                                            'sensor': '温度:28.0,湿度:55.0', 'user': ''})
        ips = {d['device_id']: d['ip'] for d in s.get(BASE + '/api/devices').json()['devices']}
        check('★ ESP 的每秒上报让服务端学到了它的 IP', ips.get('esp-bridge') == '127.0.0.1', ips)
        check('★ 浏览器发来的 /api/data **没有**被当成设备（否则会把固件烧到电脑自己身上）',
              ips.get('browser-ghost', '') == '', ips)
        js(br, sid, 'window.PAGES.ota.refresh()')
        br.pump(1.0)
        devs = snap(br, sid)['devices']
        check('★ 设备下拉里出现「名字（IP，在线）」',
              any('esp-bridge（127.0.0.1，在线）' in d for d in devs), devs)

        print('\n=== 6. 按名字烧录：四个阶段 + 倒计时 + PASS（桩掉真烧录器）===')
        js(br, sid, """(() => { document.getElementById('ota-target').value = 'esp-bridge';
          document.getElementById('btn-ota-flash').click(); })()""")
        tl = run_timeline(br, sid)

        waiting = seen_phase(tl, 'waiting_user')
        uploading = seen_phase(tl, 'uploading')
        verifying = seen_phase(tl, 'verifying')
        check('★ 阶段卡片真的走到 waiting_user（不是一路冲到底的假状态机）',
              bool(waiting), sorted(set(s['cardCls'] for s in tl)))
        check('★ 到 uploading', bool(uploading))
        check('★ 到 verifying', bool(verifying))
        check('★ 页面明确喊「请按板子上的 K-UP 键」',
              bool(waiting) and 'K-UP' in (waiting[0]['title'] or ''), waiting[0]['title'] if waiting else '')
        cd = countdowns(waiting)
        check('★ 倒计时是活的（采样到至少两个不同的剩余秒数，不是写死的文案）',
              len(cd) >= 2, cd)
        check('★ 最关键的一句保证：还没开始写 Flash',
              bool(waiting) and '一个字节都没上传' in (waiting[0]['sub'] or ''),
              waiting[0]['sub'] if waiting else '')
        check('★ 日志里有「现在还没开始写 Flash」的阶段起始行',
              '现在还没开始写 Flash' in (tl[-1]['log'] or ''))
        check('★ 步骤条在跑时高亮当前步（有 .now）',
              any('now' in c for s in tl for c in s['steps']),
              [s['steps'] for s in tl if s['steps']][:1])
        check('★ 上传阶段**不撒谎**：写明是一个整包 POST、拿不到百分比',
              bool(uploading) and '整包 POST' in (uploading[0]['sub'] or '')
              and '拿不到百分比' in (uploading[0]['sub'] or ''),
              uploading[0]['sub'] if uploading else '')
        check('★ 重启验证阶段有已等秒数',
              bool(verifying) and '已等' in (verifying[0]['sub'] or ''),
              verifying[0]['sub'] if verifying else '')

        last = tl[-1]
        check('★ 最终 PASS，不是"卡在运行中"', last['resultVisible'] and 'PASS' in (last['title'] or ''),
              {'title': last['title'], 'cls': last['cardCls']})
        check('★ 结果卡给出 PASS + 目标（名字已解析成 IP）',
              '✔ PASS' in last['resultText'] and '→ 127.0.0.1' in last['resultText'],
              last['resultText'][:120])
        check('★ 结果里带上重启后的 Slot A 全片 CRC32（可跟交付包对账）',
              'CAFEBABE' in last['resultText'], last['resultText'][:200])
        check('★ 结果页也写明还要看板子自己（Running firmware / DHT11）',
              'Running firmware' in last['resultText'])
        check('四个步骤全部标记 ok',
              len(last['steps']) == 4 and all('ok' in c for c in last['steps']), last['steps'])
        check('烧完之后按钮回到可点（不是永远"烧录中…"）',
              not last['flashOff'] and '开始烧录' in (last['flashBtn'] or ''),
              {'btn': last['flashBtn'], 'off': last['flashOff']})

        print('\n=== 7. 失败路径：K-UP 超时必须落 FAIL，而且要带上保证 ===')
        with open(FAIL_MARK, 'w', encoding='utf-8') as f:
            f.write('1')
        js(br, sid, "document.getElementById('btn-ota-flash').click()")
        tl2 = run_timeline(br, sid)
        last2 = tl2[-1]
        check('★ 超时判 FAIL（绝不能显示成 PASS）',
              last2['resultVisible'] and 'FAIL' in (last2['title'] or '')
              and '没有烧进去' in (last2['title'] or ''), last2['title'])
        check('★ FAIL 卡片样式是 failed（红），不是 done',
              last2['cardCls'].endswith('ota-phase-failed'), last2['cardCls'])
        check('★ 失败原因照原样透出 RelayError 的中文（未上传、未擦除固件）',
              '未上传、未擦除固件' in (last2['resultText'] or ''), last2['resultText'][:160])
        check('★ 这次**没有**走到上传阶段（没按 K-UP 就不该写 Flash）',
              not seen_phase(tl2, 'uploading'),
              sorted(set(s['cardCls'] for s in tl2)))
        check('失败也没弹 alert 糊弄人（原因在页面上，可复制）', last2['alerts'] == [],
              last2['alerts'])

        print('\n=== 8. 自律项：不污染真记忆 / 真固件库 / 无 JS 异常 ===')
        real_log = os.path.join(APP, 'decision_log.txt')
        with open(real_log, 'r', encoding='utf-8') as f:
            check('★ 真 decision_log.txt 里没被塞进测试记录',
                  'F407_PANEL_TEST' not in f.read())
        real_fw = os.path.join(APP, 'firmware_f407')
        leaked = sorted(os.listdir(real_fw)) if os.path.isdir(real_fw) else []
        check('★ 真 firmware_f407/ 没多出测试文件（上传物只进了 %TEMP%）',
              'F407_PANEL_TEST.bin' not in leaked, leaked)

        raw = br.console_errors()
        errs = [e for e in raw if 'favicon' not in e and 'net::ERR' not in e]
        # 第 4 步故意打了一次 400（未知名字），Chrome 会把它记成 network error 日志 ⇒ 单列
        expected_400 = [e for e in errs if 'status of 400' in e]
        errs = [e for e in errs if 'status of 400' not in e]
        check('页面没有 JS 异常 / error 级日志', not errs, errs[:6])
        check('被单列掉的只有那次故意打的 400', len(expected_400) <= 2, expected_400[:3])
    finally:
        if edge:
            try:
                edge.kill()
            except Exception:
                pass
        try:
            flask.kill()
        except Exception:
            pass
        shutil.rmtree(PROFILE, ignore_errors=True)
        if os.path.exists(FAIL_MARK):
            os.remove(FAIL_MARK)

    n_fail = len([1 for _n, ok, _d in RESULTS if not ok])
    print('\n==================== %d 项断言，%d 失败 ====================' % (len(RESULTS), n_fail))
    for n, ok, d in RESULTS:
        if not ok:
            print('  FAIL %s  -> %s' % (n, d))
    return 1 if n_fail else 0


if __name__ == '__main__':
    sys.exit(main())
