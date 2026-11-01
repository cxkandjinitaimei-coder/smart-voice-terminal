# -*- coding: utf-8 -*-
"""姿态页（三维 + 搭积木元件清单）验收 —— headless Edge + 手写 CDP。

环境里没有 websocket / websockets 库，所以 CDP 客户端是手写的（见同目录 cdp_min.py）。

跑法：  cd d:\\ArduinoIDE\\ArdProject\\flask_relay
        D:\\Python\\python.exe test_pose_page.py
        （**必须用 D:\\Python\\python.exe** —— PowerShell 会把无 BOM 的 UTF-8 按 GBK 读，
          中文断言全烂）

它会自己起 Flask（5000 端口已占用则复用）、起两个 headless Edge
（一个正常、一个 --disable-3d-apis 测降级），跑完自己清场
（只 kill 自己起的那两个，**不碰用户 webctl 在 9222 上的**）。

⚠️ 这些断言是"改坏能红"的：本轮的 71 项里实际红过两次，两次都是**真 bug**
  （迁移搬完不落盘、换数据却不重画清单）—— 别因为红了就先把断言删掉。
"""
import json
import os
import shutil
import socket
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
PORT = 9333
BASE = 'http://127.0.0.1:5000'
# ★ 中间产物（Edge profile、截图、Flask 日志）一律丢系统临时目录，
#   别落在项目里 —— 否则会被地图体检当成代码文件，也会脏了工程目录。
TMP = os.path.join(tempfile.gettempdir(), 'pose_verify')
os.makedirs(TMP, exist_ok=True)          # 下面 open(日志) / profile 目录都要它先存在

RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok), detail))
    print(('  [OK]   ' if ok else '  [FAIL] ') + name + (('  -> ' + str(detail)) if detail else ''))
    return ok


def token():
    with open(os.path.join(APP, 'config.json'), 'r', encoding='utf-8') as f:
        return json.load(f).get('token', '')


# ---------------- 起服务 ----------------

def start_flask():
    if wait_port(5000, 1):
        print('  Flask 已经在 5000 上跑着，复用')
        return None
    logf = open(os.path.join(TMP, 'flask_out.log'), 'wb')
    p = subprocess.Popen([PY, 'app.py'], cwd=APP, stdout=logf, stderr=subprocess.STDOUT)
    if not wait_port(5000, 30):
        raise SystemExit('Flask 起不来，看 ' + os.path.join(TMP, 'flask_out.log'))
    print('  Flask 已起，pid=%d' % p.pid)
    return p


def login():
    s = requests.Session()
    r = s.post(BASE + '/login', data={'token': token()}, allow_redirects=False)
    ck = s.cookies.get('session')
    if not ck:
        raise SystemExit('登录没拿到 session cookie，status=%s' % r.status_code)
    print('  登录 ok，session=%s…' % ck[:12])
    return ck


PROFILE = os.path.join(TMP, 'edge_pose_profile')


def start_edge(extra=None, port=PORT):
    if os.path.isdir(PROFILE):
        shutil.rmtree(PROFILE, ignore_errors=True)
    args = [EDGE,
            '--headless=new',
            '--remote-debugging-port=%d' % port,
            '--user-data-dir=' + PROFILE,
            '--no-first-run', '--no-default-browser-check',
            '--disable-extensions', '--disable-sync',
            '--window-size=1280,1000',
            '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
            'about:blank']
    if extra:
        args[1:1] = extra
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_port(port, 30):
        raise SystemExit('Edge 的调试端口没起来')
    # 等 /json/version 真能应答
    for _ in range(40):
        try:
            br = Browser(port)
            print('  Edge 已起，pid=%d' % p.pid)
            return p, br
        except Exception:
            time.sleep(0.5)
    raise SystemExit('连不上 Edge 的 CDP')


def open_pose(br, cookie):
    sid, tid = br.new_page('about:blank')
    br.call('Page.enable', {}, session=sid)
    br.call('Runtime.enable', {}, session=sid)
    br.call('Log.enable', {}, session=sid)
    br.call('Network.enable', {}, session=sid)
    br.call('Network.setCookie', {
        'name': 'session', 'value': cookie,
        'domain': '127.0.0.1', 'path': '/', 'httpOnly': True,
    }, session=sid)
    br.call('Page.navigate', {'url': BASE + '/#pose'}, session=sid)
    # 等 loadEventFired
    for _ in range(120):
        if any(m.get('method') == 'Page.loadEventFired' for m in br.events):
            break
        br.pump(0.25)
    br.pump(1.2)
    return sid, tid


def js(br, sid, expr):
    return br.evaluate(expr, sid)


def ready_pose(br, sid, tries=40):
    """等 window.POSE_DEBUG 有内容（模块异步加载 + Three.js dynamic import）。"""
    for _ in range(tries):
        try:
            st = js(br, sid, 'window.POSE_DEBUG ? window.POSE_DEBUG.state() : null')
        except WsError:
            st = None
        if st and st.get('ready'):
            return st
        br.pump(0.3)
    return None


def world_map(br, sid):
    snap = js(br, sid, 'window.POSE_DEBUG.snapshot()')
    return {e['id']: e for e in snap}


def near(a, b, tol=1e-6):
    return abs(a - b) <= tol


def close(p):
    try:
        p.kill()
    except Exception:
        pass


# ---------------- 禁 WebGL 的降级路径 ----------------

PROFILE2 = os.path.join(TMP, 'edge_nowebgl_profile')


def phase_nowebgl(cookie):
    """--disable-3d-apis：3D 画不出来，但**元件清单照样能编辑**。
    这正是把 pose-parts.js 拆成普通脚本的收益，必须实测，不能只看代码。"""
    global PROFILE
    old = PROFILE
    PROFILE = PROFILE2
    try:
        edge, br = start_edge(['--disable-3d-apis'], port=9344)
    finally:
        PROFILE = old
    try:
        sid, tid = open_pose(br, cookie)
        for _ in range(40):
            if js(br, sid, "!!document.querySelector('#pose-stage .pose-fallback')"):
                break
            br.pump(0.3)
        fb = js(br, sid, "document.querySelector('#pose-stage .pose-fallback') ? "
                         "document.querySelector('#pose-stage .pose-fallback').textContent.slice(0,60) : null")
        check('禁 WebGL 时给出了降级说明', bool(fb), fb)
        check('降级文案里说清了"元件清单照样能编辑"', bool(fb) and '元件' in fb, fb)
        st = js(br, sid, "window.POSE_DEBUG.state()")
        check('状态标记为 dead，不再反复重试', st['dead'] is True, st)
        rows = js(br, sid, "document.querySelectorAll('#parts-list .part-row').length")
        check('★ 元件清单照样渲染出来了（9 行）', rows == 9, rows)
        js(br, sid, """(() => {
          const s = document.getElementById('parts-add-kind');
          s.value = 'cylinder'; document.getElementById('parts-add').click();
        })()""")
        br.pump(0.4)
        rows2 = js(br, sid, "document.querySelectorAll('#parts-list .part-row').length")
        check('★ 禁 WebGL 下还能「＋ 添加」元件（10 行）', rows2 == 10, rows2)
        n = js(br, sid, "window.POSE_PARTS.get().length")
        check('数据层同步增长', n == 10, n)
        fr = js(br, sid, "document.getElementById('pose-frame-preview').textContent")
        check('帧预览仍可用', fr == '@MOVE:servo,1,90#@MOVE:servo,2,90#', fr)

        # ★ A8：闸门拆开之后，这一档也必须"加一路舵机 → 抽屉多一条"。
        #   闸门没拆的话 renderAxisRows/syncAxes 全在 `if (!state.ready) return` 里面
        #   （state.ready 只在三维建起来之后才 true），这一档下抽屉会永远停在两条。
        #   而且旧写法在这儿**已经在撒谎**：PP.setAngle 内部的 syncServoDom 照跑
        #   （清单那行数字会变，看着像在工作），但 updateFramePreview() 不执行
        #   ⇒ "将要发出的帧"停在旧值，#pose-send 发的却是新角度。
        js(br, sid, """(() => { const s = document.getElementById('parts-add-kind');
          s.value = 'servo'; document.getElementById('parts-add').click(); })()""")
        br.pump(0.6)
        ax3 = js(br, sid, """(() => ({
          rows: Array.from(document.querySelectorAll('#pose-axes-list .pose-axis'))
                  .map(w => parseInt(w.getAttribute('data-servo'), 10)),
          servos: window.POSE_PARTS.servoIds() }))()""")
        check('★ A8 禁 WebGL 下加一路舵机 → 抽屉照样多出第 3 条',
              sorted(ax3['rows']) == sorted(ax3['servos']) and len(ax3['servos']) == 3, ax3)
        js(br, sid, """(() => { const r = document.querySelector(
          '#pose-axes-list .pose-axis[data-servo="3"] .pose-axis-range');
          r.value = '30'; r.dispatchEvent(new Event('input', {bubbles: true})); })()""")
        br.pump(0.5)
        a8 = js(br, sid, """(() => ({ a3: window.POSE_PARTS.angleOf(3),
          frame: document.getElementById('pose-frame-preview').textContent }))()""")
        check('★ A8 禁 WebGL 下照样能拖这条滑块', a8['a3'] == 30, a8)
        check('★ A8 禁 WebGL 下帧预览跟着更新（不撒谎：发出去的就是这个）',
              'servo,3,30#' in a8['frame'], a8['frame'])

        # ★ 抽屉布局是**纯 DOM**（普通脚本、自己挂载），没有三维也该照常能收能拖。
        #   这条是"把布局单独拆一个文件、且不许依赖三维"这个决定的实测收益。
        check('★ 禁 WebGL 下抽屉布局照样挂上了', js(br, sid, '!!window.POSE_LAYOUT') is True)
        lay0 = js(br, sid, 'window.POSE_LAYOUT.get()')
        js(br, sid, "window.POSE_LAYOUT.toggle('left')")
        br.pump(0.3)
        w_closed = js(br, sid, "document.querySelector('#drawer-parts').getBoundingClientRect().width")
        check('★ 禁 WebGL 下左抽屉照样能收起成窄边条', w_closed == 30, w_closed)
        js(br, sid, "window.POSE_LAYOUT.toggle('left')")
        br.pump(0.3)
        check('★ 禁 WebGL 下再展开也正常',
              js(br, sid, "document.querySelector('#drawer-parts').getBoundingClientRect().width")
              == lay0['leftW'])
        # 真实指针拖分隔条（不是调 API）—— 三维挂了也不许影响它
        sp = js(br, sid, """(() => { const e = document.querySelector('.pose-split[data-split="left"]');
                  if (!e) return null;
                  const b = e.getBoundingClientRect();
                  const x = b.left + b.width / 2, y = b.top + b.height / 2;
                  const hit = document.elementFromPoint(x, y);
                  return {x: x, y: y, w: b.width, h: b.height,
                          hit: hit ? (hit.className || hit.id || hit.tagName) : null,
                          scrollY: window.scrollY}; })()""")
        check('★ 禁 WebGL 下分隔条存在且点得到（不是被别的东西盖住）',
              bool(sp) and isinstance(sp['hit'], str) and 'pose-split' in str(sp['hit']), sp)

        def dme(t, x, y, buttons):
            br.call('Input.dispatchMouseEvent',
                    {'type': t, 'x': x, 'y': y,
                     'button': 'left' if t != 'mouseMoved' else 'none',
                     'buttons': buttons, 'clickCount': 1 if t != 'mouseMoved' else 0},
                    session=sid)

        dme('mousePressed', sp['x'], sp['y'], 0)
        br.pump(0.2)
        check('★ 禁 WebGL 下按下分隔条确实起了拖拽状态',
              js(br, sid, 'window.POSE_LAYOUT.dragging()')['left'] is True,
              js(br, sid, 'window.POSE_LAYOUT.dragging()'))
        # ★★ 分多步拖、并**每一步都断言拖拽还活着**。这一条是 2026-09-16 那个真 bug 的拦网：
        #    拖拽一旦把"鼠标离开元素"当成收尾信号，就会在第一/第二步被静默中断，
        #    抽屉只动几个像素。而"只看最终宽度"的断言不可靠 —— 实测这个中断
        #    在正常档浏览器里不出现、只在 --disable-3d-apis 这一档出现（大概率和
        #    没有 WebGL 时的事件时序有关）。所以要断言的是"中途一直活着"这个性质。
        alive, broke_at = True, 0
        for k in range(1, 13):
            dme('mouseMoved', sp['x'] + 5 * k, sp['y'], 1)
            br.pump(0.05)
            if js(br, sid, 'window.POSE_LAYOUT.dragging()')['left'] is not True:
                alive, broke_at = False, k
                break
        dme('mouseReleased', sp['x'] + 60, sp['y'], 0)
        br.pump(0.4)
        check('★ 禁 WebGL 下多步拖分隔条：中途没有被提前收尾',
              alive, '第 %d 步就被收尾了' % broke_at if not alive else '')
        check('★ 禁 WebGL 下分隔条照样能拖宽（真实指针事件）',
              js(br, sid, 'window.POSE_LAYOUT.get()')['leftW'] > lay0['leftW'],
              '%s → %s' % (lay0['leftW'], js(br, sid, 'window.POSE_LAYOUT.get()')['leftW']))
        errs = [e for e in br.console_errors() if 'favicon' not in e]
        check('禁 WebGL 路径没有 JS 异常', not errs, errs[:4])
    finally:
        close(edge)
        shutil.rmtree(PROFILE2, ignore_errors=True)


