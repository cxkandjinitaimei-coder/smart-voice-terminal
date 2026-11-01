# -*- coding: utf-8 -*-
"""姿态页「抽屉布局 + 三轴拖拽」验收 —— headless Edge + 手写 CDP 客户端。

跑法：  cd d:\\ArduinoIDE\\ArdProject\\flask_relay
        D:\\Python\\python.exe test_pose_drag.py
        （**必须用 D:\\Python\\python.exe** —— PowerShell 会把无 BOM 的 UTF-8 按 GBK 读）

⚠️ 这里的拖拽**全部是 `Input.dispatchMouseEvent` 打出去的真实指针事件**，
   不调任何内部 API。理由：真正会翻车的地方（捕获阶段抢不过 OrbitControls、
   pointerup 丢在窗口外面、退化时除零飞出去）只在真事件流里才露头。
   调 API 的测试能全绿，功能照样是坏的。

⚠️ 这些断言是"改坏能红"的，别因为红了先把断言删掉。
   本轮实际红过两次：一次是断言自己写错了（拿世界坐标比局部坐标），
   一次是真的布局 bug（见下）—— 两种都得先看清楚是哪种。

配套：test_pose_page.py（71 项，元件清单 / 角度 / 下发 / 迁移 / 降级路径）。
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
from cdp_min import Browser, wait_port          # noqa: E402

APP = os.path.dirname(os.path.abspath(__file__))
PY = r'D:\Python\python.exe'
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
PORT = 9333
BASE = 'http://127.0.0.1:5000'
TMP = os.path.join(tempfile.gettempdir(), 'pose_verify')
os.makedirs(TMP, exist_ok=True)
PROFILE = os.path.join(TMP, 'edge_drag_profile')

RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok), detail))
    print(('  [OK]   ' if ok else '  [FAIL] ') + name + (('  -> ' + str(detail)) if detail else ''))
    return ok


def token():
    with open(os.path.join(APP, 'config.json'), 'r', encoding='utf-8') as f:
        return json.load(f).get('token', '')


def start_flask():
    if wait_port(5000, 1):
        print('  Flask 已在 5000 上跑着，复用')
        return None
    logf = open(os.path.join(TMP, 'flask_drag.log'), 'wb')
    p = subprocess.Popen([PY, 'app.py'], cwd=APP, stdout=logf, stderr=subprocess.STDOUT)
    if not wait_port(5000, 30):
        raise SystemExit('Flask 起不来，看 ' + os.path.join(TMP, 'flask_drag.log'))
    print('  Flask 已起，pid=%d' % p.pid)
    return p


def start_edge():
    if os.path.isdir(PROFILE):
        shutil.rmtree(PROFILE, ignore_errors=True)
    p = subprocess.Popen(
        [EDGE, '--headless=new', '--remote-debugging-port=%d' % PORT,
         '--user-data-dir=' + PROFILE, '--no-first-run', '--no-default-browser-check',
         '--disable-extensions', '--disable-sync', '--window-size=1440,1000',
         '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
         'about:blank'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_port(PORT, 30):
        raise SystemExit('Edge 调试端口没起来')
    for _ in range(40):
        try:
            return p, Browser(PORT)
        except Exception:
            time.sleep(0.5)
    raise SystemExit('连不上 CDP')


def login():
    s = requests.Session()
    s.post(BASE + '/login', data={'token': token()}, allow_redirects=False)
    ck = s.cookies.get('session')
    if not ck:
        raise SystemExit('登录没拿到 session cookie')
    return ck


flask = None
edge = None
try:
    flask = start_flask()
    ck = login()
    edge, br = start_edge()

    sid, tid = br.new_page('about:blank')
    for m in ('Page.enable', 'Runtime.enable', 'Log.enable'):
        br.call(m, {}, session=sid)
    br.call('Network.enable', {}, session=sid)
    br.call('Network.setCookie', {'name': 'session', 'value': ck,
                                  'domain': '127.0.0.1', 'path': '/',
                                  'httpOnly': True}, session=sid)
    br.call('Page.navigate', {'url': BASE + '/#pose'}, session=sid)
    for _ in range(120):
        if any(e.get('method') == 'Page.loadEventFired' for e in br.events):
            break
        br.pump(0.25)
    br.pump(2.5)

    def js(e):
        return br.evaluate(e, sid)

    def mouse(t, x, y, buttons=0):
        br.call('Input.dispatchMouseEvent',
                {'type': t, 'x': x, 'y': y,
                 'button': 'left' if t != 'mouseMoved' else 'none',
                 'buttons': buttons, 'clickCount': 1 if t != 'mouseMoved' else 0},
                session=sid)

    def drag(x0, y0, dx, dy, steps=6):
        """真实指针拖拽：按下 → 分步移动 → 松开"""
        mouse('mousePressed', x0, y0, 0)
        for k in range(1, steps + 1):
            mouse('mouseMoved', x0 + dx * k / steps, y0 + dy * k / steps, 1)
            br.pump(0.05)
        mouse('mouseReleased', x0 + dx, y0 + dy, 0)
        br.pump(0.35)

    def rect(sel):
        return js("(function(){var e=document.querySelector('%s');"
                  "if(!e)return null;var r=e.getBoundingClientRect();"
                  "return {x:r.left,y:r.top,w:Math.round(r.width),h:Math.round(r.height),"
                  "cx:r.left+r.width/2,cy:r.top+r.height/2}})()" % sel)

    def pos_of(pid):
        return js("(function(){var p=POSE_PARTS.byId('%s');return p?p.pos.slice():null})()" % pid)

    def dom_of(pid):
        return js("(function(){var o={};document.querySelectorAll('[data-pos-for=\"%s\"]')"
                  ".forEach(function(i){var k=i.getAttribute('data-pos-axis')+'_'+"
                  "(i.closest('.pose-selpanel')?'panel':'row');o[k]=parseFloat(i.value)});"
                  "return o})()" % pid)

    # ★ 前置闸门（第四期新增）：这一整套 72 条断言**全部建立在"桌面停靠三栏"这个前提上**
    #   ——宽度算得对不对、分隔条能不能拖、420 会不会被夹……全都要在 wide 模式下才有意义。
    #   窄屏模式下抽屉是浮层、分隔条是 display:none、宽度根本不进数学，
    #   那时候后面每一条都会以**错误的前提**通过或失败，红得莫名其妙。
    #   所以这里先把它钉在明处：宽屏 headless 环境本该 fine pointer，哪天
    #   (pointer:coarse) 被报成真（比如 CDP 默认值变了、或有人加了设备模拟），
    #   是这一行先红，而不是让 72 条断言集体说谎。
    print('\n=== -1. 前置：必须是 wide 模式（后面全部断言的前提）===')
    pre = js("""({mode: document.documentElement.dataset.poseMode,
                  coarse: matchMedia('(pointer: coarse)').matches,
                  w: innerWidth})""")
    check('★ 前置：指针是 fine（宽屏 headless 下 (pointer:coarse) 必须为假）',
          pre['coarse'] is False, pre)
    check('★ 前置：模式判定为 wide（不是 narrow 浮层）—— 红了说明后面 72 条前提已失效，'
          '先查设备模拟/媒体查询，别去改断言的期望值',
          pre['mode'] == 'wide', pre)
    check('★ 前置：视口确实是宽的（> 860，宽度断点不该触发）', pre['w'] > 860, pre)

    print('\n=== 0. 抽屉布局：三栏 / 拖分隔条 / 拖高度把手 ===')
    lay = js('POSE_LAYOUT.get()')
    check('布局模块起来了', isinstance(lay, dict) and lay.get('leftW'), lay)
    check('默认三栏：左 320 / 右 300',
          lay['leftW'] == 320 and lay['rightW'] == 300, lay)

    # ⚠️ 这条是 2026-09-16 那个真 bug 的拦网：抽屉正文若回到文档流里，
    #    它会把 align-items:stretch 的 flex 行撑到一千多像素高，
    #    于是点一个元件 scrollIntoView 就把整页滚走。
    wb = rect('#pose-workbench')
    check('工作台高度由 3D 卡片决定（没有被抽屉清单撑高）',
          wb and wb['h'] < 700, wb)
    check('抽屉与画布卡等高', wb and abs(rect('#drawer-parts')['h'] - wb['h']) <= 1,
          (wb, rect('#drawer-parts')))
    check('清单在自己的滚动容器里滚（不是把页面撑长）',
          js("(function(){var s=document.querySelector('.drawer-scroll');"
             "return s.scrollHeight>s.clientHeight})()"))
    check('页面当前没有被滚下去', js('window.scrollY') == 0, js('window.scrollY'))

    w0 = lay['leftW']
    r = rect('#split-left')
    drag(r['cx'], r['cy'], 70, 0)
    check('往右拖左分隔条 → 左抽屉变宽约 70px',
          abs(js('POSE_LAYOUT.get().leftW') - (w0 + 70)) <= 2,
          '%s → %s' % (w0, js('POSE_LAYOUT.get().leftW')))
    check('画布跟着变窄（宽度是"占满剩余"，不是固定值）',
          rect('.pose-canvas-card')['w'] < 900)

    # ★★ 2026-09-16 真 bug 的拦网：**分成很多小步拖，并且每步都断言"还在拖"**。
    #    上面那条"一步拖到位"的用例会**因为事件顺序碰巧通过**。实测有两处会提前把拖拽收尾：
    #      · 浏览器在鼠标离开元素时会发 `lostpointercapture`（哪怕 setPointerCapture 没生效），
    #        而且它排在 `pointermove` **之前** —— 一步到位时那一帧先算上了，多步就会在
    #        **第一步离开元素时**被打断，抽屉只动几个像素，且零报错；
    #      · 关键是这个事件**不是每个浏览器配置都会发**（实测正常档不发、`--disable-3d-apis`
    #        那一档发）。所以"只比较最终宽度"这种断言靠不住 —— 得直接断言
    #        **中途拖拽状态一直活着**，那才是真正要守的性质，跟触发源无关。
    w1 = js('POSE_LAYOUT.get().leftW')
    r = rect('#split-left')
    mouse('mousePressed', r['cx'], r['cy'], 0)
    br.pump(0.1)
    alive, broke_at = True, 0
    for k in range(1, 21):                      # 每步 3px，第 1~2 步就出了那条 6px 宽的分隔条
        mouse('mouseMoved', r['cx'] - 3 * k, r['cy'], 1)
        br.pump(0.05)
        if js('POSE_LAYOUT.dragging()')['left'] is not True:
            alive, broke_at = False, k
            break
    mouse('mouseReleased', r['cx'] - 60, r['cy'], 0)
    br.pump(0.4)
    check('★ 多步拖分隔条：中途没有被提前收尾（拖出那 6px 宽的条也照样算数）',
          alive, '第 %d 步就被收尾了' % broke_at if not alive else '')
    check('★ 多步拖分隔条：累计到位',
          abs(js('POSE_LAYOUT.get().leftW') - (w1 - 60)) <= 2,
          '%s → %s' % (w1, js('POSE_LAYOUT.get().leftW')))

    h1 = js('POSE_LAYOUT.get().stageH')
    g = rect('#pose-grip')
    mouse('mousePressed', g['cx'], g['cy'], 0)
    br.pump(0.1)
    alive2, broke2 = True, 0
    for k in range(1, 21):
        mouse('mouseMoved', g['cx'], g['cy'] + 3 * k, 1)
        br.pump(0.05)
        if js('POSE_LAYOUT.dragging()')['grip'] is not True:
            alive2, broke2 = False, k
            break
    mouse('mouseReleased', g['cx'], g['cy'] + 60, 0)
    br.pump(0.4)
    check('★ 多步拖高度把手：中途没有被提前收尾',
          alive2, '第 %d 步就被收尾了' % broke2 if not alive2 else '')
    check('★ 多步拖高度把手：累计到位',
          abs(js('POSE_LAYOUT.get().stageH') - (h1 + 60)) <= 2,
          '%s → %s' % (h1, js('POSE_LAYOUT.get().stageH')))

    h0 = js('POSE_LAYOUT.get().stageH')
    g = rect('#pose-grip')
    drag(g['cx'], g['cy'], 0, 90)
    check('往下拖右下角把手 → 画布变高约 90px',
          abs(js('POSE_LAYOUT.get().stageH') - (h0 + 90)) <= 2,
          '%s → %s' % (h0, js('POSE_LAYOUT.get().stageH')))
    check('画布 canvas 真的跟着变高了（ResizeObserver 生效）',
          rect('#pose-stage canvas')['h'] > 430)

    # 收起 / 展开：边条和头部的箭头两个入口
    js("POSE_LAYOUT.setW('left',320); POSE_LAYOUT.setH(440)")
    br.pump(0.3)
    t = rect('#drawer-parts-tab')
    mouse('mousePressed', t['cx'], t['cy'], 0)
    mouse('mouseReleased', t['cx'], t['cy'], 0)
    br.pump(0.4)
    check('点边条 → 左抽屉收起成 30px 窄条',
          js('POSE_LAYOUT.get().leftOpen') is False and rect('#drawer-parts')['w'] == 30,
          rect('#drawer-parts'))
    check('收起后边条仍然可见可点（"至少留一个能打开的位置"）',
          js("!!document.querySelector('#drawer-parts .drawer-tab').offsetParent"))
    mouse('mousePressed', t['cx'], t['cy'], 0)
    mouse('mouseReleased', t['cx'], t['cy'], 0)
    br.pump(0.4)
    check('再点一次 → 展开回来', js('POSE_LAYOUT.get().leftOpen') is True)

    print('\n=== 1. 逐轴拖拽（真实指针事件）===')
    for axis in ('x', 'y', 'z'):
        js("POSE_EDIT_DEBUG.select('p7')")
        br.pump(0.3)
        before = pos_of('p7')
        p0 = js("POSE_EDIT_DEBUG.axisPoint('%s',0.55)" % axis)
        if not p0:
            check('沿 %s 轴拖：拿得到轴上的点' % axis, False, 'axisPoint 是 null')
            continue
        drag(p0['x'], p0['y'], 84, 60 if axis == 'y' else 0, steps=6)
        after = pos_of('p7')
        i = 'xyz'.index(axis)
        check('沿 %s 轴拖：这一轴确实动了' % axis, abs(after[i] - before[i]) > 1e-9,
              '%s → %s' % (before, after))
        check('沿 %s 轴拖：另外两轴逐位不变' % axis,
              all(abs(after[j] - before[j]) < 1e-12 for j in range(3) if j != i),
              '%s → %s' % (before, after))
        check('沿 %s 轴拖：没飞出 ±8' % axis,
              all(-8.0000001 <= v <= 8.0000001 for v in after), after)
        d = dom_of('p7')
        check('沿 %s 轴拖：两个入口（行内 + 顶部面板）的坐标框都跟上了' % axis,
              d and all(abs(d.get('%s_%s' % (a, w)) - after[j]) < 1e-9
                        for j, a in enumerate('xyz') for w in ('row', 'panel')), d)

    # ★★ 2026-09-16 真 bug 的拦网：**光标拖出画布之后这一拖还算不算数**。
    #    画布只有五百来像素宽，用户把箭头往外拖是常事。浏览器在光标离开元素时会发
    #    `lostpointercapture`（哪怕压根没捕获上），挂它来"收尾/取消"就会让整个拖拽
    #    **弹回原位**、松手什么都没改 —— 零报错，用户只觉得"拖不动"。
    #    所以这里故意把光标拖到画布外面再松手，断言落库的位置**不是起点**。
    js("POSE_EDIT_DEBUG.select('p7')")
    br.pump(0.3)
    cvs = js("(function(){var b=document.querySelector('#pose-stage canvas').getBoundingClientRect();"
             "return {r:b.right,b:b.bottom,w:b.width,h:b.height}})()")
    before = pos_of('p7')
    p0 = js("POSE_EDIT_DEBUG.axisPoint('x',0.4)")
    # 一路拖出画布右下角（终点 x/y 都超过画布边界）
    drag(p0['x'], p0['y'], (cvs['r'] + 40) - p0['x'], (cvs['b'] + 40) - p0['y'], steps=16)
    after = pos_of('p7')
    check('★ 把箭头拖出画布再松手：这一拖照样算数（不许弹回起点）',
          abs(after[0] - before[0]) > 0.05, '%s → %s' % (before, after))
    check('★ 拖出画布松手后：拖拽状态收干净、控制器回到可用',
          js('POSE_EDIT_DEBUG.dragging()') is False
          and js('POSE_EDIT_DEBUG.controlsEnabled()') is True)
    js("(function(){var i=document.querySelector('.part-row[data-part=\"p7\"] [data-pos-axis=\"x\"]');"
       "i.value='0';i.dispatchEvent(new Event('input',{bubbles:true}))})()")
    js("(function(){var i=document.querySelector('.part-row[data-part=\"p7\"] [data-pos-axis=\"y\"]');"
       "i.value='0';i.dispatchEvent(new Event('input',{bubbles:true}))})()")
    br.pump(0.4)

    print('\n=== 2. posOf() 陷阱：拖完再手改一轴，另外两轴不许回滚 ===')
    js("POSE_EDIT_DEBUG.select('p7')")
    before = pos_of('p7')
    js("(function(){var i=document.querySelector('.part-row[data-part=\"p7\"] [data-pos-axis=\"x\"]');"
       "i.value='1.25';i.dispatchEvent(new Event('input',{bubbles:true}))})()")
    br.pump(0.4)
    after = pos_of('p7')
    check('手改 X 之后：Y/Z 没有被旧值回滚',
          abs(after[1] - before[1]) < 1e-12 and abs(after[2] - before[2]) < 1e-12,
          '%s → %s' % (before, after))

    print('\n=== 3. 3D 点选 ↔ 侧边栏联动 ===')
    js("POSE_EDIT_DEBUG.select(null)")
    br.pump(0.2)
    sp = js("POSE_EDIT_DEBUG.screenPos('p4')")
    mouse('mousePressed', sp['x'], sp['y'], 0)
    mouse('mouseReleased', sp['x'], sp['y'], 0)
    br.pump(0.4)
    check('3D 里点元件 → 选中它', js('POSE_EDIT_DEBUG.selected()') == 'p4',
          js('POSE_EDIT_DEBUG.selected()'))
    check('3D 里点元件 → 清单对应行标了选中态',
          js("(function(){var r=document.querySelector('.part-row.is-selected');"
             "return r?r.getAttribute('data-part'):null})()") == 'p4')
    # ⚠️ 要拿**世界坐标**比：p4 挂在舵机上，p.pos 是局部坐标，
    #    local 的 y 和世界的 y 不是一回事（第一版断言就是这么写错的）。
    check('3D 里点元件 → 手柄落到这个元件的世界位置上',
          js("(function(){var g=POSE_EDIT_DEBUG.gizmo();"
             "var w=POSE_DEBUG.snapshot().filter(function(e){return e.id==='p4'})[0];"
             "if(!g.visible||!w)return false;var d=0;"
             "for(var i=0;i<3;i++){d=Math.max(d,Math.abs(g.pos[i]-w.world[i]))}return d<1e-6})()"))
    check('点空白 → 取消选中',
          js("(function(){var c=document.querySelector('#pose-stage canvas');"
             "var r=c.getBoundingClientRect();"
             "var o={bubbles:true,clientX:r.left+8,clientY:r.top+8,button:0,pointerId:1};"
             "c.dispatchEvent(new PointerEvent('pointerdown',o));"
             "c.dispatchEvent(new PointerEvent('pointerup',o));"
             "return POSE_EDIT_DEBUG.selected()===null})()"))

    js("POSE_EDIT_DEBUG.select('p4')")
    br.pump(0.3)
    c = rect('#pose-stage canvas')
    drag(c['x'] + 40, c['y'] + 40, 84, 42, steps=7)
    check('转视角（位移 > 4px）不许改掉选中项',
          js('POSE_EDIT_DEBUG.selected()') == 'p4', js('POSE_EDIT_DEBUG.selected()'))
    check('转视角之后 OrbitControls 还是开的', js('POSE_EDIT_DEBUG.controlsEnabled()') is True)

    print('\n=== 4. 结构变化：删掉选中件 ===')
    n_geo = js('POSE_DEBUG.info().geometries')
    js("(function(){var b=document.querySelector('.part-row[data-part=\"p4\"] .part-del');"
       "if(b)b.click()})()")
    br.pump(0.5)
    check('删掉选中件 → 手柄收起、选中态清掉、不报错',
          js('POSE_EDIT_DEBUG.gizmo().visible') is False
          and js('POSE_EDIT_DEBUG.selected()') is None, js('POSE_DEBUG.state()'))
    check('删掉选中件 → 几何数没暴涨',
          js('POSE_DEBUG.info().geometries') <= n_geo + 2,
          '%s → %s' % (n_geo, js('POSE_DEBUG.info().geometries')))

    print('\n=== 5. 拖到一半切页签：不许把视角锁死 ===')
    js("POSE_EDIT_DEBUG.select('p6')")
    br.pump(0.3)
    p0 = js("POSE_EDIT_DEBUG.axisPoint('y',0.55)")
    mouse('mousePressed', p0['x'], p0['y'], 0)
    mouse('mouseMoved', p0['x'], p0['y'] - 30, 1)
    br.pump(0.1)
    check('拖拽中：dragging 是 true', js('POSE_EDIT_DEBUG.dragging()') is True)
    check('拖拽中：OrbitControls 被关掉', js('POSE_EDIT_DEBUG.controlsEnabled()') is False)
    js("DASH_API.showTab('test')")                  # 中途切走，pointerup 永远不会来
    br.pump(0.6)
    check('切页签后：手柄自己收尾，dragging 归 false', js('POSE_EDIT_DEBUG.dragging()') is False)
    check('切页签后：OrbitControls 回到可用（否则视角永远转不动，且零报错）',
          js('POSE_EDIT_DEBUG.controlsEnabled()') is True)
    js("DASH_API.showTab('pose')")
    br.pump(0.6)
    check('切回姿态页：手柄回来了、还能拖', js('POSE_EDIT_DEBUG.gizmo().visible') is True)

    print('\n=== 6. 反复点选 / 拖拽：不许泄漏几何 ===')
    base_geo = js('POSE_DEBUG.info().geometries')
    for i in range(50):
        js("POSE_EDIT_DEBUG.select('%s')" % ('p6' if i % 2 else 'p8'))
    br.pump(0.5)
    p0 = js("POSE_EDIT_DEBUG.axisPoint('x',0.55)")
    for _ in range(30):
        mouse('mousePressed', p0['x'], p0['y'], 0)
        mouse('mouseMoved', p0['x'] + 20, p0['y'], 1)
        mouse('mouseReleased', p0['x'] + 20, p0['y'], 0)
        br.pump(0.02)
    br.pump(0.6)
    check('点选 50 次 + 拖 30 次后几何数回到基线（手柄只建一次）',
          js('POSE_DEBUG.info().geometries') == base_geo,
          '%s → %s' % (base_geo, js('POSE_DEBUG.info().geometries')))

    print('\n=== 7. 布局持久化：刷新页面还回得来 ===')
    want = js("(function(){POSE_LAYOUT.setW('left',420);POSE_LAYOUT.setH(520);"
              "POSE_LAYOUT.setOpen('right',false);return POSE_LAYOUT.get()})()")
    br.pump(0.8)
    # ⚠️ 这里必须用 Page.reload，**不能**再 Page.navigate 到同一个 URL。
    #    当前地址已经是 http://127.0.0.1:5000/#pose，再导航到一模一样的 URL+hash
    #    属于**同文档导航**（只是改 hash 那一类），浏览器压根不重新加载文档，
    #    于是 loadEventFired 永远不来、循环空转到超时，页面还是旧的那一份 ——
    #    检查 1~3 会"碰巧"通过（状态本来就还在内存里），检查 4 必然失败。
    br.events.clear()
    br.call('Page.reload', {}, session=sid)
    for _ in range(120):
        if any(e.get('method') == 'Page.loadEventFired' for e in br.events):
            break
        br.pump(0.25)
    br.pump(2.0)
    got = js('POSE_LAYOUT.get()')
    check('刷新后三条尺寸都回得来', got['leftW'] == want['leftW'] and got['stageH'] == want['stageH'],
          '%s → %s' % (want, got))
    check('刷新后开合状态也回得来（右抽屉还是收起的）', got['rightOpen'] is False, got)
    check('刷新后页面上真的按存档的尺寸渲染了',
          rect('#drawer-parts')['w'] == want['leftW']
          and rect('#drawer-axes')['w'] == 30,
          (rect('#drawer-parts'), rect('#drawer-axes')))
    check('刷新后选中态**没有**被记住（瞬态，不该跟着存档走）',
          js('POSE_EDIT_DEBUG.selected()') is None)

    print('\n=== 8. 父件被转过时，拖拽写回的必须是**局部**坐标 ===')
    # pos 是相对父件的。父件（舵机组）被转到 45° 之后，沿世界 X 拖出来的位移
    # 换成局部坐标就不再平行于 X 了。要是图省事把世界坐标直接当 pos 提交，
    # 元件会**绕着父件打转**、跑到别处去 —— 而且同样是一句报错都没有。
    # 所以这条既断言"世界落点确实沿着世界 X 走"，也断言"局部增量确实被转过"，
    # 后者是防"父件其实没转、这条测试白测"的空转对照。
    servo_id = js("(function(){var P=POSE_PARTS.get();"
                  "for(var i=0;i<P.length;i++){if(P[i].servoId!=null){"
                  "var k=P[i].servoId;"
                  "for(var j=0;j<P.length;j++){if(P[j].parent===P[i].id){"
                  "return {servo:String(k),child:P[j].id,childName:P[j].name}}}}}return null})()")
    check('找到了"挂在舵机上"的元件（找不到就没法测父子换算）', bool(servo_id), servo_id)
    if servo_id:
        js("POSE_PARTS.setAngle('%s',45)" % servo_id['servo'])
        js("POSE_EDIT_DEBUG.select('%s')" % servo_id['child'])
        br.pump(0.5)
        check('舵机真的转到 45 了（对照，防止空转）',
              js("POSE_PARTS.angleOf('%s')" % servo_id['servo']) == 45)

        def world_of(pid):
            for x in js('POSE_DEBUG.snapshot()'):
                if x['id'] == pid:
                    return x['world']
            return None

        p0, l0 = world_of(servo_id['child']), js("POSE_PARTS.byId('%s').pos.slice()"
                                                % servo_id['child'])
        # 沿**世界 X 轴的屏幕方向**拖：拿轴两端点的屏幕差当方向
        a0 = js("POSE_EDIT_DEBUG.axisPoint('x', 0.0)")
        a5 = js("POSE_EDIT_DEBUG.axisPoint('x', 0.5)")
        ux, uy = a5['x'] - a0['x'], a5['y'] - a0['y']
        n = (ux * ux + uy * uy) ** 0.5
        grab = js("POSE_EDIT_DEBUG.axisPoint('x', 0.3)")
        mouse('mousePressed', grab['x'], grab['y'], 0)
        br.pump(0.15)
        check('抓到的是 X 箭头（不是别的轴、也不是没抓上）',
              js('POSE_EDIT_DEBUG.gizmo()["axis"]') == 'x'
              if js('POSE_EDIT_DEBUG.gizmo()') else False,
              js('POSE_EDIT_DEBUG.gizmo()'))
        for k in range(1, 9):
            mouse('mouseMoved', grab['x'] + ux / n * 6 * k, grab['y'] + uy / n * 6 * k, 1)
            br.pump(0.05)
        mouse('mouseReleased', grab['x'] + ux / n * 48, grab['y'] + uy / n * 48, 0)
        br.pump(0.4)
        p1 = world_of(servo_id['child'])
        l1 = js("POSE_PARTS.byId('%s').pos.slice()" % servo_id['child'])
        dw = [p1[i] - p0[i] for i in range(3)]
        dl = [l1[i] - l0[i] for i in range(3)]
        moved = abs(dw[0]) + abs(dw[2]) > 0.05
        check('沿 X 拖之后元件确实动了（否则下面两条是空断言）', moved, '%s → %s' % (p0, p1))
        check('世界落点确实沿着**世界 X** 走（y/z 分量可忽略）',
              abs(dw[0]) > 5 * max(abs(dw[1]), abs(dw[2])), '世界增量 = %s' % dw)
        # 局部增量方向必须被父件的旋转转过 —— 等于世界增量就说明换算压根没做
        check('写回的局部增量被父件转过（不是把世界坐标直接当 pos 用）',
              max(abs(dl[0] - dw[0]), abs(dl[2] - dw[2])) > 0.05,
              '局部增量 = %s / 世界增量 = %s' % (dl, dw))
        check('写回的局部坐标与"世界→局部"换算的结果一致',
              (lambda v: v and max(abs(v[i] - l1[i]) for i in range(3)) < 1e-6)(
                  js("POSE_EDIT_DEBUG.worldToLocal('%s',%s)" % (servo_id['child'], p1))),
              l1)
        check('局部坐标没有飞出 ±8 框',
              all(abs(v) <= 8 for v in l1), l1)
        js("POSE_PARTS.setAngle('%s',90)" % servo_id['servo'])   # 还原，别影响后面的用例
        br.pump(0.3)

    print('\n=== 9. 轴正对视线（退化）时拖拽不许把元件甩飞 ===')
    # 这一条盯的是 alongAxis 里 denom = 1-cos²θ 那个除法。θ 小到 8° 以内时
    # 两直线的最近点解开始发散，除出来的 s 可以瞬间把元件扔到 ±8 外面 ——
    # **而且一句报错都没有**，是最容易漏掉的一条路径。
    #
    # 怎么把相机真的转进退化锥：OrbitControls 会把极角夹在 [EPS, π-EPS]，
    # 所以往一个方向**拖到底**就是"相机正压在 Y 轴上往下看"，θ 落在 1e-5 度量级。
    # 这是夹出来的端点，每次都一样，不用靠运气。
    #
    # ⚠️ 关键的一条：**鼠标不能挪远**。θ 说的是 Y 轴与"那条射线的夹角"，
    #    不是与视轴 —— 光标离开画面中心约 70px，射线就偏出 8° 了，
    #    这时候虽然相机还压在 Y 轴上，这一帧却已经**不算退化**（实测：
    #    挪 64px 时元件确实动了 0.05 个单位）。所以下面只挪 12px，
    #    并且**先断言相机真的进了锥**，免得"拖了个空"被当成"保护生效"。
    cv = js("(function(){var b=document.querySelector('#pose-stage canvas').getBoundingClientRect();"
            "return {x:b.left,y:b.top,w:b.width,h:b.height,cy:b.top+b.height/2}})()")
    check('退化用例拿到了画布（拿不到就直接跳过等于没测，所以这条也要断言）',
          bool(cv and cv.get('w')), cv)
    if cv and cv.get('w'):
        js("POSE_EDIT_DEBUG.select('p8')")
        br.pump(0.4)
        drag(cv['x'] + 12, cv['cy'], 0, 120, steps=8)      # 贴着画布左缘拖，一定不会误抓手柄
        ang = js("({y:POSE_EDIT_DEBUG.axisAngleDeg('y'), x:POSE_EDIT_DEBUG.axisAngleDeg('x')})")
        check('视角真的转进了退化锥（Y 轴与视线夹角 < 8.13°）',
              ang and ang['y'] is not None and ang['y'] < 8.13,
              '%s（x 轴做对照：应该还是接近 90°）' % ang)
        check('同时 X 轴还远没退化（这个相机位姿确实只卡住了一根轴）',
              ang and ang['x'] > 80, ang)

        before = [x for x in js('POSE_DEBUG.snapshot()') if x['id'] == 'p8'][0]['world']
        aim = js("POSE_EDIT_DEBUG.axisPoint('y', 0.05)")
        mouse('mousePressed', aim['x'], aim['y'], 0)
        br.pump(0.15)
        grabbed = js('POSE_EDIT_DEBUG.dragging()')
        check('退化视角下仍然抓得到 Y 箭头（不是"没抓上所以没动"）', grabbed is True, grabbed)
        for k in range(1, 5):                              # 只挪 12px，留在锥里
            mouse('mouseMoved', aim['x'] + k * 3, aim['y'] + k * 3, 1)
            br.pump(0.05)
        during = [x for x in js('POSE_DEBUG.snapshot()') if x['id'] == 'p8'][0]['world']
        mouse('mouseReleased', aim['x'] + 12, aim['y'] + 12, 0)
        br.pump(0.3)
        after = [x for x in js('POSE_DEBUG.snapshot()') if x['id'] == 'p8'][0]['world']
        check('退化时拖拽：位置**逐位不变**（除零没被算进去）',
              during == before and after == before, '%s → %s' % (before, after))
        # ⚠️ 别写成 isinstance(v, float)：JSON 里 0 回来是 int，会误判成 NaN
        check('退化时拖拽：没有 NaN / Infinity',
              all(isinstance(v, (int, float)) and v == v and abs(v) != float('inf')
                  for v in during + after), during + after)
        check('退化时拖拽：没有飞出 ±8',
              all(abs(v) <= 8 for v in after), after)
        check('退化时松手：拖拽状态收干净了',
              js('POSE_EDIT_DEBUG.dragging()') is False)
        check('退化时松手：OrbitControls 回到可用（否则视角永远转不动）',
              js('POSE_EDIT_DEBUG.controlsEnabled()') is True)

    print('\n=== 10. 静态资源 MIME（本轮新增的两个文件）===')
    s2 = requests.Session()
    s2.post(BASE + '/login', data={'token': token()}, allow_redirects=False)
    for path in ('/static/pose-gizmo.js', '/static/pose-layout.js',
                 '/static/pose-parts.js', '/static/style.css'):
        r = s2.get(BASE + path)
        want_type = 'css' if path.endswith('.css') else 'javascript'
        check('%s MIME 是 %s' % (path, want_type),
              r.status_code == 200 and want_type in r.headers.get('Content-Type', ''),
              '%s %s' % (r.status_code, r.headers.get('Content-Type')))

    errs = [e for e in br.console_errors() if 'favicon' not in e]
    check('全程没有控制台报错（favicon 404 是既有的，不算）', not errs, errs)

    br.screenshot(sid, os.path.join(TMP, 'D1_after_drag.png'))
    br.close_page(tid)

finally:
    if edge:
        edge.terminate()
    if flask:
        flask.terminate()
    time.sleep(1.5)
    shutil.rmtree(PROFILE, ignore_errors=True)

print('\n================= 汇总 =================')
ok = sum(1 for _, o, _ in RESULTS if o)
print('共 %d 项，通过 %d，失败 %d' % (len(RESULTS), ok, len(RESULTS) - ok))
for n, o, d in RESULTS:
    if not o:
        print('  未过：' + n + '  -> ' + str(d))
print('清场完成（只 kill 自己起的；9222 上用户 webctl 的两个没碰）')
sys.exit(0 if ok == len(RESULTS) else 1)
