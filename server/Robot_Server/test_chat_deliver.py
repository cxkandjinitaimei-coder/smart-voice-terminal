# -*- coding: utf-8 -*-
"""网页对话/互动上屏 验收（2026-09-23，03网页端）—— 纯函数 + 真 Flask，**不碰硬件**。

跑法：  cd d:\\ArduinoIDE\\ArdProject\\flask_relay
        D:\\Python\\python.exe test_chat_deliver.py
        （**必须用 D:\\Python\\python.exe** —— PowerShell 会把无 BOM 的 UTF-8 按 GBK 读，
          中文断言全烂）

验的是三件事：
  ① `_screen_text()`：`@TEXT:` 载荷**不许越过 F407 的两道静默丢弃线**
     （`esp_uart.c:333` rx_used>=191、`ailink.c:605` strlen>=192 —— 越了就是"AI 说了、
     屏上什么都没有、零报错"）。
  ② `decision_to_raw()`：angle 的 `or 90` 把合法 0 改掉 / 浮点带单位静默吞 move /
     舵机软限位不夹 —— 三个既存坑。
  ③ `api_data()` 的 `deliver` / `no_pop`：**该下发的下发、不该领的命令一条都不许领**
     （浏览器把命令 pop 走 = 那条命令永久消失）。

★ 三条自律，别省：
  - `append_line` 一律打桩 —— `decision_log.txt` 是 **AI 的记忆源**，测试往里灌假行
    会让机器人"记得"从没发生过的事。
  - `store.record_*` 一律打桩**并记调用** —— 真写 SQLite 会让统计页出现假平线，
    而"到底有没有落库"本身是要断言的。
  - `ai_relay.get_ai_decision` 一律打桩 —— 不打真 API（花钱 + 不确定）。
"""
import json
import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import requests                                        # noqa: E402
from werkzeug.serving import make_server               # noqa: E402

import app as A                                        # noqa: E402

RESULTS = []
DEV = A.DEFAULT_DEVICE_ID


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok), detail))
    print(('  [OK]   ' if ok else '  [FAIL] ') + name + (('  -> ' + str(detail)) if detail else ''))
    return ok


def token():
    with open(os.path.join(A.BASE_DIR, 'config.json'), 'r', encoding='utf-8') as f:
        return json.load(f).get('token', '')


def segs(raw):
    """ESP 会按 `#` 把 raw 拆成多行再发 F407 ⇒ 逐段核有没有越过 192。"""
    return [s for s in str(raw or '').split('#')]


def max_seg(raw):
    return max([len(s.encode('utf-8')) for s in segs(raw)] or [0])


HAN = '你好'
CJK200 = '今天天气不错我很开心看到你回来了要不要一起聊聊天呢' * 5      # 250 汉字