# ---------------- 第三期：动态角度抽屉 + 行程/倾角 + 清单缩进成树 ----------------

def phase_v4(br, sid):
    """第四期验收：分区（分块看图，R1–R8）+ 箭头永不被挡（G1–G6）。

    用户的两条原话：「新件太大了会挡住箭头」「我想一次只看底部这一块，改完再拼起来」。
    自己 localStorage.clear() + reload 起步，不依赖前面留下的状态。"""

    def dme(t, x, y, buttons=0):
        br.call('Input.dispatchMouseEvent',
                {'type': t, 'x': x, 'y': y,
                 'button': 'left' if t != 'mouseMoved' else 'none',
                 'buttons': buttons, 'clickCount': 1 if t != 'mouseMoved' else 0},
                session=sid)

    def click_at(x, y):
        dme('mousePressed', x, y, 0)
        dme('mouseReleased', x, y, 0)
        br.pump(0.35)

    def set_group(pid, g):
        """走真 UI：改行里那个「组」输入框，再派发 change（change 才是提交时机）。
        **不调 POSE_PARTS.update** —— 那等于绕过接线直接改数据，接线坏了测不出来。"""
        return js(br, sid, """(() => {
          const i = document.querySelector('.part-row[data-part="%s"] .part-group');
          if (!i) return null;
          i.value = %s;
          i.dispatchEvent(new Event('change', {bubbles: true}));
          return window.POSE_PARTS.byId('%s').group;
        })()""" % (pid, json.dumps(g), pid))

    def rows():
        return js(br, sid, """(() => {
          const q = s => Array.from(document.querySelectorAll(s));
          const rs = q('#parts-list .part-row');
          return {
            n: rs.length,
            ids: rs.map(r => r.getAttribute('data-part')),
            shown: rs.filter(r => r.offsetParent !== null).map(r => r.getAttribute('data-part')),
            ghostCls: rs.filter(r => r.classList.contains('is-ghost')).map(r => r.getAttribute('data-part')),
            count: (document.getElementById('parts-count') || {}).textContent,
            chips: q('#parts-groups .group-chip').map(b => b.textContent),
            chipOn: q('#parts-groups .group-chip.is-on').map(b => b.textContent),
            hint: !!document.querySelector('#parts-groups .parts-groups-hint'),
            dl: q('#pose-groups option').map(o => o.value),
            groups: window.POSE_PARTS.groups(),
            solo: window.POSE_PARTS.solo(),
            frame: (document.getElementById('pose-frame-preview') || {}).textContent
          };
        })()""")

    def ghosts():
        return dict((e['id'], e) for e in js(br, sid, 'window.POSE_DEBUG.snapshot()'))

    def ls():
        return js(br, sid, "localStorage.getItem('pose_parts_v1')")

    def chip_rect(name):
        return js(br, sid, """(() => {
          const b = Array.from(document.querySelectorAll('#parts-groups .group-chip'))
            .find(x => x.textContent === %s);
          if (!b) return null;
          const r = b.getBoundingClientRect();
          return {x: r.left + r.width / 2, y: r.top + r.height / 2, w: r.width, h: r.height,
                  on: b.classList.contains('is-on')};
        })()""" % json.dumps(name))

    # ================= 20 · 组字段：填了就多一个分区 =================
    print('\n=== 20. 组字段：填了就多一个分区 ===')
    st = _reload_clean(br, sid)
    check('重载后 3D 就绪（干净默认装配）', st is not None, st)
    r0 = rows()
    check('默认装配没有任何分组：chip 行只给一句怎么用',
          r0['groups'] == [] and r0['chips'] == [] and r0['hint'] is True, r0['chips'])
    check('默认 9 个元件、9 行、全部可见', r0['n'] == 9 and len(r0['shown']) == 9, r0['n'])

    check('在行里填「组」→ 当场写进数据层',
          set_group('p1', '底部') == '底部')
    r1 = rows()
    check('填了一个就多一个分区 chip（全部 / 未分组 / 底部）',
          r1['chips'] == ['全部', '未分组', '底部'], r1['chips'])
    check('还没点，所以「全部」是亮的', r1['chipOn'] == ['全部'], r1['chipOn'])
    check('组名候选（datalist）也跟着有了', r1['dl'] == ['底部'], r1['dl'])
    for pid, g in (('p2', '底部'), ('p3', '底部'), ('p6', '头部'),
                   ('p7', '头部'), ('p8', '头部'), ('p9', '头部')):
        set_group(pid, g)
    r2 = rows()
    check('两个组都出来了，顺序按树序（底部 在前）',
          r2['groups'] == ['底部', '头部'], r2['groups'])
    check('datalist 里也是两个（都是用户自己打的名字）', r2['dl'] == ['底部', '头部'], r2['dl'])
    grp_in = js(br, sid, """(() => {
      const o = {};
      document.querySelectorAll('#parts-list .part-row').forEach(r => {
        const i = r.querySelector('.part-group');
        o[r.getAttribute('data-part')] = i ? i.value : null;
      });
      return o;
    })()""")
    check('每一行的「组」输入框都回填成了它自己的组',
          grp_in['p1'] == '底部' and grp_in['p6'] == '头部' and grp_in['p4'] == '',
          grp_in)
    check('★ 组是**真数据**：导出 JSON 里带着它',
          '"group": "底部"' in js(br, sid, 'window.POSE_PARTS.exportJSON()'))

    # ================= 21 · 只看一块（R1：隐藏只是看不见）=================
    print('\n=== 21. 只看一块：其余收起来 + 半透明幽灵（R1）===')
    # ⚠️ 落盘是延迟 300ms 的（notify 里那个 setTimeout）：刚改完组就读 localStorage，
    #    读到的还是**上一版**（没有 group 字段那份）。等它落完再取基线。
    br.pump(0.7)
    exp0 = js(br, sid, 'window.POSE_PARTS.exportJSON()')
    frame0 = rows()['frame']
    servos0 = js(br, sid, 'window.POSE_PARTS.servoIds()')
    ls0 = ls()
    cnt0 = rows()['count']
    check('基线：存档里已经带着 group 字段', ls0 and '"group"' in ls0)
    c = chip_rect('底部')
    check('「底部」chip 真的在屏幕上（不然下面这条是空断言）',
          c and c['w'] > 0 and c['h'] > 0 and 0 < c['y'] < 1000, c)
    click_at(c['x'], c['y'])
    r3 = rows()
    check('点 chip → 只看这一块', r3['solo'] == '底部', r3['solo'])
    check('亮的 chip 换成「底部」', r3['chipOn'] == ['底部'], r3['chipOn'])
    check('★ 只有「底部」那三行还露着，其余收起来',
          sorted(r3['shown']) == ['p1', 'p2', 'p3'], r3['shown'])
    check('★ 但**行数一个都没少**（只是 display:none，不是删了）',
          r3['n'] == 9 and sorted(r3['ids']) == ['p1', 'p2', 'p3', 'p4', 'p5', 'p6', 'p7', 'p8', 'p9'],
          (r3['n'], r3['ids']))
    check('收起来的行打了 is-ghost 标记（口径和 3D 一致）',
          sorted(r3['ghostCls']) == ['p4', 'p5', 'p6', 'p7', 'p8', 'p9'], r3['ghostCls'])
    check('计数还是按**全部**元件算（不是只有 3 件）', r3['count'] == cnt0, (r3['count'], cnt0))

    g3 = ghosts()
    check('★ 3D 里一个元件都没少（9 个）', len(g3) == 9, len(g3))
    check('★ 本组的件照常实心',
          g3['p1']['ghost'] is False and g3['p1']['opacity'] == 1, g3['p1']['opacity'])
    check('★ 别的组在 3D 里是半透明幽灵（0.15）',
          g3['p6']['ghost'] is True and abs(g3['p6']['opacity'] - 0.15) < 1e-9, g3['p6']['opacity'])
    check('★ 幽灵的 depthWrite 必须关掉（否则会把它后面的实心件挡出一块洞）',
          g3['p6']['depthWrite'] is False and g3['p1']['depthWrite'] is True,
          (g3['p1']['depthWrite'], g3['p6']['depthWrite']))

    check('★ R1 导出 JSON 逐字节不变', js(br, sid, 'window.POSE_PARTS.exportJSON()') == exp0)
    check('★ R1 下发帧预览一字不变', rows()['frame'] == frame0, (frame0, rows()['frame']))
    check('★ R1 舵机号集合不变', js(br, sid, 'window.POSE_PARTS.servoIds()') == servos0)
    check('★ R1 localStorage 落盘内容不变（分区是瞬态的，不进存档）', ls() == ls0)

    # ================= 22 · 换主题（R2）=================
    print('\n=== 22. 换主题后幽灵还认得出来（R2）===')
    js(br, sid, """(() => {
      const b = document.getElementById('theme-toggle') || document.querySelector('[data-theme-toggle]');
      if (b) { b.click(); return 'clicked'; }
      document.documentElement.setAttribute('data-theme', 'dark');
      window.dispatchEvent(new CustomEvent('themechange', {detail: {theme: 'dark'}}));
      return 'event';
    })()""")
    br.pump(0.8)
    st2 = js(br, sid, 'window.POSE_DEBUG.state()')
    g4 = ghosts()
    check('换主题后 3D 没死、9 个件都在',
          st2['ready'] and not st2['dead'] and st2['entries'] == 9, st2)
    check('★ 换主题（材质整份重建）之后，幽灵仍然是幽灵',
          g4['p6']['ghost'] is True and abs(g4['p6']['opacity'] - 0.15) < 1e-9,
          g4['p6']['opacity'])
    check('★ 换主题之后，本组的件也仍然是实心',
          g4['p1']['ghost'] is False and g4['p1']['opacity'] == 1, g4['p1']['opacity'])

    # ================= 23 · 点幽灵不选中（R4 / R7）=================
    print('\n=== 23. 幽灵点不着（R4/R7）===')
    # 把立柱挪到旁边一片空地上：这样"屏幕坐标 → 射线"的第一件命中一定是它自己，
    # 不依赖"哪个件挡在谁前面"的几何巧合 —— 那种断言会假通过。
    js(br, sid, 'window.POSE_PARTS.soloTo(null)')
    br.pump(0.3)
    cvs = js(br, sid, """(() => {
      const c = document.querySelector('#pose-stage canvas').getBoundingClientRect();
      return {l: c.left, r: c.right, t: c.top, b: c.bottom};
    })()""")
    # ⚠️ 挪多远是**屏幕**说了算（画布宽度随抽屉/窗口变，1280 宽时只有 328px）：
    #    挑第一个稳稳落在画布里的横向偏移，别写死一个数 —— 写死的话换个窗口尺寸
    #    点就落到抽屉底下了，而那会**静默**变成"没选中"（看着跟断言本身要测的东西一样）。
    sp = None
    for wx in (1.2, 0.9, 0.6, 1.5, 0.3):
        js(br, sid, "window.POSE_PARTS.update('p2', {pos: [%s, 0.41, 0]})" % wx)
        br.pump(0.35)
        sp = js(br, sid, "POSE_EDIT_DEBUG.screenPos('p2')")
        if sp and (cvs['l'] + 20 < sp['x'] < cvs['r'] - 20) and (cvs['t'] + 20 < sp['y'] < cvs['b'] - 20):
            break
    inside = sp and (cvs['l'] + 20 < sp['x'] < cvs['r'] - 20) and (cvs['t'] + 20 < sp['y'] < cvs['b'] - 20)
    check('立柱挪到了旁边空地上、而且这个点稳稳落在画布里（不然是空断言）', inside, (sp, cvs))
    js(br, sid, 'window.POSE_PARTS.select(null)')
    br.pump(0.2)
    click_at(sp['x'], sp['y'])
    check('先证伪：不过滤时，点这个点选中的就是它',
          js(br, sid, 'POSE_EDIT_DEBUG.selected()') == 'p2',
          js(br, sid, 'POSE_EDIT_DEBUG.selected()'))
    js(br, sid, 'window.POSE_PARTS.select(null)')
    js(br, sid, "window.POSE_PARTS.soloTo('头部')")
    br.pump(0.4)
    g5 = ghosts()
    check('立柱现在成了幽灵', g5['p2']['ghost'] is True)
    click_at(sp['x'], sp['y'])
    check('★ R7 点幽灵 = 点空白：不会选中那个半透明的件',
          js(br, sid, 'POSE_EDIT_DEBUG.selected()') is None,
          js(br, sid, 'POSE_EDIT_DEBUG.selected()'))
    check('★ 没有顺手开始拖拽（幽灵不该成为拖动把手）',
          js(br, sid, 'POSE_EDIT_DEBUG.dragging()') is False
          and js(br, sid, 'POSE_EDIT_DEBUG.controlsEnabled()') is True)
    check('★ 过滤只管眼睛：数据层照样能选中它',
          js(br, sid, "window.POSE_PARTS.select('p2')") == 'p2')
    js(br, sid, 'window.POSE_PARTS.select(null)')
    js(br, sid, "window.POSE_PARTS.update('p2', {pos: [0, 0.41, 0]});"
                "window.POSE_PARTS.soloTo(null)")
    br.pump(0.3)

    # ================= 24 · 焦点 / 空组 / 导入重置（R6 / R3）=================
    print('\n=== 24. 正在输入的行不许消失；组没了要自动回全部（R6/R3）===')
    js(br, sid, "window.POSE_PARTS.soloTo('底部')")
    br.pump(0.3)
    js(br, sid, """(() => {
      const i = document.querySelector('.part-row[data-part="p1"] .part-group');
      if (i) { i.focus(); }
      return document.activeElement === i;
    })()""")
    focused = "document.activeElement === document.querySelector('.part-row[data-part=\"p1\"] .part-group')"
    check('先把「底部」那行的组输入框点进去', js(br, sid, focused) is True)
    js(br, sid, "window.POSE_PARTS.soloTo('头部')")        # 这一下会让 p1 变成幽灵
    br.pump(0.4)
    r6 = rows()
    check('★ R6 正在输入的那一行**仍然可见**（输入框没被抽走、也没失焦）',
          'p1' in r6['shown'] and 'p1' in r6['ghostCls'], r6['shown'])
    check('★ R6 焦点还在那个框里（后半句还打得进去）', js(br, sid, focused) is True)

    js(br, sid, "window.POSE_PARTS.soloTo('底部')")
    br.pump(0.2)
    js(br, sid, "['p1','p2','p3'].forEach(id => window.POSE_PARTS.remove(id));"
                "window.POSE_PARTS.render();")
    br.pump(0.4)
    r7 = rows()
    check('★ R3 把这一组删空之后，过滤自动失效（不是留一片空白让人干瞪眼）',
          r7['solo'] is None and len(r7['shown']) == r7['n'] == 6, (r7['solo'], r7['shown']))
    check('★ R3 过滤掉的那一组从 chip 行里消失了（剩下的 头部 还在）',
          '底部' not in r7['chips'] and '头部' in r7['chips'], r7['chips'])
    check('★ R3 计数按真实件数（6）', r7['count'].startswith('6 /'), r7['count'])
    js(br, sid, 'window.POSE_PARTS.reset()')
    br.pump(0.4)
    r8 = rows()
    check('R3 恢复默认装配：9 行全露、没有残留的过滤',
          r8['n'] == 9 and len(r8['shown']) == 9 and r8['solo'] is None
          and r8['groups'] == [] and r8['hint'] is True,
          (r8['n'], r8['solo'], r8['groups']))

    # ================= 25 · 白名单（R8）=================
    print('\n=== 25. 组名的白名单与长度（R8）===')
    long_name = '一二三四五六七八九十甲乙丙丁戊己'
    imp = js(br, sid, """(() => {
      const err = window.POSE_PARTS.importJSON(JSON.stringify({v: 2, parts: [
        {id: 'p1', kind: 'box', name: 'a', group: '全部'},
        {id: 'p2', kind: 'box', name: 'b', group: %s},
        {id: 'p3', kind: 'box', name: 'c', group: '头部'}
      ], angles: {}}));
      return {err: err,
              g: window.POSE_PARTS.get().map(p => p.group),
              groups: window.POSE_PARTS.groups()};
    })()""" % json.dumps(long_name))
    check('导入成功', imp['err'] is None, imp['err'])
    check('★ R8 保留名「全部」被归一成未分组（跟 chip 行抢名字会分不清）',
          imp['g'][0] == '', imp['g'])
    check('★ R8 超长组名被截到 12 字', len(imp['g'][1]) == 12, imp['g'][1])
    check('★ R8 过滤态跟着导入清空（不然会停在上一份装配的组名上）',
          js(br, sid, 'window.POSE_PARTS.solo()') is None)
    # ⚠️ 落盘是**延迟 300ms** 的（notify 里那个 setTimeout）。不等它跑完就 reload，
    #    这一份装配会跟着页面一起死掉，然后你会以为是"存档没存住"。
    br.pump(0.7)
    st3 = _reload_clean(br, sid, clear=False)
    check('整页重载后 3D 仍就绪', st3 is not None, st3)
    pers = js(br, sid, "window.POSE_PARTS.get().map(p => p.group)")
    check('★ 组名整页重载后还在（真进了存档）',
          pers[0] == '' and len(pers[1]) == 12 and pers[2] == '头部', pers)

    # ================= 26 · 箭头永不被挡（G 系列）=================
    print('\n=== 26. 箭头永不被挡（G1–G6）===')
    _reload_clean(br, sid)

    # ⚠️ scale 是**每帧**在 gizmo.update() 里算的（loop 里跑），所以 select 之后必须
    #    pump 一帧再读 —— 当场读拿到的是上一帧的值。
    def style_after_select(pid):
        js(br, sid, 'window.POSE_PARTS.select(%s)' % json.dumps(pid))
        br.pump(0.4)
        return js(br, sid, 'POSE_EDIT_DEBUG.gizmoStyle()')

    stl = style_after_select('p1')
    check('★ G1 箭头关掉深度测试（永远不会被元件挡在后面）',
          stl and stl['depthTest'] is False, stl)
    check('★ G1 深度写入也必须关（不然箭头自己互相裁、画面发花）',
          stl and stl['depthWrite'] is False, stl)
    check('★ G1 尖端那个锥子同样处理', stl and stl['headDepthTest'] is False, stl)
    check('★ G1 挪进透明队列 + renderOrder 999（幽灵画完才轮到它 → 压在最上面）',
          stl['transparent'] is True and stl['opacity'] == 1 and stl['renderOrder'] == 999, stl)

    def clampf(s, lo, hi):
        return max(lo, min(hi, s))

    stl2 = style_after_select('p8')          # 屏幕 size [0.5,0.3] → 半径 0.29，够小
    expect = clampf(stl2['dist'] * 0.115, 0.30, 2.2)
    check('★ G2 小元件没有被误伤：尺寸还是老规矩（跟着相机距离走）',
          abs(stl2['scale'] - expect) < 1e-9 and stl2['radius'] * 1.7 < stl2['scale'],
          (stl2['scale'], expect, stl2['radius']))

    newid = js(br, sid, """(() => {
      const id = window.POSE_PARTS.add('box').id;
      window.POSE_PARTS.update(id, {size: [1.4, 1.4, 1.4]});
      return id;
    })()""")
    br.pump(0.5)
    big = style_after_select(newid)
    big['gizmo'] = js(br, sid, 'POSE_EDIT_DEBUG.gizmo()')
    check('★ G2 中等大元件：箭头按包围半径兜底放大（箭头尖探出包围球）',
          abs(big['scale'] - big['radius'] * 1.7) < 1e-9 and big['scale'] * 0.62 >= big['radius'],
          (big['scale'], big['radius']))
    check('★ G3 放大没有冲破上限（上限一破，箭头就把画布变成一整块拖动区）',
          big['scale'] <= 2.2 + 1e-9, big['scale'])
    check('大元件选中时箭头照样画着', big['gizmo']['visible'] is True, big['gizmo'])

    js(br, sid, 'window.POSE_PARTS.update(%s, {size: [4, 4, 4]})' % json.dumps(newid))
    br.pump(0.5)
    huge = js(br, sid, "(() => { const a = POSE_EDIT_DEBUG.gizmoStyle();"
                       " a.gizmo = POSE_EDIT_DEBUG.gizmo(); return a; })()")
    check('★ G3 超大元件：宁可封顶也不让箭头吞掉画布',
          abs(huge['scale'] - 2.2) < 1e-9, huge['scale'])
    check('★ G1 超大元件选中时箭头仍然画着（可见性靠"永远在最上面"保住）',
          huge['gizmo']['visible'] is True, huge['gizmo'])
    cvs2 = js(br, sid, """(() => {
      const c = document.querySelector('#pose-stage canvas').getBoundingClientRect();
      return {l: c.left, t: c.top, r: c.right, b: c.bottom};
    })()""")
    dme('mousePressed', cvs2['l'] + 4, cvs2['t'] + 4, 0)
    br.pump(0.15)
    check('★ G6 封顶之后画布角落里照样不是拖动区（按下不误判成抓箭头）',
          js(br, sid, 'POSE_EDIT_DEBUG.dragging()') is False)
    dme('mouseReleased', cvs2['l'] + 4, cvs2['t'] + 4, 0)
    br.pump(0.3)
    check('G6 松手后控制器仍然可用',
          js(br, sid, 'POSE_EDIT_DEBUG.controlsEnabled()') is True)

    print('\n=== 27. 第四期全程没有 JS 异常 ===')
    errs = [e for e in br.console_errors() if 'favicon' not in e and 'net::ERR' not in e]
    check('没有 JS 异常 / error 级日志', not errs, errs[:6])


