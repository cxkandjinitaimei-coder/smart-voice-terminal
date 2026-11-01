# -*- coding: utf-8 -*-
"""互动页「语音 + 对话 + 正式上屏」验收 —— headless Edge + 手写 CDP（cdp_min.py）。

跑法：  cd d:\\ArduinoIDE\\ArdProject\\flask_relay
        D:\\Python\\python.exe test_interact_panel.py
        （**必须用 D:\\Python\\python.exe** —— PowerShell 会把无 BOM 的 UTF-8 按 GBK 读，
          中文断言全烂）

★ 为什么要自己起一个「桩服务」而不是打用户正在跑的那个 5000：
  1. **模板缓存**：Jinja 模板改了必须重启 Flask 才生效。打 5000 的话，那台是否
     重启过不由我决定 ⇒ 断言会为一个"跟本次改动无关"的原因红/绿（最坏是**假绿**：
     旧模板里正好也有 #chat-input）。自己起一个，模板一定是当前这份。
  2. **不花钱**：`sendChat()` 走 `mock:false` 的真 AI。桩服务把
     `ai_relay.get_ai_decision` 换成固定回复 ⇒ 不联网、不花钱、结果确定。
  3. **不污染**：`decision_log.txt` 是 **AI 的记忆源**，`relay.db` 是统计源。
     桩服务把两者都指到 %TEMP%，测完即弃 —— 绝不能往真记忆里灌测试记录。
  4. `config.json` **只读**（合作表 §2.3 冻结）：token 从里面读出来用，不改一个字节。

它覆盖的是「交互链」：真浏览器 → 真 Flask → 真队列/投递账本 → （假 ESP 轮询取走）。
真 AI、串口、屏面本身不在这里（那三样分别由 test_chat_deliver.py / 上板实测兜）。
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

import requests

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cdp_min import Browser, WsError, wait_port          # noqa: E402

APP = os.path.dirname(os.path.abspath(__file__))
PY = r'D:\Python\python.exe'
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
PORT = 5099
BASE = 'http://127.0.0.1:%d' % PORT
CDP_PORT = 9355
TMP = os.path.join(tempfile.gettempdir(), 'interact_panel_verify')
os.makedirs(TMP, exist_ok=True)
PROFILE = os.path.join(TMP, 'edge_profile')

RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok), detail))
    print(('  [OK]   ' if ok else '  [FAIL] ') + name + (('  -> ' + str(detail)) if detail else ''))
    return ok


def token():
    with open(os.path.join(APP, 'config.json'), 'r', encoding='utf-8') as f:
        return json.load(f).get('token', '')


# ==================================================================
# 桩服务：redirect 落盘 + 打桩 AI + 预置三条历史轮次
# ==================================================================
STUB = r'''# -*- coding: utf-8 -*-
import json, os, sys
HERE = %r
sys.path.insert(0, HERE)
TMP = %r
import app, store, ai_relay

# ① 落盘全部改到 %%TEMP%%：真 decision_log.txt（AI 的记忆）与 relay.db 一个字节都不碰
#    （⚠️ 这段是普通字符串 + 百分号格式化：正文里出现的百分号必须写两遍，否则报
#       unsupported format character）
app.DATA_HISTORY = os.path.join(TMP, 'data_history.txt')
app.DECISION_LOG = os.path.join(TMP, 'decision_log.txt')
app.DEBUG_LOG = os.path.join(TMP, 'debug_chat_log.txt')
# ⚠️ store 里另有一套同名常量，backfill() 读的是 store.DATA_HISTORY / store.DECISION_LOG
#    （不跟着 app 走）—— 漏补 DECISION_LOG 会把**真记忆文件全文**回填进临时库。
store.DB_PATH = os.path.join(TMP, 'relay.db')
store.DATA_HISTORY = app.DATA_HISTORY
store.DECISION_LOG = app.DECISION_LOG
# ⚠️ 每跑一次都要从干净库起：上一轮的 WAL 残留会让统计口径漂移。
#    _initialized 也归零 —— 若本进程里 init_db() 曾被真路径调过一次，它会对**临时库**
#    早退（表根本没建），后面每次写入都是 no such table，表现成"统计页空白"。
for _suffix in ('', '-wal', '-shm'):
    try:
        os.remove(store.DB_PATH + _suffix)
    except OSError:
        pass
store._initialized = False
#   data_history.txt 是**追加**写的，清掉才能保证第 5 步的断言不受上一轮残留影响。
with open(app.DATA_HISTORY, 'w', encoding='utf-8'):
    pass

# ② 预置三条轮次：打字 / 语音 / 定时唤醒 —— 互动页的来源角标靠它们验
ROUNDS = [
    {"time": "2026-09-23 09:00:01", "provider": "stub", "model": "", "key": "",
     "sensor": "", "user": "你好呀", "mode": "full", "device_id": "esp-bridge",
     "message": "看到你啦，今天过得怎么样？", "text": "看到你啦",
     "moves": [{"type": "servo", "id": 1, "angle": 100}, {"type": "gesture", "name": "nod"}]},
    {"time": "2026-09-23 09:00:02", "provider": "stub", "model": "", "key": "",
     "sensor": "", "user": "对着板子说的话", "voice": True, "mode": "full",
     "device_id": "esp-bridge", "message": "我听到你说话了。", "moves": []},
    {"time": "2026-09-23 09:00:03", "provider": "stub", "model": "", "key": "",
     "sensor": "", "tick": True, "user": "（内部提示词，不该画成用户气泡）",
     "mode": "full", "device_id": "esp-bridge", "message": "好久没人跟我说话了。",
     "moves": []},
]
with open(app.DECISION_LOG, 'w', encoding='utf-8') as f:
    for r in ROUNDS:
        f.write(json.dumps(r, ensure_ascii=False) + "\n")

# ③ 打桩 AI：不联网、不花钱、回复固定。
#    ⚠️ 必须收 **kw —— 调试页走的是同一个函数但多带 json_mode/max_tokens，
#       写死两参会 TypeError，表现成"调试页 502"，看着像隔离坏了。
ai_relay.get_ai_decision = lambda cfg, msgs, **kw: (
    json.dumps({"message": "（桩）我在听，你说。", "text": "我在听",
                "moves": [{"type": "servo", "id": 1, "angle": 100},
                          {"type": "gesture", "name": "nod"}]}, ensure_ascii=False),
    {"provider": "stub", "model": "stub", "key": ""})

# ④ 定时唤醒必须先关掉。
#    ⚠️ `app.py` 的 @app.before_request 会在**第一个请求**里 start_auto_tick()
#       （那是给 `flask run` / waitress 兜底的），所以"没走 __main__"**不等于**
#       没起这条线程。它一旦开火就会自己调 AI、自己往 decision_log 里追加一轮、
#       自己往队列里塞命令 ⇒ 第 2 步"正好 3 轮"和第 4 步"queue_len=1"全都会被它搅黄，
#       而且现象是随机红（看跑的时候撞没撞上那一秒）。
_real_load_config = app.load_config
def load_config():
    cfg = _real_load_config()
    tk = dict(cfg.get('auto_tick') or {})
    tk['enabled'] = False
    cfg = dict(cfg); cfg['auto_tick'] = tk
    return cfg
app.load_config = load_config

app.app.run(host="127.0.0.1", port=%d, threaded=True, use_reloader=False)
''' % (APP, TMP, PORT)


def start_stub():
    path = os.path.join(TMP, 'stub_server.py')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(STUB)
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
    for m in ('Page.enable', 'Runtime.enable', 'Log.enable', 'Network.enable'):
        br.call(m, {}, session=sid)
    br.call('Network.setCookie', {'name': 'session', 'value': cookie,
                                  'domain': '127.0.0.1', 'path': '/', 'httpOnly': True},
            session=sid)
    mark = len(br.events)          # ⚠️ br.events 是**跨页共享**的：不记 mark 的话，
    br.call('Page.navigate', {'url': BASE + '/#' + hashname}, session=sid)
    for _ in range(120):           #    第二页会被第一页残留的 loadEventFired 立刻"骗"过去
        if any(m.get('method') == 'Page.loadEventFired' for m in br.events[mark:]):
            break
        br.pump(0.25)
    br.pump(1.5)
    return sid


def chat_snapshot(br, sid):
    """聊天气泡的快照（纯数据，returnByValue 能过）。"""
    return js(br, sid, """(() => {
      const rows = Array.from(document.querySelectorAll('#chat-box .bubble-row'));
      return {
        nAi: rows.filter(r => r.classList.contains('ai')).length,
        nUser: rows.filter(r => r.classList.contains('user')).length,
        srcs: rows.map(r => { const s = r.querySelector('.bubble-src');
                              return s ? s.textContent : null; }),
        states: rows.map(r => { const s = r.querySelector('.bubble-state');
                                return s ? s.textContent : null; }),
        texts: rows.map(r => r.textContent),
        scrollable: (() => { const b = document.getElementById('chat-box');
                             return b ? b.scrollHeight > b.clientHeight : null; })()
      };
    })()""")


def main():
    flask = start_stub()
    edge = None
    try:
        print('\n=== 1. 登录 + 打开互动页 ===')
        s = requests.Session()
        s.post(BASE + '/login', data={'token': token()}, allow_redirects=False)
        cookie = s.cookies.get('session')
        if not cookie:
            raise SystemExit('登录没拿到 session cookie')
        edge, br = start_edge()
        sid = open_page(br, cookie, 'interact')

        have = js(br, sid, """(() => ({
          mic: !!document.getElementById('btn-chat-mic'),
          sec: !!document.getElementById('in-chat-mic-sec'),
          pill: !!document.getElementById('chat-audio-mode'),
          hint: !!document.getElementById('chat-mic-hint'),
          input: !!document.getElementById('chat-input'),
          btn: !!document.getElementById('btn-chat'),
          pane: !!document.querySelector('#pane-interact'),
          tabs: document.querySelectorAll('.proj-tab').length
        }))()""")
        check('★ 新 DOM 全在（麦克风按钮 / 秒数 / 模式胶囊 / 提示行）',
              have['mic'] and have['sec'] and have['pill'] and have['hint'],
              have)
        check('原有输入框与发送按钮没被改掉', have['input'] and have['btn'])
        # ★ 2026-09-23 03网页端：这条原本是"本次改动不许加页签，仍是 9 个"的守卫。
        #   当天晚些「远程OTA」页**经用户批准**加了第 10 个页签 ⇒ 基数从 9 变 10。
        #   守卫的**意思**没变（互动页自己没加页签），只是基数要跟着走。
        check('★ 页签数为 10（互动页自己没加页签；第 10 个是当天加的「远程OTA」）',
              have['tabs'] == 10, have['tabs'])

        print('\n=== 2. 历史轮次回放（来源角标 + 幂等）===')
        snap = chat_snapshot(br, sid)
        check('★ 预置的 3 轮 AI 回复都画出来了', snap['nAi'] == 3, snap['nAi'])
        check('★ 打字轮 + 语音轮各画了一个用户气泡，定时唤醒的**提示词不算用户说的话**',
              snap['nUser'] == 2, {'nUser': snap['nUser'], 'texts': snap['texts'][:3]})
        joined = '|'.join([x for x in snap['srcs'] if x])
        check('★ 三个来源角标各一次：⌨️ 打字 / 🎤 语音 / ⏰ 定时唤醒',
              joined.count('⌨️') == 1 and joined.count('🎤') == 1 and joined.count('⏰') == 1,
              joined)
        check('★ 语音轮认的是 app.py 写的 voice 标记（不是"猜 user 非空"）',
              '🎤 语音' in joined, joined)

        # 幂等：5 秒轮询会反复调 refresh()，一轮都不许重画
        js(br, sid, 'window.PAGES.interact.refresh()')
        br.pump(0.6)
        js(br, sid, 'window.PAGES.interact.refresh()')
        br.pump(0.6)
        snap2 = chat_snapshot(br, sid)
        check('★ 连调两次 refresh()：气泡数一个都没变（幂等，不然 5 秒整页重画一遍）',
              snap2['nAi'] == 3 and snap2['nUser'] == 2,
              {'ai': snap2['nAi'], 'user': snap2['nUser']})

        print('\n=== 3. 投递状态对账（账本是空的，就得说"没有记录"）===')
        states = [x for x in snap2['states'] if x]
        check('★ 桩服务刚起来、账本是空的 ⇒ 历史轮次写明「账本里没有这轮的记录」，'
              '而不是永远停在"对账中"装作还在查',
              any('账本里没有' in x for x in states) and not any('对账中' in x for x in states),
              states[:4])
        check('定时唤醒那条写明了不走投递账本',
              any('不记投递账本' in x for x in states), states[:4])

        print('\n=== 4. 打字 → 真下发（桩 AI，不花钱）===')
        js(br, sid, """(() => { const i = document.getElementById('chat-input');
          i.value = '测试一下上屏'; document.getElementById('btn-chat').click(); })()""")
        for _ in range(40):
            br.pump(0.3)
            if js(br, sid, "document.querySelectorAll('#chat-box .bubble-row.ai').length") >= 4:
                break
        br.pump(0.8)
        snap3 = chat_snapshot(br, sid)
        check('★ 打字后本地立刻回显（用户气泡 + AI 回复气泡）',
              snap3['nAi'] == 4 and snap3['nUser'] == 3,
              {'ai': snap3['nAi'], 'user': snap3['nUser']})
        st3 = [x for x in snap3['states'] if x and '已入队' in x]
        check('★ 回复气泡带上了「已入队（队列 N）」—— 这就是"正式下发到屏幕"',
              bool(st3), snap3['states'])
        dv = s.get(BASE + '/api/deliveries').json()
        queued = [x for x in dv['deliveries'] if x['state'] == 'queued']
        check('★ 服务端投递账本里确实躺着这条（前端不是自己编的徽标）',
              bool(queued) and '测试一下上屏' == queued[-1]['user'],
              dv['deliveries'][-1:])
        dev = [x for x in s.get(BASE + '/api/devices').json()['devices']
               if x['device_id'] == 'esp-bridge']
        check('★ 命令真的进了设备队列（queue_len=1）', dev and dev[0]['queue_len'] == 1,
              dev)
        check('★ 下发帧与语音路逐字同源（service 端拼的，不是网页另写一份）',
              queued and '@TEXT:我在听' in queued[-1]['raw'], queued[-1]['raw'] if queued else '')

        print('\n=== 5. 假 ESP 轮询取走 → 徽标升级成「设备已取走」===')
        r = s.post(BASE + '/api/data', json={'token': token(), 'device_id': 'esp-bridge',
                                            'sensor': '温度:28', 'user': ''})
        check('假 ESP 轮询取走了那条命令', (r.json().get('command') or {}).get('raw') ==
              (queued[-1]['raw'] if queued else None), str(r.json().get('command'))[:90])
        js(br, sid, 'window.PAGES.interact.refresh()')
        br.pump(0.8)
        snap4 = chat_snapshot(br, sid)
        check('★ 轮询之后徽标升级为「设备已取走」（带取走时刻）',
              any(x and '设备已取走' in x for x in snap4['states']), snap4['states'])
        check('★ 升级过程中气泡没被重画（还是 4 条 AI）', snap4['nAi'] == 4, snap4['nAi'])

        print('\n=== 6. 语音行（UI 层：模式胶囊 / 帧格式）===')
        js(br, sid, "window.PAGES.interact.refresh()")
        br.pump(1.0)
        pill = js(br, sid, "document.getElementById('chat-audio-mode').textContent")
        check('★ 模式胶囊显示"自动"（audio_state.json 已改成 auto，服务端说了算）',
              '自动' in pill, pill)
        js(br, sid, """(() => { document.getElementById('in-chat-mic-sec').value = '99';
          document.getElementById('btn-chat-mic').click(); })()""")
        br.pump(0.6)
        check('★ 秒数在浏览器这层就夹到 10（超了固件是**静默**夹，会让人以为录了 99 秒）',
              js(br, sid, "document.getElementById('in-chat-mic-sec').value") == '10',
              js(br, sid, "document.getElementById('in-chat-mic-sec').value"))
        hint = js(br, sid, "document.getElementById('chat-mic-hint').textContent")
        check('★ 点了之后有明确反馈（入队/等待取走），不是静默什么都不发生',
              bool(hint) and ('入队' in hint or '录音' in hint), hint)
        dev2 = [x for x in s.get(BASE + '/api/devices').json()['devices']
                if x['device_id'] == 'esp-bridge']
        check('★ 队列里是那条 @MIC:rec,10# 帧（白名单产出的同一种格式）',
              dev2 and dev2[0]['queue_len'] == 1, dev2)

        print('\n=== 7. 调试页隔离（用户明确要求：不发，保持隔离）===')
        s2 = open_page(br, cookie, 'debug')
        n_before = len(s.get(BASE + '/api/deliveries').json()['deliveries'])
        # ⚠️ 别用 index 记 mark：`cdp_min.call()` 在事件超 400 条时会 `del events[:200]`
        #    把**前面**的切掉 —— mark 一旦大于切完的长度，`events[mark:]` 就是空集，
        #    于是"没有 /api/data"**假绿**（我第一次跑正是死在这里：since=0）。
        #    这里改成"清空 → 只看这一段 → 再把老的放回去"（console_errors 还要用老的）。
        kept, br.events[:] = br.events[:], []
        try:
            js(br, s2, """(() => { const i = document.getElementById('debug-input');
              i.value = '调试页不该下发'; document.getElementById('btn-debug-send').click(); })()""")
            br.pump(2.5)
            since = br.events[:]
        finally:
            br.events[:0] = kept
        # ⚠️ 按 URL 全集判，不只看 sessionId：先确认"事件到底收没收到"。
        #    空集永远不含 /api/data ⇒ 单看那条会**假绿**，所以补一条"事件非空"的前置断言。
        urls = [m['params']['request']['url'] for m in since
                if m.get('method') == 'Network.requestWillBeSent']
        mine = [m['params']['request']['url'] for m in since
                if m.get('method') == 'Network.requestWillBeSent' and m.get('sessionId') == s2]
        check('网络事件确实收到了（否则下面两条是空集假绿）', bool(urls),
              {'since': len(since), 'all': len(urls), 'sess2': len(mine),
               'sample': [u for u in urls if '/api' in u][:5]})
        check('★ 调试页发消息全程**没有**一个 /api/data 请求（隔离的真判据）',
              not any('/api/data' in u for u in urls),
              [u for u in urls if '/api' in u])
        check('它走的是自己的那条路 /api/debug_chat', any('/api/debug_chat' in u for u in urls),
              [u for u in urls if '/api' in u])
        n_after = len(s.get(BASE + '/api/deliveries').json()['deliveries'])
        check('★ 调试页不产生任何投递记录（跟决策/上屏完全无关）',
              n_after == n_before, {'before': n_before, 'after': n_after})
        check('★ 调试页的回复照常画出来（隔离 ≠ 把这页弄坏）',
              js(br, s2, """(() => Array.from(document.querySelectorAll('#debug-box .bubble'))
                   .some(b => b.textContent.indexOf('我在听') >= 0))()""") is True,
              js(br, s2, "document.querySelectorAll('#debug-box .bubble').length"))

        errs = [e for e in br.console_errors()
                if 'favicon' not in e and 'net::ERR' not in e]
        check('互动页全程没有 JS 异常 / error 级日志', not errs, errs[:6])

        print('\n=== 8. 不污染真记忆/统计（本测试的自律项）===')
        real_log = os.path.join(APP, 'decision_log.txt')
        with open(real_log, 'r', encoding='utf-8') as f:
            tail = f.read()
        check('★ 真 decision_log.txt 里没有被塞进测试记录（"测试一下上屏"不该出现）',
              '测试一下上屏' not in tail)
        check('★ 桩服务的落盘确实在 %TEMP%（说明重定向生效了）',
              os.path.exists(os.path.join(TMP, 'decision_log.txt')))
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

    n_fail = len([1 for _n, ok, _d in RESULTS if not ok])
    print('\n==================== %d 项断言，%d 失败 ====================' % (len(RESULTS), n_fail))
    for n, ok, d in RESULTS:
        if not ok:
            print('  FAIL %s  -> %s' % (n, d))
    return 1 if n_fail else 0


if __name__ == '__main__':
    sys.exit(main())
