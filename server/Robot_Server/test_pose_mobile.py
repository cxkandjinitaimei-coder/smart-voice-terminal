# -*- coding: utf-8 -*-
"""姿态页第四期验收：铺满整屏（⛶）+ 平板浮层 + 触摸阈值。

跑法：  cd d:\\ArduinoIDE\\ArdProject\\flask_relay
        D:\\Python\\python.exe test_pose_mobile.py
        （**必须用 D:\\Python\\python.exe** —— PowerShell 会把无 BOM 的 UTF-8 按 GBK 读）

为什么单独一个文件、单独一个端口：
  设备模拟（视口 / 触摸 / 媒体）**必须在 Page.navigate 之前**下，
  而 test_pose_page.py 的 open_pose() 没有那个插入点，硬塞进去会污染两套既有
  suite 的前提。所以这里自己开一个浏览器（端口 9355、独立 profile），
  Flask 5000 仍然复用。

★ 一个实测出来的关键事实（和最初的设计假设相反，必须记下来）：
    让 (pointer: coarse) 变真的**唯一**开关是 `Emulation.setTouchEmulationEnabled`。
    - `Emulation.setEmulatedMedia` 里的 pointer / hover **被 CDP 静默忽略**
      （不在它支持的 feature 列表里），matchMedia 一点不变；
    - `Emulation.setDeviceMetricsOverride(mobile=True)` 也不管用。
  所以下面第一段是**硬前置**：模拟没生效就直接红在明处，
  绝不让后面二十来条用错误前提"假通过"。

⚠️ 不开启 `Emulation.setEmitTouchEventsForMouse` —— 它会把鼠标事件也转成触摸，
   那样"触摸档 / 鼠标档"的对照断言会被自己搅浑。
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import requests                                                    # noqa: E402
from cdp_min import Browser, WsError, wait_port                    # noqa: E402
# ★ test_pose_page.py 可以安全 import（逻辑全在函数里，入口有 if __name__）；
#   test_pose_drag.py **绝对不能** —— 它在模块顶层就把整套跑完了。
from test_pose_page import close, login, ready_pose, start_flask, token   # noqa: E402

APP = os.path.dirname(os.path.abspath(__file__))
PY = r'D:\Python\python.exe'
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
PORT = 9355
BASE = 'http://127.0.0.1:5000'
TMP = os.path.join(tempfile.gettempdir(), 'pose_verify')
os.makedirs(TMP, exist_ok=True)
PROFILE = os.path.join(TMP, 'edge_mobile_profile')

RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok), detail))
    print(('  [OK]   ' if ok else '  [FAIL] ') + name + (('  -> ' + str(detail)) if detail else ''))
    return ok


def start_edge(extra=None, port=PORT, profile=PROFILE):
    """本地复制一份，不复用 test_pose_page.start_edge ——
    那个函数内部用的是**它的模块级 PROFILE 全局**，import 过来调用会去删它的目录。"""
    if os.path.isdir(profile):
        shutil.rmtree(profile, ignore_errors=True)
    args = [EDGE, '--headless=new', '--remote-debugging-port=%d' % port,
            '--user-data-dir=' + profile,
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
    for _ in range(40):
        try:
            return p, Browser(port)
        except Exception:
            time.sleep(0.5)
    raise SystemExit('连不上 Edge 的 CDP')


def emulate(br, sid, w, h, dpr=2.0, touch=True, mobile=True):
    """把这一页装成一台平板。★ 必须在 Page.navigate 之前调。"""
    br.call('Emulation.setDeviceMetricsOverride', {
        'width': w, 'height': h, 'deviceScaleFactor': dpr, 'mobile': mobile,
        'screenWidth': w, 'screenHeight': h,
        'screenOrientation': {'type': 'portraitPrimary' if h >= w else 'landscapePrimary',
                              'angle': 0 if h >= w else 90},
    }, session=sid)
    if touch:
        br.call('Emulation.setTouchEmulationEnabled',
                {'enabled': True, 'maxTouchPoints': 5}, session=sid)
    else:
        # ★ 显式关掉，不能"不设"了事 —— 这个开关在同一个 session 里是粘的，
        #   不关的话下面测"宽屏 + 细指针"时 coarse 还是 true，模式判定会一直卡在 narrow。
        br.call('Emulation.setTouchEmulationEnabled', {'enabled': False}, session=sid)


def open_page(br, cookie, w=768, h=1024, touch=True):
    sid, tid = br.new_page('about:blank')
    for m in ('Page.enable', 'Runtime.enable', 'Log.enable', 'Network.enable'):
        br.call(m, {}, session=sid)
    br.call('Network.setCookie', {'name': 'session', 'value': cookie,
                                  'domain': '127.0.0.1', 'path': '/',
                                  'httpOnly': True}, session=sid)
    emulate(br, sid, w, h, touch=touch)          # ← 必须在导航之前
    br.call('Page.navigate', {'url': BASE + '/#pose'}, session=sid)
    for _ in range(120):
        if any(m.get('method') == 'Page.loadEventFired' for m in br.events):
            break
        br.pump(0.25)
    br.pump(1.2)
    return sid, tid


def js(br, sid, expr):
    return br.evaluate(expr, sid)


def rect(br, sid, sel):
    return js(br, sid, """(() => { const e = document.querySelector(%r);
      if (!e) return null; const b = e.getBoundingClientRect();
      return {x: Math.round(b.left*100)/100, y: Math.round(b.top*100)/100,
              w: Math.round(b.width*100)/100, h: Math.round(b.height*100)/100}; })()""" % sel)


def mouse(br, sid, t, x, y, buttons=0):
    br.call('Input.dispatchMouseEvent',
            {'type': t, 'x': x, 'y': y, 'button': 'left', 'buttons': buttons,
             'clickCount': 1 if t != 'mouseMoved' else 0}, session=sid)


def real_click(br, sid, sel):
    b = rect(br, sid, sel)
    if not b:
        return False
    x, y = b['x'] + b['w'] / 2, b['y'] + b['h'] / 2
    mouse(br, sid, 'mousePressed', x, y, 1)
    br.pump(0.05)
    mouse(br, sid, 'mouseReleased', x, y, 0)
    br.pump(0.45)
    return True


def touch_tap(br, sid, x, y):
    br.call('Input.dispatchTouchEvent',
            {'type': 'touchStart', 'touchPoints': [{'x': x, 'y': y, 'id': 0}]}, session=sid)
    br.pump(0.06)
    br.call('Input.dispatchTouchEvent', {'type': 'touchEnd', 'touchPoints': []}, session=sid)
    br.pump(0.45)


def touch_swipe(br, sid, x, y, dx, dy, steps=8):
    """按下 → 分多步划走 → 抬起。每步之间留时间，模拟真实手指的连续移动。"""
    br.call('Input.dispatchTouchEvent',
            {'type': 'touchStart', 'touchPoints': [{'x': x, 'y': y, 'id': 0}]}, session=sid)
    br.pump(0.05)
    for i in range(1, steps + 1):
        br.call('Input.dispatchTouchEvent', {
            'type': 'touchMove',
            'touchPoints': [{'x': x + dx * i / steps, 'y': y + dy * i / steps, 'id': 0}]},
            session=sid)
        br.pump(0.03)
    br.call('Input.dispatchTouchEvent', {'type': 'touchEnd', 'touchPoints': []}, session=sid)
    br.pump(0.5)


def esc(br, sid):
    for t in ('keyDown', 'keyUp'):
        br.call('Input.dispatchKeyEvent', {
            'type': t, 'key': 'Escape', 'code': 'Escape',
            'windowsVirtualKeyCode': 27, 'nativeVirtualKeyCode': 27}, session=sid)
    br.pump(0.6)


def main():
    flask = None
    edge = None
    try:
        flask = start_flask()
        ck = login()
        edge, br = start_edge()
        sid, tid = open_page(br, ck)

        # ================= 0. 硬前置：模拟真的生效了吗 =================
        print('\n=== 0. 前置：设备模拟（这节红了后面全部没有意义）===')
        probe = js(br, sid, """(() => {
          function m(q){ try { return matchMedia(q).matches; } catch(e){ return 'ERR'; } }
          return {coarse: m('(pointer: coarse)'), fine: m('(pointer: fine)'),
                  w: innerWidth, h: innerHeight, dpr: devicePixelRatio,
                  maxTP: navigator.maxTouchPoints};
        })()""")
        check('★ 模拟出了粗指针（模式判定全靠它；靠 setTouchEmulationEnabled）',
              probe['coarse'] is True, probe)
        check('★ 同时细指针为假（真的像平板，不是"带触摸屏的笔记本"）',
              probe['fine'] is False, probe)
        check('模拟视口宽度 = 768（说明页面里的 viewport meta 生效了）',
              probe['w'] == 768, probe)
        check('模拟 DPR = 2', probe['dpr'] == 2, probe)
        modes = js(br, sid, "({mode: document.documentElement.dataset.poseMode, "
                            "attr: document.querySelector('#pose-workbench').dataset.mode})")
        check('★ 判定结果确实进了 narrow', modes['mode'] == 'narrow' and modes['attr'] == 'narrow', modes)

        # ================= 1. 窄屏浮层 =================
        print('\n=== 1. 平板竖屏：画布占满 + 抽屉浮层 ===')
        st = rect(br, sid, '#pose-stage')
        check('★ 画布宽 > 600（桌面停靠三栏时它只有约 330，竖屏更会掉到 122）',
              st and st['w'] > 600, st)
        check('★ 页面没有横向溢出（浮层没有把文档撑宽）',
              js(br, sid, 'document.documentElement.scrollWidth') <= probe['w'],
              js(br, sid, 'document.documentElement.scrollWidth'))
        check('分隔条在窄屏被藏掉了（宽度由 CSS 的 min(86%,420px) 接管）',
              rect(br, sid, '.pose-split[data-split="left"]')['w'] == 0,
              rect(br, sid, '.pose-split[data-split="left"]'))
        check('两个抽屉默认都是收起的（否则一进页面就被浮层糊住画布）',
              rect(br, sid, '#drawer-parts')['w'] == 0 and rect(br, sid, '#drawer-axes')['w'] == 0,
              (rect(br, sid, '#drawer-parts'), rect(br, sid, '#drawer-axes')))
        check('两个开合按钮露出来了（宽屏下它们是 hidden）',
              js(br, sid, "[...document.querySelectorAll('.pose-fab[data-drawer-fab]')]"
                          ".every(b => !b.hidden)") is True)
        check('⛶ 按钮在画布卡里、点得到',
              js(br, sid, """(() => { const b = document.getElementById('pose-full');
                const r = b.getBoundingClientRect();
                const hit = document.elementFromPoint(r.left + r.width/2, r.top + r.height/2);
                return !!hit && (hit === b || b.contains(hit)); })()""") is True)

        before = rect(br, sid, '#pose-stage')
        check('点 🧩 能打开左抽屉', real_click(br, sid, '.pose-fab[data-drawer-fab="left"]'))
        after = rect(br, sid, '#pose-stage')
        check('★ 浮层开合时画布的位置和宽度**逐像素不变**（这是"浮层"与"挤压"的唯一分界线）',
              before == after, '%s -> %s' % (before, after))
        dw = rect(br, sid, '#drawer-parts')
        check('左抽屉确实当浮层展开了（有真实宽度）', dw and dw['w'] > 200, dw)
        check('★ 浮层确实画在画布**上面**（画布中心的命中元素在抽屉里）',
              js(br, sid, """(() => { const s = document.getElementById('pose-stage');
                const r = s.getBoundingClientRect();
                const hit = document.elementFromPoint(r.left + r.width/2, r.top + r.height/2);
                return !!(hit && hit.closest('#drawer-parts')); })()""") is True)
        check('遮罩露出来了', js(br, sid, "!document.getElementById('pose-mask').hidden") is True)

        # 点遮罩（画布中心偏右，避开浮层）→ 收起
        wb = rect(br, sid, '#pose-workbench')
        touch_tap(br, sid, wb['x'] + wb['w'] * 0.9, wb['y'] + wb['h'] * 0.5)
        check('★ 点空白（遮罩）→ 抽屉收起来了（证明遮罩是真元素、能命中）',
              rect(br, sid, '#drawer-parts')['w'] == 0, rect(br, sid, '#drawer-parts'))
        check('遮罩自己也藏回去了', js(br, sid, "document.getElementById('pose-mask').hidden") is True)

        # 开 🎛 时 🧩 自动关（不叠两层）
        real_click(br, sid, '.pose-fab[data-drawer-fab="left"]')
        real_click(br, sid, '.pose-fab[data-drawer-fab="right"]')
        check('开 🎛 时 🧩 自动关掉（不叠两层浮层）',
              rect(br, sid, '#drawer-parts')['w'] == 0
              and rect(br, sid, '#drawer-axes')['w'] > 200,
              (rect(br, sid, '#drawer-parts'), rect(br, sid, '#drawer-axes')))

        # 浮层里点元件触发的 scrollIntoView 不许把整页拖走
        # ★ 必须先把**左**抽屉开出来再选：清单（.part-row）在左抽屉里，
        #   抽屉关着时它是 display:none，scrollIntoView 本来就是空操作 ——
        #   那样这条断言会因为"根本没滚"而白通过，测不到任何东西。
        real_click(br, sid, '.pose-fab[data-drawer-fab="left"]')
        check('（前提）左抽屉开着，清单可见（否则下面那条是空断言）',
              (rect(br, sid, '#drawer-parts') or {}).get('w', 0) > 200,
              rect(br, sid, '#drawer-parts'))
        js(br, sid, "POSE_EDIT_DEBUG.select('p4')")
        br.pump(0.5)
        check('★ 浮层打开时点元件（内部会 scrollIntoView）→ 整页纹丝不动',
              js(br, sid, 'window.scrollY') == 0
              and js(br, sid, 'document.documentElement.scrollWidth') <= probe['w'],
              (js(br, sid, 'window.scrollY'),
               js(br, sid, 'document.documentElement.scrollWidth')))
        touch_tap(br, sid, wb['x'] + wb['w'] * 0.9, wb['y'] + wb['h'] * 0.5)

        # ★ 粗指针单独就能定窄屏：把视口拉到 1280（宽度断点已经不触发）后仍该是 narrow
        emulate(br, sid, 1280, 800)
        br.pump(0.6)
        check('★ 视口拉到 1280 后仍是 narrow（证明 (pointer: coarse) 才是那条真正起作用的判据，'
              '光靠宽度断点救不了平板横屏）',
              js(br, sid, "document.documentElement.dataset.poseMode") == 'narrow',
              js(br, sid, "document.documentElement.dataset.poseMode"))
        emulate(br, sid, 768, 1024)
        br.pump(0.6)
        check('转回竖屏仍是 narrow', js(br, sid, "document.documentElement.dataset.poseMode") == 'narrow')

        # ================= 2. 尺寸收敛 =================
        print('\n=== 2. 尺寸收敛：夹紧的是有效值，不是用户意图 ===')
        js(br, sid, "localStorage.setItem('pose_layout_v1', JSON.stringify("
                    "{v:1,leftW:560,rightW:500,stageH:900,leftOpen:true,rightOpen:true}))")
        br.events.clear()
        br.call('Page.reload', {}, session=sid)
        for _ in range(120):
            if any(m.get('method') == 'Page.loadEventFired' for m in br.events):
                break
            br.pump(0.25)
        br.pump(1.5)
        check('灌进 PC 存档后：意图原封不动（get 仍是 560 / 900）',
              js(br, sid, "POSE_LAYOUT.get().leftW") == 560
              and js(br, sid, "POSE_LAYOUT.get().stageH") == 900,
              js(br, sid, "POSE_LAYOUT.get()"))
        # ★ 窄屏下宽度**根本不进数学**：浮层宽度由 CSS 的 min(86%,420px) 给，
        #   --dw-* 写成什么都不挤压画布。所以这里该断言的是"原样透传"，
        #   而不是"被夹小" —— 真正要证明的是"560 没把画布挤扁"，也就是下一条的 stage 宽 > 600。
        #   （"夹紧只夹有效值"在真正的宽屏下测，见 2b。）
        check('★ 窄屏下宽度意图原样透传、不做宽度夹紧（浮层模式的设计如此）',
              js(br, sid, "POSE_LAYOUT.eff().leftW") == 560,
              js(br, sid, "POSE_LAYOUT.eff()"))
        st2 = rect(br, sid, '#pose-stage')
        check('★ 灌了超宽存档，画布照样铺满（没被那 560+500 挤扁）',
              st2 and st2['w'] > 600, st2)
        check('灌了超宽存档也不横向溢出',
              js(br, sid, 'document.documentElement.scrollWidth') <= 768,
              js(br, sid, 'document.documentElement.scrollWidth'))
        ih = js(br, sid, 'innerHeight')
        want_h = round(max(280, min(900, ih * 0.56)))
        got_h = js(br, sid, "document.getElementById('pose-stage').getBoundingClientRect().height")
        check('★ 窄屏画布高度 = 0.56 × innerHeight（夹在 280~900 内）',
              abs(got_h - want_h) <= 2, '得到 %s，期望 %s（innerHeight=%s）' % (got_h, want_h, ih))
        check('★ 算高度**没有**顺手把 stageH 写回存档（存的还是 900）',
              js(br, sid, "POSE_LAYOUT.get().stageH") == 900,
              js(br, sid, "POSE_LAYOUT.get().stageH"))

        snap0 = js(br, sid, "POSE_LAYOUT.get()")
        for _ in range(3):
            emulate(br, sid, 1024, 768)
            br.pump(0.45)
            emulate(br, sid, 768, 1024)
            br.pump(0.45)
        time.sleep(0.4)
        h1 = js(br, sid, "document.getElementById('pose-stage').getBoundingClientRect().height")
        time.sleep(0.35)
        h2 = js(br, sid, "document.getElementById('pose-stage').getBoundingClientRect().height")
        check('★ 反复转屏后尺寸**定住了**（ResizeObserver ↔ refit 没有打架/来回抖）',
              h1 == h2, '%s vs %s' % (h1, h2))
        check('★ 反复转屏也没有污染用户意图（get 三次完全相同）',
              js(br, sid, "POSE_LAYOUT.get()") == snap0, js(br, sid, "POSE_LAYOUT.get()"))

        # ================= 2b. 宽屏：夹紧只夹有效值 =================
        # ★ 这一段同时干两件事：
        #   ① 把"夹紧"这件事放到它真正会发生的场景里去测（窄屏压根不做宽度夹紧）；
        #   ② 拿到**细指针档**的控件尺寸，下面第四段靠它做对照 ——
        #      直接比"粗档 > 细档"是证明 @media (pointer:coarse) 生效的最干净的办法，
        #      不用赌"≥40px"这种拍脑袋的绝对阈值。
        print('\n=== 2b. 宽屏（真桌面）：夹紧只夹有效值 + 抓手对照 ===')
        emulate(br, sid, 1440, 900, dpr=1.0, touch=False, mobile=False)
        br.pump(0.9)
        check('★ 关掉触摸模拟后细指针回来了（证明那个开关是可逆的、不留脏状态；'
              '也反证了模式判定确实由它驱动）',
              js(br, sid, "matchMedia('(pointer: fine)').matches") is True
              and js(br, sid, "matchMedia('(pointer: coarse)').matches") is False,
              js(br, sid, "({f:matchMedia('(pointer: fine)').matches,"
                          "c:matchMedia('(pointer: coarse)').matches})"))
        check('★ 宽屏下模式回到 wide', js(br, sid, "document.documentElement.dataset.poseMode") == 'wide',
              js(br, sid, "document.documentElement.dataset.poseMode"))
        check('★ 宽屏 1440 下 560 真的被夹：意图仍 560、有效值 < 560'
              '（一条同时证伪"不夹紧→溢出"和"改存档→意图丢失"）',
              js(br, sid, "POSE_LAYOUT.get().leftW") == 560
              and js(br, sid, "POSE_LAYOUT.eff().leftW") < 560,
              (js(br, sid, "POSE_LAYOUT.get()"), js(br, sid, "POSE_LAYOUT.eff()")))
        check('夹过之后不横向溢出', js(br, sid, 'document.documentElement.scrollWidth') <= 1440,
              js(br, sid, 'document.documentElement.scrollWidth'))
        check('★ 页面上真的按夹过的值渲染（不是只在内存里算了一下）',
              abs(rect(br, sid, '#drawer-parts')['w'] - js(br, sid, 'POSE_LAYOUT.eff().leftW')) <= 1,
              (rect(br, sid, '#drawer-parts')['w'], js(br, sid, 'POSE_LAYOUT.eff().leftW')))
        check('★ 夹紧之后画布仍然够大（夹紧不是为了把画布饿死）',
              rect(br, sid, '#pose-stage')['w'] >= 240, rect(br, sid, '#pose-stage'))

        FINE_BTN = js(br, sid, """(() => { const b = document.getElementById('pose-send');
          return b ? Math.round(b.getBoundingClientRect().height * 100) / 100 : null; })()""")
        FINE_RNG = js(br, sid, """(() => { const r = document.querySelector('.pose-axis input[type=range]');
          return r ? Math.round(r.getBoundingClientRect().height * 100) / 100 : null; })()""")
        check('（采集）细指针档的控件尺寸拿到了（宽屏下抽屉是停靠的，量得到）',
              FINE_BTN and FINE_RNG, (FINE_BTN, FINE_RNG))

        grip = rect(br, sid, '#pose-grip')
        check('★ 宽屏下"拖高度"的把手可见（对照：全屏下它必须藏起来）',
              js(br, sid, "document.getElementById('pose-grip').offsetParent !== null") is True
              and grip and grip['w'] > 0, grip)
        sh_before = js(br, sid, "POSE_LAYOUT.get().stageH")
        real_click(br, sid, '#pose-full')
        br.pump(0.6)
        check('★ 宽屏全屏：抓手也不可见（拖了没效果的把手等于撒谎）',
              js(br, sid, "document.getElementById('pose-grip').offsetParent === null") is True)
        if grip:
            gx, gy = grip['x'] + grip['w'] / 2, grip['y'] + grip['h'] / 2
            mouse(br, sid, 'mousePressed', gx, gy, 1)
            br.pump(0.05)
            mouse(br, sid, 'mouseReleased', gx, gy, 0)
            br.pump(0.45)
        check('★ 全屏下点抓手原来的位置 → stageH 一点都没变（不是"藏起来了但还在吃事件"）',
              js(br, sid, "POSE_LAYOUT.get().stageH") == sh_before,
              '%s vs %s' % (js(br, sid, "POSE_LAYOUT.get().stageH"), sh_before))
        real_click(br, sid, '#pose-full')
        br.pump(0.6)
        check('★ 退出全屏（宽屏）：把手回来了',
              js(br, sid, "document.getElementById('pose-grip').offsetParent !== null") is True)
        check('★ 宽屏进出全屏后 stageH 也没被写脏',
              js(br, sid, "POSE_LAYOUT.get().stageH") == sh_before,
              js(br, sid, "POSE_LAYOUT.get().stageH"))

        emulate(br, sid, 768, 1024)
        br.pump(0.9)
        check('★ 转回平板竖屏后又是 narrow（模式判定是可逆的，不是一次性卡死）',
              js(br, sid, "document.documentElement.dataset.poseMode") == 'narrow',
              js(br, sid, "document.documentElement.dataset.poseMode"))

        # ================= 3. 铺满整屏 =================
        print('\n=== 3. 铺满整屏（⛶）===')
        raw0 = js(br, sid, "localStorage.getItem('pose_layout_v1')")
        sh0 = js(br, sid, "(getComputedStyle(document.getElementById('pose-workbench'))"
                          ".getPropertyValue('--stage-h') || '').trim()")
        st_h0 = rect(br, sid, '#pose-stage')['h']
        real_click(br, sid, '#pose-full')
        br.pump(0.6)
        check('进了全屏态', js(br, sid, "document.querySelector('#pose-workbench').dataset.fullscreen") == '1')
        wbr = rect(br, sid, '#pose-workbench')
        vw, vh = js(br, sid, 'innerWidth'), js(br, sid, 'innerHeight')
        check('★ 工作台铺满整个视口（fixed inset:0 绕过了 main 的 max-width:1080）',
              abs(wbr['x']) < 1 and abs(wbr['y']) < 1
              and abs(wbr['w'] - vw) < 1 and abs(wbr['h'] - vh) < 1,
              '%s vs 视口 %sx%s' % (wbr, vw, vh))
        check('★ 全屏下画布高度 > 0.8 倍视口高（桌面原来固定 440，现在撑满了）',
              rect(br, sid, '#pose-stage')['h'] > vh * 0.8, rect(br, sid, '#pose-stage'))
        check('★ 全屏下 canvas 真的量到了非零尺寸（不是"容器 0 高 → 一片黑"）',
              js(br, sid, """(() => { const c = document.querySelector('#pose-stage canvas');
                return !!c && c.clientWidth > 0 && c.clientHeight > 0; })()""") is True,
              js(br, sid, """(() => { const c = document.querySelector('#pose-stage canvas');
                return c ? [c.clientWidth, c.clientHeight] : null; })()"""))
        check('全屏下 3D 还是活的（没有卡在 ready=false）',
              js(br, sid, 'window.POSE_DEBUG.state().ready') is True,
              js(br, sid, 'window.POSE_DEBUG.state()'))
        check('★ 全屏下那个"拖高度"的把手藏起来了（拖了也没效果的把手等于撒谎）',
              js(br, sid, "document.getElementById('pose-grip').offsetParent === null") is True)
        check('全屏下滚动被锁住了', js(br, sid,
              "getComputedStyle(document.documentElement).overflow === 'hidden'") is True)
        check('全屏时开了页面不会横向溢出', js(br, sid, 'document.documentElement.scrollWidth') <= vw,
              js(br, sid, 'document.documentElement.scrollWidth'))

        real_click(br, sid, '#pose-full')
        br.pump(0.6)
        check('★ 退出全屏：data-fullscreen 摘掉了',
              js(br, sid, "document.querySelector('#pose-workbench').dataset.fullscreen") is None)
        check('★ 退出全屏：滚动锁也摘掉了',
              js(br, sid, "!document.documentElement.classList.contains('is-pose-fs')") is True)
        raw1 = js(br, sid, "localStorage.getItem('pose_layout_v1')")
        check('★ 进出全屏一次，存档**逐字节不变**（全屏不持久化）', raw0 == raw1,
              '%r vs %r' % (raw0, raw1))
        sh1 = js(br, sid, "(getComputedStyle(document.getElementById('pose-workbench'))"
                          ".getPropertyValue('--stage-h') || '').trim()")
        check('★ 退出全屏：--stage-h 回到原值（全屏态没有写脏它）', sh0 == sh1,
              '%r vs %r' % (sh0, sh1))
        check('★ 退出全屏：画布高度也回到原值', abs(rect(br, sid, '#pose-stage')['h'] - st_h0) < 1,
              '%s vs %s' % (rect(br, sid, '#pose-stage')['h'], st_h0))
        # ⚠️ 这里**不能**断言"抓手回来了"：窄屏下 .pose-grip 本来就被 display:none 掉
        #   （浮层模式下没有停靠布局可拖），那股"隐"是 narrow 造成的、不是全屏造成的。
        #   要隔离这个因果，得在宽屏下测 —— 已挪到 2b 段。
        check('★ 退出全屏后，抓手的隐藏原因只剩 narrow（宽屏下会回来，见 2b）',
              js(br, sid, "document.documentElement.dataset.poseMode") == 'narrow'
              and js(br, sid, "document.getElementById('pose-grip').offsetParent === null") is True,
              js(br, sid, "document.documentElement.dataset.poseMode"))

        # Esc 退出（真全屏没进成时靠 JS 那条 keydown 兜底）
        real_click(br, sid, '#pose-full')
        br.pump(0.5)
        esc(br, sid)
        check('★ 按 Esc 也能退出全屏（真全屏被拒时靠这条兜底，否则只能刷新页面）',
              js(br, sid, "document.querySelector('#pose-workbench').dataset.fullscreen") is None
              and js(br, sid, "!document.documentElement.classList.contains('is-pose-fs')") is True,
              js(br, sid, "document.querySelector('#pose-workbench').dataset.fullscreen"))

        # ================= 4. 触摸 =================
        print('\n=== 4. 触摸：阈值分档 + 命中区 ===')
        # ★ 不硬编码"点中的是哪个元件"：p7/p8/p9 在世界坐标里可能重合，
        #   射线最后命中谁由几何决定 —— 问题日志·陷阱③ 已经在这上头栽过一次。
        #   做法改成：先用零位移点击**问出**这个落点真正命中谁，再拿它当基准。
        sp = js(br, sid, "POSE_EDIT_DEBUG.screenPos('p4')")
        check('拿到了元件在屏幕上的位置（坐标都没有就别谈点选了）', bool(sp), sp)

        def probe(px, py):
            """零位移鼠标点击 → (选中的 id, 按下时有没有起拖拽)。"""
            js(br, sid, "POSE_EDIT_DEBUG.select(null)")
            br.pump(0.25)
            mouse(br, sid, 'mousePressed', px, py, 1)
            br.pump(0.06)
            dragged = js(br, sid, 'POSE_EDIT_DEBUG.dragging()')
            mouse(br, sid, 'mouseReleased', px, py, 0)
            br.pump(0.4)
            return js(br, sid, 'POSE_EDIT_DEBUG.selected()'), dragged

        PID, OFF = None, None
        if sp:
            PID, dragged = probe(sp['x'], sp['y'])
            check('★ 落点确实打在元件上，且**没有**打在选中手柄的箭头上'
                  '（打中箭头的表现和"点了没反应"一模一样，必须先把这个可能性排掉）',
                  bool(PID) and dragged is False, (PID, dragged))
        if PID:
            for ox, oy in ((10, 0), (-10, 0), (0, 10), (0, -10)):
                g, d = probe(sp['x'] + ox, sp['y'] + oy)
                if g == PID and d is False:
                    OFF = (ox, oy)
                    break
            check('（前提）找到一个"飘 10px 后仍落在**同一个**元件上"的方向'
                  '（找不到就没法做下面那对分档对照）', OFF is not None, OFF)

        if PID and OFF:
            ox, oy = OFF
            # ① 触摸档：按下在中心、飘 10px 后抬起 → 放行（12px）
            js(br, sid, "POSE_EDIT_DEBUG.select(null)")
            br.pump(0.25)
            br.call('Input.dispatchTouchEvent', {
                'type': 'touchStart',
                'touchPoints': [{'x': sp['x'], 'y': sp['y'], 'id': 0}]}, session=sid)
            br.pump(0.06)
            for i in range(1, 6):
                br.call('Input.dispatchTouchEvent', {'type': 'touchMove', 'touchPoints': [
                    {'x': sp['x'] + ox * i / 5.0, 'y': sp['y'] + oy * i / 5.0, 'id': 0}]}, session=sid)
                br.pump(0.02)
            br.call('Input.dispatchTouchEvent', {'type': 'touchEnd', 'touchPoints': []}, session=sid)
            br.pump(0.5)
            check('★ 触摸：按下后手指飘了 10px 才抬起 → **照样算点中**（12px 档）',
                  js(br, sid, 'POSE_EDIT_DEBUG.selected()') == PID,
                  js(br, sid, 'POSE_EDIT_DEBUG.selected()'))

            # ② 鼠标档：同样的 10px 位移 → 拦住（4px）。这一对才是"分档真的生效了"
            js(br, sid, "POSE_EDIT_DEBUG.select(null)")
            br.pump(0.25)
            mouse(br, sid, 'mousePressed', sp['x'], sp['y'], 1)
            for i in range(1, 6):
                mouse(br, sid, 'mouseMoved', sp['x'] + ox * i / 5.0, sp['y'] + oy * i / 5.0, 1)
                br.pump(0.02)
            mouse(br, sid, 'mouseReleased', sp['x'] + ox, sp['y'] + oy, 0)
            br.pump(0.45)
            check('★ 鼠标：同样的 10px 位移 → **不算点中**（鼠标档仍是 4px，没被一起放大）',
                  js(br, sid, 'POSE_EDIT_DEBUG.selected()') is None,
                  js(br, sid, 'POSE_EDIT_DEBUG.selected()'))

            # ③ 手指划 30px 不许把选中改掉、也不许卡住转视角
            js(br, sid, "POSE_EDIT_DEBUG.select('%s')" % PID)
            br.pump(0.3)
            touch_swipe(br, sid, sp['x'], sp['y'], 30, 0)
            check('★ 手指划走 30px：选中项没被改掉',
                  js(br, sid, 'POSE_EDIT_DEBUG.selected()') == PID,
                  js(br, sid, 'POSE_EDIT_DEBUG.selected()'))
            check('★ 划完没留下拖拽残影、OrbitControls 回到可用（否则视角永远转不动）',
                  js(br, sid, 'POSE_EDIT_DEBUG.dragging()') is False
                  and js(br, sid, 'POSE_EDIT_DEBUG.controlsEnabled()') is True,
                  (js(br, sid, 'POSE_EDIT_DEBUG.dragging()'),
                   js(br, sid, 'POSE_EDIT_DEBUG.controlsEnabled()')))

        check('★ .pose-stage 上显式写了 touch-action:none（不再只靠 OrbitControls 的副作用）',
              js(br, sid, "getComputedStyle(document.getElementById('pose-stage')).touchAction")
              == 'none',
              js(br, sid, "getComputedStyle(document.getElementById('pose-stage')).touchAction"))

        # ★ 控件尺寸：**必须先把右抽屉开出来**。抽屉关着时是 display:none，
        #   量到的是 0 —— 第一次跑就是这么把"媒体查询生没生效"测成假红的。
        real_click(br, sid, '.pose-fab[data-drawer-fab="right"]')
        check('（前提）右抽屉开着，📤 下发与角度滑块可见（否则下面两条量到的是 0）',
              (rect(br, sid, '#drawer-axes') or {}).get('w', 0) > 200,
              rect(br, sid, '#drawer-axes'))
        CO_BTN = js(br, sid, """(() => { const b = document.getElementById('pose-send');
          return b ? Math.round(b.getBoundingClientRect().height * 100) / 100 : null; })()""")
        CO_RNG = js(br, sid, """(() => { const r = document.querySelector('.pose-axis input[type=range]');
          return r ? Math.round(r.getBoundingClientRect().height * 100) / 100 : null; })()""")
        check('（采集）粗指针档的控件尺寸拿到了', CO_BTN and CO_RNG, (CO_BTN, CO_RNG))
        # ★ 用"粗档 > 细档"而不是"粗档 ≥ 某个绝对像素值"：前者直接证明
        #   @media (pointer:coarse) 这段规则**参与进来了**，不赌字号/行高凑出来的数。
        check('★ @media (pointer: coarse) 确实生效：📤 下发按钮在粗指针下比细指针下高',
              CO_BTN and FINE_BTN and CO_BTN > FINE_BTN,
              '粗 %s vs 细 %s' % (CO_BTN, FINE_BTN))
        check('★ @media (pointer: coarse) 确实生效：角度滑块在粗指针下比细指针下高'
              '（调角度是平板的主要用途）',
              CO_RNG and FINE_RNG and CO_RNG > FINE_RNG,
              '粗 %s vs 细 %s' % (CO_RNG, FINE_RNG))

        # ================= 5. 地板 =================
        print('\n=== 5. 地板 ===')
        errs = [e for e in br.console_errors()
                if 'favicon' not in e.lower()]
        check('全程没有控制台报错（favicon 404 是既有的，不算）', not errs, errs[:4])

        print('\n=== 6. 无 WebGL 档也管用（浮层/全屏是布局层的事，不该依赖三维）===')
        br2 = None
        edge2 = None
        try:
            edge2, br2 = start_edge(extra=['--disable-3d-apis'], port=9366,
                                    profile=os.path.join(TMP, 'edge_mobile_nowebgl'))
            sid2, _ = open_page(br2, ck)
            st = js(br2, sid2, """(() => ({
              mode: document.documentElement.dataset.poseMode,
              layout: !!window.POSE_LAYOUT }))()""")
            check('★ 没有三维时也照样进了 narrow 模式（布局模块是纯 DOM、不依赖 Three）',
                  st['mode'] == 'narrow' and st['layout'] is True, st)
            b = rect(br2, sid2, '#pose-full')
            if b:
                mouse(br2, sid2, 'mousePressed', b['x'] + b['w'] / 2, b['y'] + b['h'] / 2, 1)
                br2.pump(0.05)
                mouse(br2, sid2, 'mouseReleased', b['x'] + b['w'] / 2, b['y'] + b['h'] / 2, 0)
                br2.pump(0.5)
            check('★ 没有三维时 ⛶ 照样能铺满整屏',
                  js(br2, sid2, "document.querySelector('#pose-workbench').dataset.fullscreen") == '1',
                  js(br2, sid2, "document.querySelector('#pose-workbench').dataset.fullscreen"))
            real_click(br2, sid2, '.pose-fab[data-drawer-fab="left"]')
            check('★ 没有三维时浮层抽屉也开得出来',
                  (rect(br2, sid2, '#drawer-parts') or {}).get('w', 0) > 200,
                  rect(br2, sid2, '#drawer-parts'))
        except Exception:
            # ★ 必须把它变成一条**红的断言**，而且把栈打出来：
            #   PowerShell 的 >> 只吃 stdout，异常栈原样落到控制台就消失了，
            #   表现成"这一节什么都没打印就跳去清场了"，非常难查（第一次跑就是这样）。
            #   所以这里既 print（进 stdout → 进日志），又把末行塞进 detail（进汇总）。
            tb = traceback.format_exc()
            print(tb)
            check('★ 禁 WebGL 档整段跑通（没抛异常）', False, tb.strip().splitlines()[-1])
        finally:
            close(edge2)

    finally:
        print('\n=== 清场 ===')
        close(edge)
        if flask:
            close(flask)
        time.sleep(0.6)
        shutil.rmtree(PROFILE, ignore_errors=True)
        shutil.rmtree(os.path.join(TMP, 'edge_mobile_nowebgl'), ignore_errors=True)

    print('\n================= 汇总 =================')
    bad = [r for r in RESULTS if not r[1]]
    print('共 %d 项，通过 %d，失败 %d' % (len(RESULTS), len(RESULTS) - len(bad), len(bad)))
    for n, _, d in bad:
        print('  FAIL  %s  %s' % (n, d))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