def _reload_clean(br, sid, clear=True):
    """整页重载。clear=True 先清 localStorage → 拿回一份**干净的默认装配**。
    前面十几节把 doc 搅得很乱，新断言必须从一个确定状态起步。"""
    js(br, sid, ('localStorage.clear(); location.reload()') if clear else 'location.reload()')
    br.pump(0.6)
    for _ in range(60):
        if any(m.get('method') == 'Page.loadEventFired' for m in br.events[-12:]):
            break
        br.pump(0.25)
    return ready_pose(br, sid)


def _probe(br, sid):
    """抽屉 + 清单的现状快照。**必须全是纯数据** —— POSE_EDIT_DEBUG.axes() 里的
    node 是 DOM 节点，returnByValue 传不过来（CDP 会把它变成空对象）。"""
    return js(br, sid, """(() => {
      const a = window.POSE_EDIT_DEBUG.axes();
      const q = s => Array.from(document.querySelectorAll(s));
      const rows = q('#parts-list .part-row');
      return {
        ok: a.ok, key: a.key,
        rows: a.rows.map(r => ({servo: r.servo, name: r.name, value: r.value,
          min: r.min, max: r.max, out: r.out, invert: r.invert, axis: r.axis,
          mount: r.mount, quickDisabled: r.quickDisabled})),
        nRange: q('#pose-axes-list .pose-axis-range').length,
        nInv:   q('#pose-axes-list .pose-axis-inv input').length,
        nAxis:  q('#pose-axes-list .pose-axis-axis').length,
        nMount: q('#pose-axes-list .pose-axis-mount').length,
        nRows:  rows.length,
        nShown: rows.filter(r => r.offsetParent !== null).length,
        nSel:   rows.filter(r => r.classList.contains('is-selected')).length,
        domIds: rows.map(r => r.getAttribute('data-part')),
        dataIds: window.POSE_PARTS.get().map(p => p.id),
        depth: rows.map(r => r.getAttribute('data-depth')),
        selId: window.POSE_EDIT_DEBUG.selected()
      };
    })()""")