# ==================== ① _screen_text 纯函数 ====================
def test_screen_text():
    print('\n=== ① _screen_text()：上屏文字闸门 ===')
    SHORT = 'HELLO'
    check('短 ASCII 原样返回', A._screen_text(SHORT) == (SHORT, False))

    s55 = HAN * 27 + '好'[:0]        # 54 汉字 = 162 字节
    t, cut = A._screen_text(s55)
    check('54 汉字（162B）不切', (t == s55) and (cut is False), '%d 字节' % len(t.encode('utf-8')))

    s56 = HAN * 28                   # 56 汉字 = 168 字节，正好到界
    t, cut = A._screen_text(s56)
    check('56 汉字（168B）正好到界不切', (t == s56) and (cut is False),
          '%d 字节 / %d 格' % (len(t.encode('utf-8')), sum(A._text_cells(c) for c in t)))

    s57 = HAN * 28 + '好'            # 57 汉字 = 171 字节，必须切
    t, cut = A._screen_text(s57)
    check('57 汉字（171B）切掉', (cut is True) and len(t.encode('utf-8')) <= A.SCREEN_TEXT_MAX_BYTES,
          '%d 字节' % len(t.encode('utf-8')))

    # 250 汉字：切点必须落在句末标点上（读起来是一句完整的话）
    sent = ('今天天气不错，我很开心看到你回来了。要不要一起聊聊天呢？' * 12)[:250]
    t, cut = A._screen_text(sent)
    check('250 汉字：在句末标点处切', cut is True and t[-1] in A.SCREEN_BREAK_CHARS,
          '结尾=%r 长度=%d' % (t[-1], len(t)))
    check('250 汉字：字节 <= 上限', len(t.encode('utf-8')) <= A.SCREEN_TEXT_MAX_BYTES,
          '%d 字节' % len(t.encode('utf-8')))

    # 纯 ASCII：格数先到顶（1 字符 = 1 格），字节不是约束
    t, cut = A._screen_text('A' * 400)
    cells = sum(A._text_cells(c) for c in t)
    check('400 ASCII：按格数切（<=160 格）', cut is True and cells <= A.SCREEN_TEXT_MAX_CELLS,
          '%d 字符 / %d 格' % (len(t), cells))

    check('换行折成空格（ESP 那条会被 \n 切成两行）',
          A._screen_text('第一行\n第二行')[0] == '第一行 第二行')
    check('@ 与 # 被剥掉（它们是拆帧字符）',
          '#' not in A._screen_text('a#b@c')[0] and '@' not in A._screen_text('a#b@c')[0])
    check('空串 / None → 空、不截断', A._screen_text('') == ('', False)
          and A._screen_text(None) == ('', False))
    check('emoji（非 BMP）不炸、按 2 格算',
          A._text_cells('😀') == 2 and A._screen_text('😀' * 100)[1] is True)

    # 不变式：任何输入都不许越线
    bad = []
    for probe in (SHORT, s55, s56, s57, CJK200, 'A' * 400, sent, '，' * 200, 'a b' * 200):
        out, _c = A._screen_text(probe)
        if len(out.encode('utf-8')) > A.SCREEN_TEXT_MAX_BYTES:
            bad.append(('bytes', probe[:12]))
        if sum(A._text_cells(c) for c in out) > A.SCREEN_TEXT_MAX_CELLS:
            bad.append(('cells', probe[:12]))
        if len(('@TEXT:' + out).encode('utf-8')) >= 191:
            bad.append(('line>=191', probe[:12]))
    check('不变式：所有探针 字节/格数/整行 <191 全不越线', not bad, bad[:3])