def _add_via_ui(br, sid, kind):
    """走真实的「＋ 添加」按钮（不调 PP.add），免得测试绕过用户实际走的路径。"""
    js(br, sid, """(() => { const s = document.getElementById('parts-add-kind');
      s.value = '%s'; document.getElementById('parts-add').click(); })()""" % kind)
    br.pump(0.5)


def _fold(br, sid, pid):
    """点那一行的折叠三角。"""
    js(br, sid, """(() => { const t = document.querySelector(
      '.part-row[data-part="%s"] .part-fold'); t.click(); })()""" % pid)
    br.pump(0.4)


def phase_v3(br, sid):
    """第三期验收：动态角度抽屉（A1–A7）+ 行程/倾角（A9–A12）+ 缩进成树（B9–B14）。
    （A8 在无 WebGL 那一档里，见 phase_nowebgl。）"""

    def dme(t, x, y, buttons=0):
        br.call('Input.dispatchMouseEvent',
                {'type': t, 'x': x, 'y': y,
                 'button': 'left' if t != 'mouseMoved' else 'none',
                 'buttons': buttons, 'clickCount': 1 if t != 'mouseMoved' else 0},
                session=sid)

    # ================= A · 右抽屉按装配动态生成 =================
    print('\n=== 15. 右抽屉按装配动态生成（A1–A7）===')
    st = _reload_clean(br, sid)
    check('重载后 3D 就绪（干净默认装配）', st is not None, st)
    p0 = _probe(br, sid)
    ids0 = js(br, sid, 'window.POSE_PARTS.servoIds()')
    check('A1 ★ 首屏（未做任何交互）每一路舵机都有一条滑块',
          p0['ok'] and sorted(r['servo'] for r in p0['rows']) == sorted(ids0),
          {'抽屉': [r['servo'] for r in p0['rows']], 'servoIds': ids0})
    check('A2 每一路的 range / 反向勾 / 转轴各恰好 1 个、倾角 3 个（防重复生成 / 控件跑到容器外）',
          p0['nRange'] == len(ids0) and p0['nInv'] == len(ids0)
          and p0['nAxis'] == len(ids0) and p0['nMount'] == len(ids0) * 3,
          {'range': p0['nRange'], 'inv': p0['nInv'], 'axis': p0['nAxis'],
           'mount': p0['nMount'], 'servos': len(ids0)})
    check('A2 滑块两端默认就是 0~360 行程内的 0/180',
          all(r['min'] == 0 and r['max'] == 180 for r in p0['rows']),
          [(r['servo'], r['min'], r['max']) for r in p0['rows']])

    # ★ A3：多步拖滑块，每一步都断言"还是同一个 DOM 节点" + 值严格递增。
    #    这是**唯一**能证伪"改角度偷偷重建了"的断言 —— 重建出来的新节点长得一模一样，
    #    只有节点身份能区分。而 range 的拖拽状态绑在节点上：节点一被 remove，后续
    #    pointermove 全投递给已脱离文档的旧节点、input 冒泡不到委托容器
    #    ⇒ 表现是"滑块只跳一格就永久卡死"，且零报错。
    js(br, sid, """(() => {
      window.__t = { node: document.querySelector(
        '#pose-axes-list .pose-axis[data-servo="2"] .pose-axis-range'),
        same: [], vals: [], ang: [] };
    })()""")
    for k in range(1, 6):
        js(br, sid, """(() => {
          const cur = document.querySelector(
            '#pose-axes-list .pose-axis[data-servo="2"] .pose-axis-range');
          cur.value = String(%d);
          cur.dispatchEvent(new Event('input', {bubbles: true}));
          window.__t.same.push(cur === window.__t.node && window.__t.node.isConnected);
          window.__t.vals.push(parseInt(cur.value, 10));
          window.__t.ang.push(window.POSE_PARTS.angleOf(2));
        })()""" % (25 * k))
        br.pump(0.12)
    tr = js(br, sid, 'window.__t')
    check('A3 ★ 多步拖滑块：自始至终是同一个 DOM 节点（改角度没重建）',
          all(tr['same']), tr['same'])
    check('A3 ★ 多步拖滑块：值严格递增，且每一步 angleOf(2) 都跟上',
          all(tr['vals'][i] < tr['vals'][i + 1] for i in range(len(tr['vals']) - 1))
          and tr['ang'] == tr['vals'],
          {'值': tr['vals'], 'angleOf': tr['ang']})
    rowAng = js(br, sid, """(() => {
      const r = document.querySelector('#parts-list .part-row[data-part="p3"]');
      const i = r ? r.querySelector('.part-servo-angle') : null;
      return i ? parseInt(i.value, 10) : null; })()""")
    check('A4 ★ 拖抽屉滑块后，清单那一行的角度框跟着走（守 servoNo 那个字符串坑）',
          rowAng == tr['vals'][-1], {'清单行': rowAng, '抽屉': tr['vals'][-1]})

    # ★ A5：真实 click() 触发（不是直接改数据），并且**绝不能只查 .checked** ——
    #    dataset.servo 是字符串时 servoPart() 严格相等会返回 null、handler 直接
    #   return，勾还显示成勾上（浏览器原生行为）但 doc 里没变 ⇒ 用户拿实物校准过的值
    #   刷新就丢。所以要断言数据层 + localStorage + reload 三处都是 true。
    js(br, sid, """(() => { document.querySelector(
      '#pose-axes-list .pose-axis[data-servo="2"] .pose-axis-inv input').click(); })()""")
    br.pump(0.5)
    inv = js(br, sid, """(() => {
      const p = window.POSE_PARTS.get().find(x => x.kind === 'servo' && x.servoId === 2);
      let disk = null;
      try { const d = JSON.parse(localStorage.getItem('pose_parts_v1') || 'null');
            disk = d ? (d.parts.find(x => x.kind === 'servo' && x.servoId === 2) || {}).invert : null;
      } catch (e) { disk = 'parse-error'; }
      return {mem: p ? p.invert : null, disk: disk,
              checked: document.querySelector(
                '#pose-axes-list .pose-axis[data-servo="2"] .pose-axis-inv input').checked};
    })()""")
    check('A5 ★ 真实 click() 反向勾 → 数据层 invert=true（不能只看 .checked）',
          inv['mem'] is True and inv['checked'] is True, inv)
    check('A5 ★ 反向勾已落盘', inv['disk'] is True, inv)
    _reload_clean(br, sid, clear=False)
    inv2 = js(br, sid, """(() => {
      const p = window.POSE_PARTS.get().find(x => x.kind === 'servo' && x.servoId === 2);
      return {mem: p ? p.invert : null,
              checked: document.querySelector(
                '#pose-axes-list .pose-axis[data-servo="2"] .pose-axis-inv input').checked};
    })()""")
    check('A5 ★ 刷新后反向还在，抽屉里的勾也跟着显示为勾上（一个真相两个入口）',
          inv2['mem'] is True and inv2['checked'] is True, inv2)

    # 加第三路舵机，测"动态生成"与"快捷按钮不许串路"
    _add_via_ui(br, sid, 'servo')
    p3 = _probe(br, sid)
    ids3 = js(br, sid, 'window.POSE_PARTS.servoIds()')
    check('A1b ★ 加了一路舵机之后，抽屉里立刻多出对应的那一条',
          sorted(r['servo'] for r in p3['rows']) == sorted(ids3) and len(ids3) == 3,
          {'抽屉': [r['servo'] for r in p3['rows']], 'servoIds': ids3})
    js(br, sid, "window.POSE_PARTS.setAngle(1,90); window.POSE_PARTS.setAngle(2,90);"
                "window.POSE_PARTS.setAngle(3,90);")
    br.pump(0.4)
    js(br, sid, """(() => { document.querySelector(
      '#pose-axes-list .pose-axis[data-servo="3"] .qbtn[data-set="0"]').click(); })()""")
    br.pump(0.4)
    xt = js(br, sid, """(() => ({
      a1: window.POSE_PARTS.angleOf(1), a2: window.POSE_PARTS.angleOf(2),
      a3: window.POSE_PARTS.angleOf(3),
      frame: document.getElementById('pose-frame-preview').textContent }))()""")
    check('A7 ★ 点舵机3 的 0° → 只有它变，1/2 不许被串到',
          xt['a3'] == 0 and xt['a1'] == 90 and xt['a2'] == 90, xt)
    check('A7 ★ 帧预览里 servo,3 也是 0', 'servo,3,0#' in xt['frame'], xt['frame'])

    newId = js(br, sid, """(() => {
      const p = window.POSE_PARTS.get().find(x => x.kind === 'servo' && x.servoId === 3);
      return p ? p.id : null; })()""")
    js(br, sid, "window.POSE_PARTS.select('%s')" % newId)
    br.pump(0.3)
    js(br, sid, """(() => { document.querySelector(
      '.part-row[data-part="%s"] .part-del').click(); })()""" % newId)
    br.pump(0.6)
    p6 = _probe(br, sid)
    ids2 = js(br, sid, 'window.POSE_PARTS.servoIds()')
    check('A6 删掉一路舵机 → 抽屉跟着收回那一条，无报错',
          sorted(r['servo'] for r in p6['rows']) == sorted(ids2) and len(ids2) == 2,
          {'抽屉': [r['servo'] for r in p6['rows']], 'servoIds': ids2})
    check('A6 删掉选中件后选中态收干净（没有行还挂着 is-selected）',
          p6['nSel'] == 0 and p6['selId'] is None, {'nSel': p6['nSel'], 'sel': p6['selId']})

    # ================= A+ · 行程上下限 + 安装倾角 =================
    print('\n=== 16. 行程上下限 + 安装倾角（A9–A12）===')
    _reload_clean(br, sid)
    base = js(br, sid, """(() => JSON.stringify(window.POSE_DEBUG.snapshot()
      .map(e => ({id: e.id, w: e.world, l: e.localRotDeg, r: e.rotDeg}))))()""")
    w4_before = js(br, sid, "window.POSE_DEBUG.snapshot().find(e=>e.id==='p4').world")

    # A12：老模型一眼不变 —— mount 默认就是 [0,0,0]，**显式写一次**必须逐字节无变化
    js(br, sid, """(() => {
      const p = window.POSE_PARTS.get().find(x => x.kind === 'servo' && x.servoId === 2);
      window.POSE_PARTS.update(p.id, { mount: [0, 0, 0] }); })()""")
    br.pump(0.5)
    same = js(br, sid, """(() => JSON.stringify(window.POSE_DEBUG.snapshot()
      .map(e => ({id: e.id, w: e.world, l: e.localRotDeg, r: e.rotDeg}))))()""")
    check('A12 ★ mount=[0,0,0] 时坐标/旋转与改动前逐字节一致（老存档、老模型一眼不变）',
          base == same, (base[:90], same[:90]))

    # A9：横着装（绕 Z 倾 90°）—— 3D 姿态要跟着变，导出要带着，刷新要还在，拖拽仍跟手
    js(br, sid, """(() => {
      const p = window.POSE_PARTS.get().find(x => x.kind === 'servo' && x.servoId === 2);
      window.POSE_PARTS.update(p.id, { mount: [0, 0, 90] }); })()""")
    br.pump(0.6)
    mt = js(br, sid, """(() => {
      const p = window.POSE_PARTS.get().find(x => x.kind === 'servo' && x.servoId === 2);
      const d = JSON.parse(window.POSE_PARTS.exportJSON());
      const q = d.parts.find(x => x.kind === 'servo' && x.servoId === 2);
      return {mem: window.POSE_PARTS.mountOf(p.id), exp: q.mount,
              w4: window.POSE_DEBUG.snapshot().find(e => e.id === 'p4').world}; })()""")
    check('A9 倾角 [0,0,90] 写进了数据层与导出', mt['mem'] == [0, 0, 90] and mt['exp'] == [0, 0, 90], mt)
    check('A9 倾角真的把孩子的世界坐标挪了（不是只存了个字段）',
          not all(near(mt['w4'][i], w4_before[i], 1e-6) for i in range(3)),
          (w4_before, mt['w4']))
    _reload_clean(br, sid, clear=False)
    mt2 = js(br, sid, """(() => {
      const p = window.POSE_PARTS.get().find(x => x.kind === 'servo' && x.servoId === 2);
      return {mem: window.POSE_PARTS.mountOf(p.id),
              w4: window.POSE_DEBUG.snapshot().find(e => e.id === 'p4').world}; })()""")
    check('A9 ★ 刷新后倾角还在，姿态也照着重放（不是只把数字存回来了）',
          mt2['mem'] == [0, 0, 90]
          and all(near(mt2['w4'][i], mt['w4'][i], 1e-6) for i in range(3)), mt2)

    # A9 ③：倾角非零之后，三轴拖拽必须仍然跟手（父级矩阵变了，世界→局部的换算得跟上）
    js(br, sid, "window.POSE_EDIT_DEBUG.select('p4')")
    br.pump(0.6)
    gp = js(br, sid, "window.POSE_EDIT_DEBUG.axisPoint('x', 0.55)")
    check('A9 抓得到 X 箭头（抓不上后面两条就是空断言）', bool(gp), gp)
    w0 = js(br, sid, "window.POSE_DEBUG.snapshot().find(e=>e.id==='p4').world")
    dme('mousePressed', gp['x'], gp['y'], 0)
    br.pump(0.15)
    for k in range(1, 7):
        dme('mouseMoved', gp['x'] + 12 * k, gp['y'], 1)
        br.pump(0.05)
    dme('mouseReleased', gp['x'] + 72, gp['y'], 0)
    br.pump(0.45)
    w1 = js(br, sid, "window.POSE_DEBUG.snapshot().find(e=>e.id==='p4').world")
    dpos = js(br, sid, "window.POSE_PARTS.get().find(x=>x.id==='p4').pos")
    check('A9 ★ 倾角非零时拖拽仍然跟手：元件确实动了',
          not all(near(w0[i], w1[i], 1e-6) for i in range(3)), (w0, w1))
    check('A9 ★ 且世界落点仍沿着**世界 X** 走（父级被倾过，换算没算错）',
          near(w1[1], w0[1], 1e-4) and near(w1[2], w0[2], 1e-4), (w0, w1))
    check('A9 写回的局部坐标没有 NaN、没飞出 ±8',
          all(isinstance(v, (int, float)) and abs(v) <= 8 for v in dpos), dpos)

    # A10：行程 30~150
    # 先把倾角复位 —— 倾角非零时欧拉三个分量会互相混进彼此，A11 就没法直接用
    # localRotDeg[1] 读数断言了（那不是 bug，是欧拉分解的固有性质）。
    js(br, sid, """(() => {
      const p = window.POSE_PARTS.get().find(x => x.kind === 'servo' && x.servoId === 2);
      window.POSE_PARTS.update(p.id, { mount: [0, 0, 0] }); })()""")
    br.pump(0.5)
    js(br, sid, """(() => {
      const w = document.querySelector('#pose-axes-list .pose-axis[data-servo="2"]');
      const mn = w.querySelector('.pose-axis-min'); mn.value = '30';
      mn.dispatchEvent(new Event('change', {bubbles: true}));
      const mx = w.querySelector('.pose-axis-max'); mx.value = '150';
      mx.dispatchEvent(new Event('change', {bubbles: true})); })()""")
    br.pump(0.6)
    a10 = js(br, sid, """(() => {
      const w = document.querySelector('#pose-axes-list .pose-axis[data-servo="2"]');
      const r = w.querySelector('.pose-axis-range');
      const qd = {};
      w.querySelectorAll('.qbtn').forEach(b => { qd[b.getAttribute('data-set')] = !!b.disabled; });
      return {min: parseInt(r.min, 10), max: parseInt(r.max, 10), qd: qd}; })()""")
    check('A10 ★ 设了行程 30~150 → 滑块两端跟着变',
          a10['min'] == 30 and a10['max'] == 150, a10)
    check('A10 ★ 行程外的快捷按钮置灰（0° 与 180°），行程内的不置灰',
          a10['qd'].get('0') is True and a10['qd'].get('180') is True
          and a10['qd'].get('45') is False and a10['qd'].get('90') is False
          and a10['qd'].get('135') is False, a10['qd'])
    js(br, sid, """(() => { const r = document.querySelector(
      '#pose-axes-list .pose-axis[data-servo="2"] .pose-axis-range');
      r.value = r.min; r.dispatchEvent(new Event('input', {bubbles: true})); })()""")
    br.pump(0.35)
    lo = js(br, sid, "window.POSE_PARTS.angleOf(2)")
    js(br, sid, """(() => { const r = document.querySelector(
      '#pose-axes-list .pose-axis[data-servo="2"] .pose-axis-range');
      r.value = r.max; r.dispatchEvent(new Event('input', {bubbles: true})); })()""")
    br.pump(0.35)
    hi = js(br, sid, "window.POSE_PARTS.angleOf(2)")
    check('A10 ★ 滑到两头恰好停在行程端点 30 / 150（不是 0 / 180）',
          lo == 30 and hi == 150, (lo, hi))
    fr = js(br, sid, "document.getElementById('pose-frame-preview').textContent")
    check('A10 ★ 下发帧里的数也在行程内', 'servo,2,150#' in fr, fr)

    # A11：行程收窄 → 当前角度被夹回来，且抽屉/清单/3D 三处一致
    js(br, sid, """(() => { const r = document.querySelector(
      '#pose-axes-list .pose-axis[data-servo="2"] .pose-axis-range');
      r.value = '30'; r.dispatchEvent(new Event('input', {bubbles: true})); })()""")
    br.pump(0.35)
    js(br, sid, """(() => {
      const w = document.querySelector('#pose-axes-list .pose-axis[data-servo="2"]');
      const mn = w.querySelector('.pose-axis-min'); mn.value = '60';
      mn.dispatchEvent(new Event('change', {bubbles: true})); })()""")
    br.pump(0.6)
    a11 = js(br, sid, """(() => {
      const w = document.querySelector('#pose-axes-list .pose-axis[data-servo="2"]');
      const p = window.POSE_PARTS.get().find(x => x.kind === 'servo' && x.servoId === 2);
      const row = document.querySelector('#parts-list .part-row[data-part="' + p.id + '"]');
      const snap = window.POSE_DEBUG.snapshot().find(e => e.id === 'p3');
      return {ang: window.POSE_PARTS.angleOf(2),
              slider: parseInt(w.querySelector('.pose-axis-range').value, 10),
              rowAng: row ? parseInt(row.querySelector('.part-servo-angle').value, 10) : null,
              rot: snap.localRotDeg}; })()""")
    check('A11 ★ 把下限提到 60 → 当前角度 30 被夹到 60，滑块也跟到 60',
          a11['ang'] == 60 and a11['slider'] == 60, a11)
    check('A11 ★ 清单那一行也是 60（不许上面 60、下面 30）', a11['rowAng'] == 60, a11)
    check('A11 ★ 3D 也按夹后的值转（-30° = 60-90）', near(a11['rot'][1], -30, 1e-6), a11['rot'])

    # ================= B · 元件清单缩进成树 =================
    print('\n=== 17. 元件清单缩进成树（B9–B14）===')
    st17 = _reload_clean(br, sid)
    check('重载后 3D 就绪（干净默认装配）', st17 is not None, st17)

    def ids_match(tag):
        p = _probe(br, sid)
        return check('B9 ★ %s：清单 DOM 的 id 集合 === 数据层 id 集合（不丢件、不凭空多件）' % tag,
                     sorted(p['domIds']) == sorted(p['dataIds']),
                     {'DOM': p['domIds'], '数据': p['dataIds']})
    ids_match('首屏默认装配')

    # 加一个方块：它默认挂在"第一个舵机"(p3) 下 —— 旧实现里它会跑到清单最底部
    _add_via_ui(br, sid, 'box')
    order = js(br, sid, """(() => ({
      dom: Array.from(document.querySelectorAll('#parts-list .part-row'))
             .map(r => r.getAttribute('data-part')),
      dfs: window.POSE_PARTS.treeOrder().map(x => x.p.id) }))()""")
    check('B10 ★ 清单 DOM 顺序 = DFS（不是数组原序、也不是"新件一律垫底"）',
          order['dom'] == order['dfs'], order)
    i3 = order['dom'].index('p3')
    check('B10 ★ p3 后面紧跟它的子树 p4/p5/p6，再往里是 p7/p8/p9',
          order['dom'][i3:i3 + 7] == ['p3', 'p4', 'p5', 'p6', 'p7', 'p8', 'p9'], order['dom'])
    check('B10 ★ 新加的那个子件排在 p3 子树里（不再甩到最底下）',
          order['dom'].index('p10') > order['dom'].index('p9'), order['dom'])

    b11 = js(br, sid, """(() => {
      const depth = {}, left = {};
      document.querySelectorAll('#parts-list .part-row').forEach(r => {
        const id = r.getAttribute('data-part');
        depth[id] = r.getAttribute('data-depth');
        /* ★ 量 .part-head 的左边：缩进用的是 padding-left，
           量行本身的 left 是不动的（padding 不移动盒子的左边缘）。 */
        const h = r.querySelector('.part-head');
        left[id] = h ? Math.round(h.getBoundingClientRect().left) : null;
      });
      return {depth: depth, left: left}; })()""")
    check('B11 ★ data-depth：p1/p2/p3=0、p4/p5/p6=1、p7/p8/p9=2、新件 p10=1',
          all(b11['depth'].get(k) == v for k, v in
              [('p1', '0'), ('p2', '0'), ('p3', '0'), ('p4', '1'), ('p5', '1'), ('p6', '1'),
               ('p7', '2'), ('p8', '2'), ('p9', '2'), ('p10', '1')]), b11['depth'])
    check('B11 ★ 实测左边距随深度递增（缩进真的生效，不是只挂了个属性）',
          b11['left']['p1'] < b11['left']['p4'] < b11['left']['p7'],
          {k: b11['left'][k] for k in ('p1', 'p3', 'p4', 'p7')})
    ids_match('add 之后')

    # B12：折叠是**纯列表行为** —— 不许碰 doc、不许碰导出、不许碰 3D
    js(br, sid, """(() => { window.__b = { exp: window.POSE_PARTS.exportJSON(),
      snap: JSON.stringify(window.POSE_DEBUG.snapshot()) }; })()""")
    _fold(br, sid, 'p3')
    b12 = js(br, sid, """(() => {
      const rows = Array.from(document.querySelectorAll('#parts-list .part-row'));
      return {sameExp: window.POSE_PARTS.exportJSON() === window.__b.exp,
              sameSnap: JSON.stringify(window.POSE_DEBUG.snapshot()) === window.__b.snap,
              nRows: rows.length,
              nShown: rows.filter(r => r.offsetParent !== null).length,
              collapsed: window.POSE_PARTS.expanded('p3'),
              state: window.POSE_DEBUG.state()}; })()""")
    check('B12 ★ 折叠 p3：导出 JSON 逐字节不变（折叠态不进导出）', b12['sameExp'])
    check('B12 ★ 折叠 p3：3D 快照逐字节不变（折叠不碰三维）', b12['sameSnap'])
    check('B12 ★ 隐藏 ≠ 移除：.part-row 仍是 10 个，但可见的少了',
          b12['nRows'] == 10 and b12['nShown'] < 10 and b12['collapsed'] is False,
          (b12['nRows'], b12['nShown'], b12['collapsed']))
    check('B12 折叠没把 3D 弄死', b12['state']['ready'] and not b12['state']['dead'], b12['state'])

    # B13：★ 必须"先选中 p7 → 再折 p3 → 再从 3D 点同一个件"。
    #     这一下才会走 selectPart 的**早退分支**（next === selectedId），
    #     早退分支里不 revealRow 的话祖先永远展不开。只点一次会假通过。
    _fold(br, sid, 'p3')                                  # 展开回来
    js(br, sid, "window.POSE_EDIT_DEBUG.select('p7')")
    br.pump(0.4)

    # ⚠️ 落点不能取元件中心 —— **手柄的三根箭头就压在元件身上**，那一下会命中箭头、
    #    走成"开始拖拽"（把手柄的候选点击清掉），松手时既不改选也不展开，看着像功能坏了。
    #    所以按"离三根箭头都最远"挑一个半径 30px 的点。
    sp = js(br, sid, "window.POSE_EDIT_DEBUG.screenPos('p7')")
    check('B13 拿得到 p7 的屏幕坐标（拿不到就没法点）', bool(sp) and sp.get('onScreen'), sp)
    tips = js(br, sid, """(() => { const o = [];
      ['x','y','z'].forEach(a => { const p = window.POSE_EDIT_DEBUG.axisPoint(a, 0.55);
        if (p) o.push([p.x, p.y]); }); return o; })()""")

    def _seg_dist(p, a, b):
        (ax, ay), (bx, by), (px, py) = a, b, p
        dx, dy = bx - ax, by - ay
        L = dx * dx + dy * dy
        t = 0.0 if L == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L))
        return ((px - (ax + t * dx)) ** 2 + (py - (ay + t * dy)) ** 2) ** 0.5

    import math as _m
    _o = (sp['x'], sp['y'])
    _cands = [(_o[0] + 30 * _m.cos(_m.radians(d)), _o[1] + 30 * _m.sin(_m.radians(d)))
              for d in range(0, 360, 10)]
    _clr = lambda p: min(_seg_dist(p, _o, tuple(t)) for t in tips)      # noqa: E731
    click_pt = max(_cands, key=_clr)
    check('B13 挑到一个躲开三轴箭头的落点（点到箭头就变成拖拽了）',
          _clr(click_pt) > 18, {'point': click_pt, 'clearance': _clr(click_pt)})
    hitel = js(br, sid, """(() => { const e = document.elementFromPoint(%f, %f);
      return e ? e.tagName : null; })()""" % (click_pt[0], click_pt[1]))
    check('B13 落点确实在 3D 画布上（不被抽屉/工具栏盖住）', hitel == 'CANVAS', hitel)

    def click_at(pt):
        """返回按下时有没有起拖拽 —— 起了就说明打到了箭头，不是点选。"""
        dme('mousePressed', pt[0], pt[1], 0)
        br.pump(0.12)
        down = js(br, sid, "window.POSE_EDIT_DEBUG.dragging()")
        dme('mouseReleased', pt[0], pt[1], 0)
        br.pump(0.4)
        return down

    # 第一下：走**正常分支**，真正选中那一点上的件。
    # （不自作主张认成 p7 —— p8「屏幕」/p9「视线箭头」和它同位置，挡住谁是射线说了算。）
    d1 = click_at(click_pt)
    # ⚠️ 别硬编码"点中的一定是 p7" —— p8「屏幕」/p9「视线箭头」和它同位置，
    #    谁先被射线命中是几何说了算。只要它在 p3 子树里，这个用例就成立
    #    （折起 p3 会让它的行消失，第二下才谈得上"早退分支要 revealRow"）。
    hit1 = js(br, sid, """(() => {
      const id = window.POSE_EDIT_DEBUG.selected();
      const par = {}; window.POSE_PARTS.get().forEach(p => { par[p.id] = p.parent; });
      let cur = id, guard = 0, under = false;
      while (cur && guard++ < 80) { if (cur === 'p3') { under = true; break; } cur = par[cur]; }
      return {id: id, underP3: under, exp: window.POSE_PARTS.expanded('p3')}; })()""")
    check('B13 第一下点中的是 p3 子树里的某个件（不是箭头、也不是打了空）',
          d1 is False and hit1['underP3'] is True, (d1, hit1))
    hid = hit1['id']

    _fold(br, sid, 'p3')                                  # 再折起来
    vis0 = js(br, sid, """(() => { const r = document.querySelector('.part-row[data-part="%s"]');
      return {shown: r.offsetParent !== null, sel: r.classList.contains('is-selected')}; })()""" % hid)
    check('B13 折起来之后那一行确实看不见了（否则下面的断言是空转）',
          vis0['shown'] is False and vis0['sel'] is True, vis0)

    # ★ 第二下：同一个屏幕落点、同一个件 → selectPart 走**早退分支**
    #   （next === selectedId → 直接 return）。这里不 revealRow 的话祖先永远展不开。
    d2 = click_at(click_pt)
    check('B13 第二下仍然没打到箭头（否则测的就不是点选）', d2 is False)
    vis1 = js(br, sid, """(() => { const r = document.querySelector('.part-row[data-part="%s"]');
      return {shown: r.offsetParent !== null, sel: r.classList.contains('is-selected'),
              selId: window.POSE_EDIT_DEBUG.selected(),
              expanded: window.POSE_PARTS.expanded('p3')}; })()""" % hid)
    check('B13 ★ 折叠后从 3D 点**同一个件**（走早退分支）→ 祖先自动展开、那行可见且选中',
          vis1['shown'] is True and vis1['sel'] is True
          and vis1['selId'] == hid and vis1['expanded'] is True, vis1)

    # B14：折着的时候 add —— 新件默认挂在 p3 下，不 revealRow 的话用户点「＋ 添加」看着毫无变化
    # ⚠️ 别假设 B13 结束时 p3 是折着的（它可能已经自己展开了）—— 明确摆好状态再测
    if js(br, sid, "window.POSE_PARTS.expanded('p3')"):
        _fold(br, sid, 'p3')
    check('B14 准备：p3 已折叠', js(br, sid, "window.POSE_PARTS.expanded('p3')") is False)
    _add_via_ui(br, sid, 'box')
    b14 = js(br, sid, """(() => {
      const a = window.POSE_PARTS.get();
      const last = a[a.length - 1];
      const r = document.querySelector('.part-row[data-part="' + last.id + '"]');
      const rect = r ? r.getBoundingClientRect() : null;
      return {id: last.id, parent: last.parent,
              shown: !!(r && r.offsetParent !== null && rect.height > 0),
              expanded: window.POSE_PARTS.expanded('p3')}; })()""")
    check('B14 ★ 折着 p3 时新加的子件照样几何可见（祖先链被自动展开）',
          b14['shown'] is True and b14['expanded'] is True, b14)
    ids_match('再 add 之后')

    # B9 剩下的时点：删一个带子件的舵机（孤儿回落）/ 导入带环的 JSON / reset
    # ⚠️ 走 UI 的删除按钮，别调 PP.remove() —— 那几个 API **故意不自己重渲染**
    #    （它们被高频调用，一渲染就会把用户正在输入的数字框清掉），
    #    重画是 UI 的 handler 负责的。直接调 API 再断言 DOM，测的是测试自己的错。
    js(br, sid, """(() => { document.querySelector(
      '.part-row[data-part="p3"] .part-del').click(); })()""")
    br.pump(0.7)
    ids_match('删掉带子件的舵机 p3（孤儿回落）')
    js(br, sid, """(() => { window.POSE_PARTS.importJSON(JSON.stringify(
      {v:2, seq:99, angles:{1:90,2:90}, parts:[
        {id:'p1',name:'A',kind:'box',parent:'p2',pos:[0,0.5,0],size:[0.3,0.3,0.3],facing:'y',color:'--muted'},
        {id:'p2',name:'B',kind:'box',parent:'p1',pos:[0,1.0,0],size:[0.3,0.3,0.3],facing:'y',color:'--muted'}
      ]})); })()""")
    br.pump(0.7)
    ids_match('导入带环的 JSON')
    js(br, sid, "window.POSE_PARTS.reset()")
    br.pump(0.6)
    ids_match('reset 之后')

    errs = [e for e in br.console_errors() if 'favicon' not in e]
    check('第三期全程没有 JS 异常', not errs, errs[:4])


# ---------------- 主流程 ----------------

def main():
    print('=== 准备 ===')
    flask = start_flask()
    cookie = login()
    edge, br = start_edge()

    try:
        sid, tid = open_pose(br, cookie)

        print('\n=== 1. 姿态页加载 / 3D 就绪 ===')
        st = ready_pose(br, sid)
        check('POSE_DEBUG 报 ready，3D 起来了', st is not None, st)
        if st is None:
            print('  控制台:', br.console_errors()[-8:])
            return
        check('元件数 = 装配数（12 个默认件 → 9）', st['entries'] == 9, st)
        check('舵机节点 = 2', st['servos'] == 2, st)

        print('\n=== 2. 默认装配的世界坐标（升级不许位移）===')
        w = world_map(br, sid)
        expect = {
            'p1': (0, 0.08, 0),
            'p2': (0, 0.41, 0),
            'p3': (0, 0.71, 0),        # 舵机2 转轴
            'p4': (-0.38, 1.02, 0),    # 左叉架 = 轴 + 0.31
            'p5': (0.38, 1.02, 0),
            'p6': (0, 1.28, 0),        # 舵机1 转轴 = 0.71 + 0.57
            'p7': (0, 1.28, 0),        # 脑袋
            'p8': (0, 1.28, 0.152),    # 屏幕
            'p9': (0, 1.28, 0),        # 视线箭头
        }
        ok_all = True
        for pid, e in expect.items():
            got = w.get(pid)
            if not got:
                ok_all = False
                check('%s 存在' % pid, False)
                continue
            good = all(near(got['world'][i], e[i], 1e-9) for i in range(3))
            if not good:
                ok_all = False
            check('%s 世界坐标 %s' % (pid, e), good, got['world'] if not good else '')
        check('默认装配整体位置与升级前一致', ok_all)

        print('\n=== 3. 下发帧（顺序 + 字节）===')
        fp = js(br, sid, "document.getElementById('pose-frame-preview').textContent")
        check('帧预览 = @MOVE:servo,1,90#@MOVE:servo,2,90#',
              fp == '@MOVE:servo,1,90#@MOVE:servo,2,90#', fp)

        print('\n=== 4. 角度驱动：拖滑块 → 舵机节点转、孩子跟着走 ===')
        js(br, sid, """(() => {
          const ax = n => document.querySelector(
            '#pose-axes-list .pose-axis[data-servo="' + n + '"] .pose-axis-range');
          const r = ax(2);      // 「左右」那一路 = 舵机2（同旧 pose-pan-range）
          r.value = '0'; r.dispatchEvent(new Event('input', {bubbles:true}));
          const t = ax(1);      // 「上下」那一路 = 舵机1（同旧 pose-tilt-range）
          t.value = '135'; t.dispatchEvent(new Event('input', {bubbles:true}));
        })()""")
        br.pump(0.4)
        w2 = world_map(br, sid)
        # 舵机2：rotation.y = (0-90)° = -90°（sign 取 +1，与旧代码逐字一致）。
        # Ry(-90) 把 (x,z)=(-0.38,0) 映到 (0,-0.38) → 左叉架世界坐标 (0, 1.02, -0.38)
        p4 = w2['p4']['world']
        check('舵机2 转到 0° 后左叉架绕 Y 轴公转（半径 0.38 保持不变）',
              near(p4[0], 0, 1e-6) and near(p4[1], 1.02, 1e-6) and near(p4[2], -0.38, 1e-6), p4)
        # 转的是**舵机节点 p3**（holder 那层是定位用的，不转），所以读 p3 的本地角；
        # p4 自己只跟着父级走，它的 localRotDeg 恒为 0、world rotDeg 才是 -90°。
        check('舵机2 节点本地旋转 = -90°', near(w2['p3']['localRotDeg'][1], -90, 1e-6),
              w2['p3']['localRotDeg'])
        check('p4 本地旋转仍为 0（自己没转，是爹在转）',
              all(near(v, 0, 1e-9) for v in w2['p4']['localRotDeg']), w2['p4']['localRotDeg'])
        check('p4 世界旋转 = -90°（继承父级）', near(w2['p4']['rotDeg'][1], -90, 1e-6),
              w2['p4']['rotDeg'])
        # 舵机1：rotation.x = -(135-90)° = -45°（axis=x 时 sign=-1，同样照旧代码）
        check('舵机1 本地旋转 = -45°（135-90，sign=-1）',
              near(w2['p6']['localRotDeg'][0], -45, 1e-6), w2['p6']['localRotDeg'])
        # 脑袋在舵机1 轴上，位置不该变（轴心不动）
        check('脑袋仍在舵机1 轴心（绕轴转不位移）',
              all(near(w2['p7']['world'][i], (0, 1.28, 0)[i], 1e-6) for i in range(3)),
              w2['p7']['world'])
        # 屏幕在舵机1 下 z=0.152，Rx(-45) → (0, +0.1075, +0.1075)；
        # 加舵机1 偏移 (0,0.57,0)，再经舵机2 的 Ry(-90) → 世界 (-0.1075, 1.3875, 0)
        p8 = w2['p8']['world']
        import math
        k = 0.152 * math.sin(math.radians(45))
        exp8 = (-k, 1.28 + k, 0)
        check('屏幕绕 X 抬起（位置按旋转矩阵走，非零位移）',
              all(near(p8[i], exp8[i], 1e-6) for i in range(3)), (p8, exp8))
        fp2 = js(br, sid, "document.getElementById('pose-frame-preview').textContent")
        check('帧预览跟着更新', fp2 == '@MOVE:servo,1,135#@MOVE:servo,2,0#', fp2)

        print('\n=== 5. 加一个 servoId:3 的舵机 + 挂在它下面的方块 ===')
        before = len(world_map(br, sid))
        js(br, sid, """(() => {
          const s = document.getElementById('parts-add-kind');
          s.value = 'servo'; document.getElementById('parts-add').click();
        })()""")
        br.pump(0.4)
        after_add = js(br, sid, 'window.POSE_PARTS.get().length')
        check('加了一个元件', after_add == before + 1, (before, after_add))
        # 新舵机默认挂第一个舵机（p3）
        newp = js(br, sid, """(() => {
          const a = window.POSE_PARTS.get();
          return a.map(p => ({id:p.id, kind:p.kind, servoId:p.servoId||null, parent:p.parent}));
        })()""")
        check('新舵机拿到了 servoId=3', any(x['servoId'] == 3 for x in newp), newp[-1])
        w3 = world_map(br, sid)
        check('模型没消失（NaN 兜底生效）：元件数 = 10', len(w3) == 10, len(w3))
        check('没有 NaN 坐标',
              all(all(v == v for v in e['world']) for e in w3.values()),
              [e['id'] for e in w3.values() if any(v != v for v in e['world'])])
        fp3 = js(br, sid, "document.getElementById('pose-frame-preview').textContent")
        check('帧里出现了 servo,3 且在最后（升序）',
              fp3.endswith('@MOVE:servo,3,90#') and '@MOVE:servo,1,' in fp3, fp3)

        print('\n=== 6. 挂载：把方块挂到舵机3 → 跟着转 ===')
        sid3 = js(br, sid, "window.POSE_PARTS.get().filter(p=>p.servoId===3)[0].id")
        js(br, sid, """(() => {
          const s = document.getElementById('parts-add-kind');
          s.value = 'box'; document.getElementById('parts-add').click();
        })()""")
        br.pump(0.4)
        boxid = js(br, sid, "window.POSE_PARTS.get()[window.POSE_PARTS.get().length-1].id")
        # ★ 摆在**离轴 0.6** 的地方：摆在轴上（如 [0,0.6,0]）绕 Y 转是**不该动**的，
        #   那种情况测不出"跟不跟着转"。
        js(br, sid, "window.POSE_PARTS.update('%s', {parent:'%s', pos:[0,0,0.6]})" % (boxid, sid3))
        js(br, sid, "window.POSE_PARTS.setAngle(3, 90)")
        br.pump(0.4)
        b1 = world_map(br, sid)[boxid]['world']
        js(br, sid, "window.POSE_PARTS.setAngle(3, 180)")
        br.pump(0.4)
        b2 = world_map(br, sid)[boxid]['world']
        check('方块挂在舵机3 下，角度一改就跟着动',
              not all(near(b1[i], b2[i], 1e-6) for i in range(3)), (b1, b2))
        check('舵机3 转到 180° → 方块绕 Y 公转 90°，半径 0.6 不变',
              near(b2[1], b1[1], 1e-6)
              and near(abs(b2[0]) + abs(b2[2]), 0.6, 1e-6)
              and (abs(b2[0] - b1[0]) + abs(b2[2] - b1[2])) > 0.5, (b1, b2))

        print('\n=== 7. 不泄漏：连点增删 + 手拖尺寸 ===')
        js(br, sid, "window.POSE_PARTS.setAngle(3, 90)")
        br.pump(0.3)
        base = js(br, sid, 'window.POSE_DEBUG.info()')
        js(br, sid, """(() => {
          const s = document.getElementById('parts-add-kind'); s.value='box';
          const add = document.getElementById('parts-add');
          for (let i=0;i<20;i++) { add.click(); }
        })()""")
        br.pump(0.5)
        n_after_add = js(br, sid, 'window.POSE_PARTS.get().length')
        js(br, sid, """(() => {
          const a = window.POSE_PARTS.get().slice();
          for (let i=a.length-1; i>=0 && a.length>9; i--) {
            if (a[i].kind === 'box' && a[i].parent !== 'p3') { window.POSE_PARTS.remove(a[i].id); }
          }
        })()""")
        br.pump(0.5)
        js(br, sid, 'window.POSE_DEBUG.rebuild()')
        br.pump(0.5)
        peak = js(br, sid, 'window.POSE_DEBUG.info()')
        js(br, sid, """(() => {
          // 拖 100 次尺寸：就地改，不许重建
          const p = window.POSE_PARTS.get().find(x=>x.kind==='box' && x.id!=='p4');
          if (p) { for (let i=0;i<100;i++) { window.POSE_PARTS.update(p.id, {size:[0.3+i*0.001,0.4,0.4]}); } }
        })()""")
        br.pump(0.6)
        after_drag = js(br, sid, 'window.POSE_DEBUG.info()')
        print('    info: base=%s  peak=%s  after_drag=%s' % (base, peak, after_drag))
        check('拖 100 次尺寸后几何数**一点没涨**（就地改生效）',
              after_drag['geometries'] == peak['geometries'],
              (peak['geometries'], after_drag['geometries']))
        check('增删 20 个后几何数没有单调爬升（回到同量级）',
              peak['geometries'] <= base['geometries'] + 24,
              (base['geometries'], peak['geometries']))

        print('\n=== 8. 反例：parent 成环不许卡死 ===')
        js(br, sid, """(() => {
          window.POSE_PARTS.importJSON(JSON.stringify({v:2, seq:99, angles:{1:90,2:90}, parts:[
            {id:'p1',name:'A',kind:'box',parent:'p2',pos:[0,0.5,0],size:[0.3,0.3,0.3],facing:'y',color:'--muted'},
            {id:'p2',name:'B',kind:'box',parent:'p1',pos:[0,1.0,0],size:[0.3,0.3,0.3],facing:'y',color:'--muted'}
          ]}));
        })()""")
        br.pump(0.6)
        cyc = js(br, sid, """(() => {
          const a = window.POSE_PARTS.get().map(p=>({id:p.id,parent:p.parent}));
          const st = window.POSE_DEBUG.state();
          return {a:a, st:st};
        })()""")
        check('环被打破（至少一个回落 base）',
              any(x['parent'] == 'base' for x in cyc['a']), cyc['a'])
        check('页面没卡死，3D 仍在跑', cyc['st']['ready'] and not cyc['st']['dead'], cyc['st'])
        check('两个元件都建出来了（没静默丢件）', cyc['st']['entries'] == 2, cyc['st'])
        check('★ 直接调 importJSON 后，清单 DOM 与数据一致（都是 2 行）',
              js(br, sid, "document.querySelectorAll('#parts-list .part-row').length") == 2,
              js(br, sid, "document.querySelectorAll('#parts-list .part-row').length"))

        print('\n=== 9. 恢复默认 + 主题切换 ===')
        js(br, sid, "window.POSE_PARTS.reset()")
        br.pump(0.5)
        w5 = world_map(br, sid)
        check('恢复默认后仍是 9 件', len(w5) == 9, len(w5))
        check('★ 直接调 reset 后，清单 DOM / 计数文本 / 3D 三者一致',
              js(br, sid, "document.querySelectorAll('#parts-list .part-row').length") == 9
              and js(br, sid, "document.getElementById('parts-count').textContent").startswith('9 /'),
              (js(br, sid, "document.querySelectorAll('#parts-list .part-row').length"),
               js(br, sid, "document.getElementById('parts-count').textContent")))
        check('恢复默认后 p6 世界坐标仍 = (0,1.28,0)',
              all(near(w5['p6']['world'][i], (0, 1.28, 0)[i], 1e-9) for i in range(3)),
              w5['p6']['world'])
        js(br, sid, """(() => {
          const b = document.getElementById('theme-toggle') || document.querySelector('[data-theme-toggle]');
          if (b) { b.click(); return 'clicked'; }
          document.documentElement.setAttribute('data-theme','dark');
          window.dispatchEvent(new CustomEvent('themechange', {detail:{theme:'dark'}}));
          return 'event';
        })()""")
        br.pump(0.6)
        st_after_theme = js(br, sid, 'window.POSE_DEBUG.state()')
        check('换主题后 3D 没死、元件还在',
              st_after_theme['ready'] and not st_after_theme['dead'] and st_after_theme['entries'] == 9,
              st_after_theme)
        bg = js(br, sid, "window.POSE_DEBUG.snapshot() && (function(){const c=document.querySelector('#pose-stage canvas');return c?[c.width,c.height]:null;})()")
        check('canvas 尺寸非零', bg and bg[0] > 0 and bg[1] > 0, bg)

        print('\n=== 10. 截图（留档 + 变化检测）===')
        n1 = br.screenshot(sid, os.path.join(TMP, 'pose_after.png'))
        check('截图拿到了', n1 > 10000, n1)

        print('\n=== 11. 切页签再切回 ===')
        js(br, sid, "window.DASH_API.showTab('data')")
        br.pump(0.6)
        js(br, sid, "window.DASH_API.showTab('pose')")
        br.pump(0.8)
        st_ret = js(br, sid, 'window.POSE_DEBUG.state()')
        wh = js(br, sid, "(function(){const c=document.querySelector('#pose-stage canvas');return c?[c.width,c.height]:null;})()")
        check('切回来 3D 继续跑（rafId 非 0）', st_ret['rafId'] != 0, st_ret)
        check('切回来 canvas 尺寸正常（没被压成 0×0）', wh and wh[0] > 0 and wh[1] > 0, wh)
        check('切回来元件还是 9 个', st_ret['entries'] == 9, st_ret)

        print('\n=== 12. 控制台有没有报错 ===')
        errs = br.console_errors()
        bad = [e for e in errs if 'favicon' not in e and 'net::ERR' not in e]
        check('没有 JS 异常 / error 级日志', not bad, bad[:6])

        print('\n=== 13. localStorage 落盘 ===')
        saved = js(br, sid, "localStorage.getItem('pose_parts_v1') ? JSON.parse(localStorage.getItem('pose_parts_v1')).parts.length : null")
        check('已写入 localStorage（9 件）', saved == 9, saved)

        print('\n=== 14. 迁移：旧 pose_view_v1 的反向开关不许丢 ===')
        # 反向开关是拿实物校准过的值。旧键里故意把两个都设成 true + 非中位角度，
        # 然后整页重载 —— 走的就是"新用户第一次打开的升级路径"。
        js(br, sid, """(() => {
          localStorage.clear();
          localStorage.setItem('pose_view_v1', JSON.stringify({
            angle: {pan: 30, tilt: 150},
            invert: {pan: true, tilt: true}
          }));
          location.reload();
        })()""")
        br.pump(0.5)
        for _ in range(60):
            if any(m.get('method') == 'Page.loadEventFired' for m in br.events[-12:]):
                break
            br.pump(0.25)
        st2 = ready_pose(br, sid)
        check('重载后 3D 仍就绪', st2 is not None, st2)
        mig = js(br, sid, """(() => {
          const a = window.POSE_PARTS.get();
          const inv = {};
          a.forEach(p => { if (p.kind === 'servo') inv[p.servoId] = p.invert; });
          return {
            inv: inv,
            a1: window.POSE_PARTS.angleOf(1),
            a2: window.POSE_PARTS.angleOf(2),
            legacy: localStorage.getItem('pose_view_v1'),
            newKey: !!localStorage.getItem('pose_parts_v1'),
            chk2: document.querySelector(
              '#pose-axes-list .pose-axis[data-servo="2"] .pose-axis-inv input').checked,
            chk1: document.querySelector(
              '#pose-axes-list .pose-axis[data-servo="1"] .pose-axis-inv input').checked
          };
        })()""")
        check('舵机2 的反向被搬过来了', mig['inv'].get('2') is True, mig['inv'])
        check('舵机1 的反向被搬过来了', mig['inv'].get('1') is True, mig['inv'])
        check('角度也搬过来了（pan30→舵机2, tilt150→舵机1）',
              mig['a1'] == 150 and mig['a2'] == 30, (mig['a1'], mig['a2']))
        check('右边那两条「反向」勾跟着显示为勾上（一个真相两个入口）',
              mig['chk1'] is True and mig['chk2'] is True, (mig['chk1'], mig['chk2']))
        check('旧键搬完即删', mig['legacy'] is None, mig['legacy'])
        check('新键已建立（搬完立刻落盘，不等用户动手）', mig['newKey'] is True)
        # 落盘的内容里得**真的带着**那两个 true，光有键不算数
        persisted = js(br, sid, """(() => {
          const d = JSON.parse(localStorage.getItem('pose_parts_v1') || '{}');
          const inv = {};
          (d.parts || []).forEach(p => { if (p.kind === 'servo') inv[p.servoId] = p.invert; });
          return {inv: inv, angles: d.angles};
        })()""")
        check('落盘内容里反向=true 都在', persisted['inv'].get('1') is True and persisted['inv'].get('2') is True,
              persisted)
        check('落盘内容里角度也都是搬过来的值',
              persisted['angles'].get('1') == 150 and persisted['angles'].get('2') == 30, persisted['angles'])
        # 再整页重载一次：这一次没有任何旧键可搬，必须从新键读回来
        js(br, sid, "location.reload()")
        br.pump(0.5)
        for _ in range(60):
            if any(m.get('method') == 'Page.loadEventFired' for m in br.events[-12:]):
                break
            br.pump(0.25)
        ready_pose(br, sid)
        again = js(br, sid, """(() => {
          const a = window.POSE_PARTS.get(); const inv = {};
          a.forEach(p => { if (p.kind === 'servo') inv[p.servoId] = p.invert; });
          return {inv: inv, a1: window.POSE_PARTS.angleOf(1), a2: window.POSE_PARTS.angleOf(2)};
        })()""")
        check('★ 第二次打开（无旧键可搬）反向仍然保住了',
              again['inv'].get('1') is True and again['inv'].get('2') is True, again)
        check('★ 第二次打开角度也保住了',
              again['a1'] == 150 and again['a2'] == 30, (again['a1'], again['a2']))
        # 反向开关只影响模型，不许影响下发
        js(br, sid, "window.POSE_PARTS.setAngle(1, 90); window.POSE_PARTS.setAngle(2, 90);")
        br.pump(0.3)
        fp4 = js(br, sid, "document.getElementById('pose-frame-preview').textContent")
        check('反向开关不影响下发字节', fp4 == '@MOVE:servo,1,90#@MOVE:servo,2,90#', fp4)
        # 90° 中位不受反向影响（反向只翻方向、不翻中位）
        js(br, sid, "window.POSE_PARTS.setAngle(2, 0)")
        br.pump(0.3)
        w4b = world_map(br, sid)
        check('反向=true 时 舵机2 到 0° → p4 世界旋转为 +90°（方向被翻过来了）',
              near(w4b['p4']['rotDeg'][1], 90, 1e-6), w4b['p4']['rotDeg'])

        # 第三期：动态角度抽屉 / 行程与倾角 / 清单缩进成树
        # （它自己会 localStorage.clear() + reload，不依赖上面几节留下的状态）
        phase_v3(br, sid)

        # 第四期：分区（分块看图）+ 箭头永不被挡
        phase_v4(br, sid)

        print('\n=== 18. 静态资源 MIME ===')
        s = requests.Session()
        s.cookies.set('session', cookie)
        for path in ('/static/page-pose.js', '/static/pose-parts.js',
                     '/static/vendor/three.module.min.js',
                     '/static/vendor/three-addons/controls/OrbitControls.js'):
            r = s.get(BASE + path)
            ct = r.headers.get('Content-Type', '')
            check('%s MIME = javascript' % path,
                  r.status_code == 200 and 'javascript' in ct,
                  '%s %s' % (r.status_code, ct))

        print('\n=== 19. 禁 WebGL 的降级路径（另开一个浏览器）===')
        phase_nowebgl(cookie)

    finally:
        print('\n=== 清场 ===')
        close(edge)
        if flask:
            close(flask)
        time.sleep(0.6)
        shutil.rmtree(PROFILE, ignore_errors=True)

    print('\n================= 汇总 =================')
    bad = [r for r in RESULTS if not r[1]]
    print('共 %d 项，通过 %d，失败 %d' % (len(RESULTS), len(RESULTS) - len(bad), len(bad)))
    for n, _, d in bad:
        print('  FAIL  %s  %s' % (n, d))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