# ==================== ② decision_to_raw ====================
def test_decision_to_raw():
    print('\n=== ② decision_to_raw()：angle / 软限位 / 上屏闸门 ===')

    r = A.decision_to_raw({'moves': [{'type': 'servo', 'id': 2, 'angle': 0}]})
    check('angle=0 就是 0（原先 `or 90` 会改成 90）', r == '@MOVE:servo,2,0', r)

    # 浮点/带单位：原先 `int(m.get("angle") or 90)` 会 ValueError 把整条 move 静默吞掉
    r = A.decision_to_raw({'moves': [{'type': 'servo', 'id': 1, 'angle': 90.6}]})
    check('angle=90.6 四舍五入成 91（原先静默吞掉整条 move）', r == '@MOVE:servo,1,91', r)
    r = A.decision_to_raw({'moves': [{'type': 'servo', 'id': 2, 'angle': '120'}]})
    check('angle="120"（数字字符串）解得开，按S2上限夹到90', r == '@MOVE:servo,2,90', r)

    r = A.decision_to_raw({'moves': [{'type': 'servo', 'id': 1, 'angle': '90度'}]})
    check('angle="90度" 解不出来 → 跳过该条（不静默）', r == '', r)

    r = A.decision_to_raw({'moves': [{'type': 'gesture', 'name': 'nod'},
                                     {'type': 'expression', 'face': 'happy'}]})
    check('gesture / expression 照旧保留', r == '@MOVE:gesture,nod#@MOVE:expression,happy', r)

    # 软限位按当前唯一硬件契约：S1 15~165，S2 0~90，S3 0~180。
    cl = []
    r = A.decision_to_raw({'moves': [{'type': 'servo', 'id': 1, 'angle': 0}]}, cl)
    check('id1 angle=0 被夹到 15 且记下夹取', r == '@MOVE:servo,1,15' and cl
          and '0→15' in cl[0], '%s | %s' % (r, cl))

    cl = []
    r = A.decision_to_raw({'moves': [{'type': 'servo', 'id': 1, 'angle': 200}]}, cl)
    check('id1 angle=200 被夹到 165', r == '@MOVE:servo,1,165' and '200→165' in cl[0], r)

    cl = []
    r = A.decision_to_raw({'moves': [{'type': 'servo', 'id': 2, 'angle': 0}]}, cl)
    check('id2 行程 0~90：0 不夹、不改', r == '@MOVE:servo,2,0' and not cl, '%s | %s' % (r, cl))

    cl = []
    r = A.decision_to_raw({'moves': [{'type': 'servo', 'id': 9, 'angle': 5}]}, cl)
    check('未登记的 id 不夹（config 里只有 1/2）', r == '@MOVE:servo,9,5' and not cl, r)

    # 上屏闸门接在 decision_to_raw 里
    r = A.decision_to_raw({'message': CJK200})
    check('超长 message 走闸门，整行 <191', max_seg(r) < 191, 'max_seg=%d' % max_seg(r))

    # 不变式：一整组决策里每一段都不越线
    probes = [
        {'text': 'HELLO', 'moves': [{'type': 'servo', 'id': 1, 'angle': 100}]},
        {'message': CJK200, 'moves': [{'type': 'gesture', 'name': 'nod'}]},
        {'text': 'A' * 300},
        {'moves': [{'type': 'servo', 'id': 1, 'angle': 0}, {'type': 'servo', 'id': 2, 'angle': 180}]},
    ]
    bad = [(A.max_seg if False else max_seg(A.decision_to_raw(p)), i) for i, p in enumerate(probes)]
    check('不变式：每条决策拆帧后都 <191 字节', all(m < 191 for m, _i in bad), bad)


# ==================== ③ /api/data 的 deliver / no_pop ====================
AI_ENV = {
    'message': '看到你啦，今天过得怎么样？我一直在这儿等你回来呢。',
    'text': '欢迎回来',
    'moves': [{'type': 'servo', 'id': 1, 'angle': 0},
              {'type': 'gesture', 'name': 'nod'},
              {'type': 'expression', 'face': 'happy'}],
    'raw': '{"这是大模型的原始输出，不是帧串":true}',
}


def start_server():
    srv = make_server('127.0.0.1', 0, A.app)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, 'http://127.0.0.1:%d' % srv.server_port


def test_endpoints():
    print('\n=== ③ /api/data：deliver / no_pop（真 Flask，AI 打桩）===')
    srv, base = start_server()

    # ---- 打桩：不打真 AI、不写记忆、不写统计库 ----
    calls = {'log': [], 'cmd': [], 'inter': []}
    saved = (A.append_line, A.ai_relay.get_ai_decision,
             A.store.record_command, A.store.record_interaction, A.store.record_sensor)
    A.append_line = lambda path, line: calls['log'].append((path, line))
    A.ai_relay.get_ai_decision = lambda cfg, msgs: (json.dumps(AI_ENV), {'provider': 'stub'})
    A.store.record_command = lambda *a, **k: calls['cmd'].append((a, k))
    A.store.record_interaction = lambda *a, **k: calls['inter'].append((a, k))
    A.store.record_sensor = lambda *a, **k: None

    try:
        s = requests.Session()
        s.post(base + '/login', data={'token': token()}, allow_redirects=False)

        def dev(devid=DEV):
            j = s.get(base + '/api/devices').json()
            d = [x for x in (j.get('devices') or []) if x.get('device_id') == devid]
            return (d[0] if d else {}), j

        def data(body):
            r = s.post(base + '/api/data', json=body)
            return r.status_code, (r.json() if r.content else {})

        # 清干净队列与在线状态，从确定状态开始
        A._pending_commands.pop(DEV, None)
        A._device_last_seen.pop(DEV, None)

        # --- 1) 设备离线：拒发，不入队 ---
        A._device_last_seen.pop(DEV, None)
        A._device_last_seen[DEV] = A.datetime.now() - A.timedelta(seconds=200)
        before = A._queue_len(DEV)
        code, j = data({'device_id': DEV, 'user': '你好', 'need_ai': True, 'deliver': True})
        check('离线时 deliver：200 且 refused_offline（不 502）',
              code == 200 and j.get('deliver_state') == 'refused_offline',
              '%s / %s' % (code, j.get('deliver_err')))
        check('离线时 deliver：队列不长', A._queue_len(DEV) == before, before)

        # --- 2) 在线 + deliver：入队，且这次请求**不算设备上报** ---
        A._device_last_seen[DEV] = A.datetime.now()
        seen_before = A._device_last_seen[DEV]
        calls['cmd'].clear()
        code, j = data({'device_id': DEV, 'user': '你好呀', 'need_ai': True, 'deliver': True})
        check('在线 deliver：deliver_state=queued', j.get('deliver_state') == 'queued',
              str(j.get('deliver_state')))
        check('在线 deliver：queue_len=1', j.get('queue_len') == 1 and A._queue_len(DEV) == 1)
        check('在线 deliver：不刷新"设备在线"（浏览器不能伪造上报）',
              A._device_last_seen[DEV] == seen_before)
        check('在线 deliver：device_frames 非空且每段 <191',
              j.get('device_frames') and max_seg(j.get('device_frames')) < 191,
              j.get('device_frames'))
        check('在线 deliver：文字+动作都下发（@TEXT: 与 @MOVE:gesture 同在）',
              '@TEXT:' in (j.get('device_frames') or '')
              and '@MOVE:gesture,nod' in (j.get('device_frames') or ''))
        check('在线 deliver：servo_clamped 记下了 id1 0→15',
              any('0→15' in x for x in (j.get('servo_clamped') or [])),
              str(j.get('servo_clamped')))
        check('在线 deliver：command 落库 source=web_chat（统计/命令历史不瞎）',
              calls['cmd'] and calls['cmd'][0][1].get('source') == 'web_chat',
              str(calls['cmd'][:1]))

        # --- 3) 账本：queued → (设备取走) → taken ---
        dv = s.get(base + '/api/deliveries').json()
        rec = [x for x in dv.get('deliveries') or [] if x.get('state') == 'queued']
        check('/api/deliveries 里有这条 queued', bool(rec), dv.get('deliveries')[-1:])
        did = rec[-1]['id'] if rec else None

        code, j2 = data({'device_id': DEV, 'user': '', 'sensor': '温度:28.5'})
        check('设备轮询（空 user）取走了那条 command',
              (j2.get('command') or {}).get('_did') == did, str(j2.get('command'))[:120])
        dv = s.get(base + '/api/deliveries').json()
        got = [x for x in dv.get('deliveries') or [] if x.get('id') == did]
        check('账本随之变 taken（带 pop_at）',
              got and got[0]['state'] == 'taken' and got[0]['pop_at'], got[:1])

        # --- 4) 空轮询绝不许灌队列（问题日志那条 ❌ 的钉子）---
        before = A._queue_len(DEV)
        for _ in range(3):
            data({'device_id': DEV, 'user': '', 'sensor': '温度:28.5'})
        check('ESP 式空轮询 x3：队列一直是 0（绝不灌"已记录（未调用AI）"）',
              A._queue_len(DEV) == before == 0, A._queue_len(DEV))
        check('空轮询响应里没有假的 @TEXT: 帧',
              '已记录' not in str(j2.get('device_frames') or '') and 'device_frames' not in j2,
              str(list(j2.keys())))

        # --- 5) 抢单回归：浏览器请求不许领命令 ---
        #   三个真前端调用点就这三处：page-interact.js:129（deliver）、
        #   page-test.js:377 / :410（no_pop）。每处都要能把命令**留住**。
        MARK = '@LED_ON,LED1'

        def reseed():
            A._pending_commands.pop(DEV, None)
            A.enqueue_command(DEV, {'time': A.now_str(), 'device_id': DEV,
                                    'raw': MARK, 'note': '抢单回归'})

        def queue_raws():
            return [c.get('raw') for c in list(A._pending_commands.get(DEV) or [])]

        for tag, body in (('互动页 deliver', {'user': '在吗', 'need_ai': True, 'deliver': True}),
                          ('测试页 模拟异常 no_pop', {'user': '异常', 'need_ai': True, 'no_pop': True}),
                          ('测试页 手动上报 no_pop', {'user': '你好', 'mock': True, 'no_pop': True})):
            reseed()
            _c, jj = data(dict(body, device_id=DEV))
            check('浏览器请求（%s）不领命令：那条命令还在队列里' % tag,
                  MARK in queue_raws() and 'command' not in jj,
                  'queue=%s command=%s' % (queue_raws(), jj.get('command')))
        #  反面钉：不设任何标志的裸 POST 按契约就是"当成设备轮询" ⇒ 会领走。
        #  钉住它是为了让"以后再加前端调用点时忘了带标志"立刻炸出来。
        reseed()
        _c, jj = data({'device_id': DEV, 'user': '在吗', 'need_ai': True})
        check('（契约）不带任何标志的裸 POST 会领走命令 —— 新增前端调用点必须带标志',
              jj.get('command', {}).get('raw') == MARK, str(jj.get('command'))[:80])

        reseed()
        data({'device_id': DEV, 'user': '', 'sensor': '温度:1'})
        check('设备轮询（user="")才领得走：队列回到 0', A._queue_len(DEV) == 0, queue_raws())

        # --- 6) 01语音 的契约不许被我改坏 ---
        code, j3 = data({'device_id': DEV, 'user': '语音回灌', 'need_ai': True,
                         'enqueue_decision': True})
        check('enqueue_decision 仍返回 queued_to_device=True（01语音 的 inject() 靠它）',
              j3.get('queued_to_device') is True and code == 200,
              '%s / %s' % (code, j3.get('queued_to_device')))
        A._pending_commands.pop(DEV, None)

        # --- 7) 上屏文字：message 全文进记忆，text 才是屏上那一行 ---
        calls['log'].clear()
        #  AI 只给了 message（没给 text）⇒ 走隧道的正是那条长 message
        long_ai = dict(AI_ENV, message=CJK200)
        long_ai.pop('text', None)
        A.ai_relay.get_ai_decision = lambda cfg, msgs: (json.dumps(long_ai), {'provider': 'stub'})
        code, j4 = data({'device_id': DEV, 'user': '说长一点', 'need_ai': True, 'deliver': True})
        check('长回复：响应 text 被裁到 <=168 字节 且 screen_truncated=True',
              len(str(j4.get('text', '')).encode('utf-8')) <= 168 <= len(CJK200.encode('utf-8'))
              and j4.get('screen_truncated') is True,
              '%d 字节 / truncated=%s' % (len(str(j4.get('text', '')).encode('utf-8')),
                                          j4.get('screen_truncated')))
        check('长回复：message 仍是全文（记忆源与统计不许被截断）',
              j4.get('message') == CJK200)
        logged = [json.loads(x) for p, x in calls['log']
                  if p == A.DECISION_LOG and '说长一点' in x]
        check('落 decision_log 的那条 message 也是全文（AI 的记忆不许被截断）',
              logged and logged[0].get('message') == CJK200,
              ('%d 字节' % len(str(logged[0].get('message', '')).encode('utf-8'))) if logged
              else ('no log; got=%s' % [x[:60] for x in calls['log']]))
        #  记忆是**端到端**验的：把桩日志喂给真的记忆读取器，看它读回来的是不是全文。
        #  （只断言"我写进去的是全文"没用 —— 记忆源到底是哪个字段由 ai_relay 决定。）
        _saved_tail, A.read_tail = A.read_tail, (
            lambda path, n: [x for p, x in calls['log'] if p == path])
        try:
            _h, decs = A.load_memory_context({'memory_window': 10, 'memory_turns': 6,
                                              'memory_digest_lines': 80})
            mine = [d for d in decs if d.get('user') == '说长一点']
            check('记忆读取器读回的 message 是全文（AI 不会以为自己在说短句）',
                  mine and mine[0].get('message') == CJK200,
                  ('%d 字节' % len(str(mine[0].get('message', '')).encode('utf-8')))
                  if mine else '本轮没进记忆')
        finally:
            A.read_tail = _saved_tail
        check('长回复：下发帧每段仍 <191', max_seg(j4.get('device_frames')) < 191,
              max_seg(j4.get('device_frames')))

        # --- 8) mock + deliver：不花钱就能验整条链 ---
        A._pending_commands.pop(DEV, None)
        A._device_last_seen[DEV] = A.datetime.now()
        code, j5 = data({'device_id': DEV, 'user': '测试模式', 'mock': True, 'deliver': True})
        check('mock + deliver 允许下发（不用真花钱就能验链路）',
              j5.get('deliver_state') == 'queued'
              and '@MOVE:servo,1,100' in (j5.get('device_frames') or ''),
              '%s | %s' % (j5.get('deliver_state'), j5.get('device_frames')))
        A._pending_commands.pop(DEV, None)

        # --- 9) 前端契约：互动页 JS 真正读的那些字段名，一个都不许改 ---
        #   字段名是 **JS 里写死**的（page-interact.js:227/238/312-316/321），
        #   服务端这边改名**不会报错**：网页照常画回复，只是"已入队/未下发"那些
        #   徽标永远不出现 —— 静默失效，光看页面像"这条功能没做"。
        #   所以在这里把"JS 读过谁"钉成断言：改名就得同时改 JS，一改就红。
        #   钉的清单是 grep `page-interact.js` 数出来的，不是照抄 app.py。
        A._pending_commands.pop(DEV, None)
        A._device_last_seen[DEV] = A.datetime.now()
        code, j6 = data({'device_id': DEV, 'user': '契约', 'need_ai': True, 'deliver': True})
        need = ('message', 'time', 'deliver_state', 'deliver_err', 'queue_len',
                'deliver_id', 'servo_clamped', 'device_frames')
        missing = [k for k in need if k not in j6]
        check('★ 前端契约：deliver 响应带着互动页要读的全部字段（缺一个=那条徽标永远不显示）',
              not missing, {'missing': missing, 'got': sorted(j6)})
        check('★ 前端契约：time 与 /api/history 的口径一致（本地回显去重就靠它）',
              j6.get('time') and len(str(j6.get('time'))) == 19, j6.get('time'))
        #  反面钉：**拒发**时不许给 deliver_id。JS 是 `did != null` 才去 /api/deliveries
        #  对账的，如果拒发也塞一个 id，徽标就会先显示"已入队"再被对账改成别的。
        A._pending_commands.pop(DEV, None)
        A._device_last_seen[DEV] = A.datetime.now() - A.timedelta(seconds=300)
        _c, j7 = data({'device_id': DEV, 'user': '离线契约', 'need_ai': True, 'deliver': True})
        check('★ 前端契约：拒发（离线）时 deliver_state/err 在、deliver_id 不在',
              j7.get('deliver_state') == 'refused_offline' and j7.get('deliver_err')
              and 'deliver_id' not in j7,
              {k: v for k, v in j7.items() if k.startswith('deliver')})
        A._device_last_seen.pop(DEV, None)

    finally:
        (A.append_line, A.ai_relay.get_ai_decision,
         A.store.record_command, A.store.record_interaction, A.store.record_sensor) = saved
        A._pending_commands.pop(DEV, None)
        A._device_last_seen.pop(DEV, None)
        srv.shutdown()


def main():
    print('网页对话/互动上屏 验收（不打真 AI、不写记忆/统计库、不碰硬件）')
    test_screen_text()
    test_decision_to_raw()
    test_endpoints()
    n_fail = len([1 for _n, ok, _d in RESULTS if not ok])
    print('\n==================== %d 项断言，%d 失败 ====================' % (len(RESULTS), n_fail))
    for n, ok, d in RESULTS:
        if not ok:
            print('  FAIL %s  -> %s' % (n, d))
    return 1 if n_fail else 0


if __name__ == '__main__':
    sys.exit(main())
