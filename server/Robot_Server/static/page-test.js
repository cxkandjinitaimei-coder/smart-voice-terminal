// ============================================================
// 测试页（page-test.js）—— 三段：① 模拟异常 ② 舵机/LED 指令 ③ 用户请求
// ------------------------------------------------------------
// ⚠️⚠️ 白名单红线 ⚠️⚠️
//   filterAllowedFrames() 必须与 ST32 侧 App/ailink.c::handle_esp_line 的收帧白名单
//   **完全一致**。这里为了"测试方便"放宽一点点，F407 那边就会收到它不认识的帧，
//   现象是"网页说发出去了、机器人一动不动"，而且极难查。
//   所以下面这段是**原样搬运**的，不要改。要加新帧类型，先改 App/ailink.c 再改这里。
//   ★ 2026-09-19：本行原写 main.c，已订正 —— 收帧白名单早搬到了 App/ailink.c::handle_esp_line。
// ============================================================
(function () {
  'use strict';
  var D = window.DASH_API;

  var REMOTE_DEVICE_ID = 'esp-bridge';
  var REMOTE_QUICK_KEY = 'remote_quick_servo_led_v2';
  var REMOTE_QUICK_DEFAULT = [
    { name: '舵机1→100', frame: '@MOVE:servo,1,100#' },
    { name: '舵机1→90', frame: '@MOVE:servo,1,90#' },
    { name: '舵机2→45', frame: '@MOVE:servo,2,45#' },
    { name: '舵机2→30', frame: '@MOVE:servo,2,30#' },
    { name: '舵机3→90', frame: '@MOVE:servo,3,90#' },
    { name: '三舵归中', frame: '@MOVE:home,all#' },
    { name: 'LED1 亮', frame: '@LED_ON,LED1#' },
    { name: 'LED1 灭', frame: '@LED_OFF,LED1#' },
    { name: 'LED2 亮', frame: '@LED_ON,LED2#' },
    { name: 'LED2 灭', frame: '@LED_OFF,LED2#' },
  ];

  /* ================= 白名单（与 main.c 严格一致，别改） ================= */

  /** 规范化成 @...# 帧串（允许多帧粘在一起或换行分隔） */
  function normalizeAtHashFrames(text) {
    let s = String(text || '').trim();
    if (!s) return '';
    s = s.replace(/\r/g, '');
    if (s.indexOf('#') < 0) {
      s = s.split('\n').map((ln) => ln.trim()).filter(Boolean)
        .map((ln) => (ln.startsWith('@') ? ln : '@' + ln))
        .map((ln) => (ln.endsWith('#') ? ln : ln + '#'))
        .join('');
    }
    return s;
  }

  /* === [03网页端] BEGIN === 屏幕文字（@TEXT:）的字形检查 / 组帧工具 */

  /** F407 **片内**（编译进固件）字库那 32 个汉字 —— 也就是**回退**字库。
   *  ★ 源头 App/font_data.h：config.json::font_lib.charset（26 字）
   *    ∪ Tools/font_gen/gen.py 里写死的校名「重庆粤嵌科技」（6 字），26+6=32。
   *  ⚠️ 改 charset → 跑 gen.py → 重编译烧录之后**这一行必须同步**，否则提示会偏乐观。 */
  const SCREEN_CHARSET = new Set(
    ('你我他她好呀再见热冷谢累的了在不请来去真棒爱困怕吗呢' + '重庆粤嵌科技').split('')
  );

  /** 汉字基本区 —— 扩展字库 FONT16.BIN（3755 个一级汉字）**只会**落在这个范围里。
   *  ★ 为什么不把 3755 个字在网页里列一份：那份表是 Tools/font_gen 的产物，网页抄一份
   *    **必然抄歪**，而抄歪的后果是"报错报反"（能显示的字被报成 ?）—— 比不报还糟。
   *    所以这里只分"一定没有 / 取决于灌没灌"两档，不去猜具体哪几个字在库里。 */
  const CJK_MIN = 0x4e00;
  const CJK_MAX = 0x9fff;

  /** 返回 text 里**屏上不一定有字形**的字（去重、保持出现顺序）。两档都为空 = 全部能显示。
   *  · `certain` = 连扩展字库都救不了（非汉字 / 汉字基本区之外，如 emoji、全角标点）⇒ 一定画 `?`
   *  · `maybe`   = 汉字基本区里、但不在片内 32 字内 ⇒ **取决于机上灌没灌扩展字库**
   *  ★ 网页**看不到灌没灌**：那个判据在板子自己身上（调试口的 `FONT:W25Q 3755` 绿 /
   *    `FONT:FLASH 32` 黄）⇒ 这里只能说"取决于"，不能拍板说会变 ?。
   *  ★ 字库查不到时固件画 `?`（App/font.c 的 Font_Glyph16 返回 0）。 */
  function screenMissingChars(text) {
    const certain = [];
    const maybe = [];
    for (const ch of String(text || '')) {
      const cp = ch.codePointAt(0);
      if (cp >= 32 && cp <= 126) continue;   /* ASCII 走 lcd_font.h 的 8×16 点阵，不占字库 */
      if (SCREEN_CHARSET.has(ch)) continue;
      const bucket = (cp >= CJK_MIN && cp <= CJK_MAX) ? maybe : certain;
      if (bucket.indexOf(ch) < 0) bucket.push(ch);
    }
    return { certain, maybe };
  }

  /** 用户输入的一行文字 → @TEXT: 帧。不合格就 throw（由调用方弹给用户看）。
   *  ⚠️ 为什么必须在**这里**就挡掉 '#'
   *     —— 网页这层按第一个 '#' 切帧尾（本文件），ESP 那层
   *        （sketch_aug28a.ino::forwardAtHashRawToF407）也按第一个 '#' 截。
   *        正文里带 '#' ⇒ 句子被切成半句发出去，**而且两端都不报错**。
   *        与其静默发半句，不如拒发并说清楚。 */
  /** 上屏容量上限 —— **必须与 `flask_relay/app.py::SCREEN_TEXT_MAX_*` 同值**（两处都改）。 */
  const SCREEN_TEXT_MAX_BYTES = 168;
  const SCREEN_TEXT_MAX_CELLS = 160;

  /** 字形格数：ASCII 可打印字符 1 格，其余 2 格（对齐 App/media.c::draw_text）。 */
  function screenTextCells(s) {
    let n = 0;
    for (const ch of String(s || '')) {
      const cp = ch.codePointAt(0);
      n += (cp >= 32 && cp <= 126) ? 1 : 2;
    }
    return n;
  }

  function buildScreenTextFrame(text) {
    const t = String(text || '').trim();
    if (!t) throw new Error('先写点要显示的文字');
    if (t.indexOf('#') >= 0) {
      throw new Error("文字里不能有 '#' —— 它是帧尾，网页和 ESP 都会在这里把句子截断");
    }
    if (/[\r\n]/.test(t)) throw new Error('只能一行，不能换行');
    /* ★ 2026-09-23 订正：这行原写 `t.length > 64`（64 个**字**），是个"黑洞判据" ——
     *   F407 侧 `esp_uart.c:333` 在 `rx_used >= 191` 时**整行静默丢弃**（`ailink.c:605`
     *   又守一道 192）⇒ 64 个汉字 = 192 字节，加 `@TEXT:` 6 字节 = **198 字节，正好越线**：
     *   用户点了发送、网页说发出去了、屏上什么都没有、两端零报错。
     *   而 64 个 ASCII 才 70 字节，离上限远得很 —— 同一个数对两种字宽含义完全不同。
     * ★ 所以判据换成**字节 + 字形格数**双限（与云端 `app.py::_screen_text()` 一致）：
     *   ASCII 可打印字符占 1 格（8px），其余（汉字/emoji/全角标点）占 2 格（16px）。
     *   168 字节 ≈ 56 个汉字；`@TEXT:` + 168 = 174 < 191，留 17 字节余量。 */
    const bytes = new TextEncoder().encode(t).length;
    const cells = screenTextCells(t);
    if (bytes > SCREEN_TEXT_MAX_BYTES || cells > SCREEN_TEXT_MAX_CELLS) {
      throw new Error(
        `太长，屏上放不下：现在 ${t.length} 字 / ${bytes} 字节 / ${cells} 格，`
        + `上限 ${SCREEN_TEXT_MAX_BYTES} 字节且 ${SCREEN_TEXT_MAX_CELLS} 格（约 56 个汉字）。`
        + `★ 超了不是"显示不全"，是 F407 把整行丢掉 —— 屏上什么都不出现，也不报错。`
      );
    }
    /* 走同一条白名单，防手抄走形（自己拼的帧也要过闸） */
    const back = filterAllowedFrames(`@TEXT:${t}#`);
    if (!back) throw new Error(`内部错误：@TEXT:${t}# 没过白名单`);
    return back;
  }

  /* === [03网页端] END === */

  /** 白名单过滤：只保留
   *    @MOVE:servo,<1~16>,<角度>#         舵机定点（id = PCA9685 丝印号，直通）
   *    @SWEEP:servo,<1~16>#               舵机连续扫动 0↔180
   *    @LED_ON,LED<1|2># / @LED_OFF,...#  双LED（PA6=LED1, PA7=LED2）
   *    @MIC:start# / @MIC:stop# / @MIC:rec,<1~99>#   语音链路触发
   *    @TEXT:<文本>#                       整句送上屏（正文里的逗号/空格**原样保留**）
   *  其余一律丢弃。
   *  ★ 2026-09-19 从 1~3 放开到 1~16（对齐固件 PCA9685_CH_NUM=16）。
   *  ⚠️ 角度固件只收 0~180，超了是**丢弃**不是夹取（App/ailink.c::do_servo）。
   *  === [03网页端] BEGIN ===
   *  ★ 2026-09-20 加 @MIC: 三条（01语音 的固件帧，见 App/ailink.c::do_mic）。
   *    固件侧是 `strcmp`/`strncmp` **逐字比对，只认小写**；本函数与其它三条一样
   *    用 /i 收下任意大小写、**输出转小写**再发 ⇒ 用户敲 @MIC:START 也能用。
   *    ⚠️ 秒数这里写 [1-9]\d?（即 1~99），**比固件更严**，故意的：
   *      固件对 rec,0 和 rec,>99 是"**收下但忽略**"—— 不报错、什么都不发生。
   *      那种"发了没反应还不报错"正是本文件头警告的坑，所以挡在浏览器里让用户看到弹窗。
   *      ★ 宽松方向是禁止的（会发出 F407 不认识的帧）；收紧方向是安全的。
   *  === [03网页端] END ===
   */
  function filterAllowedFrames(text) {
    const s = normalizeAtHashFrames(text);
    if (!s) return '';
    const kept = [];
    let from = 0;
    while (from < s.length) {
      const at = s.indexOf('@', from);
      if (at < 0) break;
      const hash = s.indexOf('#', at + 1);
      if (hash < 0) break;
      const one = s.slice(at, hash + 1);
      const c = one.replace(/\s+/g, '');

      /* === [03网页端] BEGIN === @TEXT: 上屏（对齐 App/ailink.c 的 @TEXT: 分支）
       * ⚠️ 这一条**必须用 `one`（保空格），不能用上面的 `c`** —— `c` 把空白全删了，
       *    "HELLO WORLD" 会变成 "HELLOWORLD" 才发给机器人。
       *    @TEXT: 是唯一在乎空格的帧（固件那边也是"按前缀截后面全部"，逗号都不拆）。
       * ⚠️ 且必须排在其它帧名之前判断：下面那条 `else if` 链子一旦命中就会 continue。
       * `one` 保证只含一个 '#'（`hash` 是 `at` 之后的**第一个** '#'）⇒ 正文吞不掉帧尾。 */
      let mt = one.match(/^@TEXT:([\s\S]*)#$/i);
      if (mt) {
        const txt = mt[1].trim();
        if (txt) { kept.push(`@TEXT:${txt}#`); from = hash + 1; continue; }
      }
      /* === [03网页端] END === */

      let m = c.match(/^@MOVE:home(?:,(all|[1-9]|1[0-6])(?:,(\d+))?)?#$/i);
      if (m && (m[2] === undefined || Number(m[2]) <= 360)) {
        kept.push(`@MOVE:home,${(m[1] || 'all').toLowerCase()}${m[2] === undefined ? '' : ',' + m[2]}#`);
        from = hash + 1; continue;
      }
      m = c.match(/^@MOVE:servo,([1-9]|1[0-6]),(-?\d+)(?:,(\d+))?#$/i);
      if (m && (m[3] === undefined || Number(m[3]) <= 360)) { kept.push(`@MOVE:servo,${m[1]},${m[2]}${m[3] === undefined ? "" : "," + m[3]}#`); from = hash + 1; continue; }

      m = c.match(/^@SWEEP:servo,([1-9]|1[0-6])#$/i);
      if (m) { kept.push(`@SWEEP:servo,${m[1]}#`); from = hash + 1; continue; }

      m = c.match(/^@LED_(ON|OFF),LED([12])#$/i);
      if (m) { kept.push(`@LED_${m[1].toUpperCase()},LED${m[2]}#`); from = hash + 1; continue; }

      /* === [03网页端] BEGIN === @MIC: 三条（对齐 App/ailink.c::do_mic，见上方注释） */
      m = c.match(/^@MIC:(start|stop|rec,[1-9]\d?)#$/i);
      if (m) { kept.push(`@MIC:${m[1].toLowerCase()}#`); from = hash + 1; continue; }
      /* === [03网页端] END === */

      from = hash + 1;
    }
    return kept.join('');
  }

  /* ================= 快捷指令（localStorage） ================= */

  function loadQuickList() {
    try {
      const raw = localStorage.getItem(REMOTE_QUICK_KEY);
      if (raw) {
        const arr = JSON.parse(raw);
        if (Array.isArray(arr) && arr.length) {
          let migrated = false;
          arr.forEach(x => {
            if (x.name === '舵机2→135' && normalizeAtHashFrames(x.frame) === '@MOVE:servo,2,135#') {
              x.name = '舵机2→30'; x.frame = '@MOVE:servo,2,30#'; migrated = true;
            }
            if (x.name === '三舵归中' && normalizeAtHashFrames(x.frame) ===
                '@MOVE:servo,1,90#@MOVE:servo,2,90#@MOVE:servo,3,90#') {
              x.frame = '@MOVE:home,all#'; migrated = true;
            }
          });
          if (migrated) saveQuickList(arr);
          return arr.map((x) => ({
            name: x.name,
            frame: filterAllowedFrames(normalizeAtHashFrames(x.frame)),
          })).filter((x) => x.name && x.frame);
        }
      }
    } catch (e) { /* localStorage 用不了就用默认列表 */ }
    return REMOTE_QUICK_DEFAULT.slice();
  }

  function saveQuickList(list) {
    try { localStorage.setItem(REMOTE_QUICK_KEY, JSON.stringify(list)); }
    catch (e) { D.remoteLogSafe('快捷指令保存失败（localStorage 不可用）'); }
  }

  /* ================= 日志 ================= */

  function remoteLog(line) {
    const latest = D.$('#test-command-result');
    if (latest) latest.textContent = String(line).startsWith('入队 ok')
      ? '指令已入队，等待设备执行。请观察实物；详细指令见高级测试日志。'
      : String(line);
    const box = D.$('#remote-log');
    if (!box) return;
    const t = new Date().toTimeString().slice(0, 8);
    box.textContent = `[${t}] ${line}\n` + box.textContent;
  }

  /* ================= 渲染 ================= */

  function renderQuickButtons() {
    const box = D.$('#remote-quick');
    if (!box) return;
    const list = loadQuickList();
    box.innerHTML = '';
    list.forEach((item, idx) => {
      const wrap = document.createElement('span');
      wrap.className = 'qbtn-wrap';
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'qbtn';
      btn.textContent = item.name;
      btn.title = item.frame;
      btn.addEventListener('click', async () => {
        try {
          await enqueueRemote({ raw: normalizeAtHashFrames(item.frame), note: item.name });
        } catch (err) {
          remoteLog('入队失败: ' + err);
        }
      });
      const del = document.createElement('button');
      del.type = 'button';
      del.className = 'qbtn-del';
      del.textContent = '×';
      del.title = '删除此快捷';
      del.addEventListener('click', () => {
        const next = loadQuickList().filter((_, i) => i !== idx);
        saveQuickList(next);
        renderQuickButtons();
      });
      wrap.appendChild(btn);
      wrap.appendChild(del);
      box.appendChild(wrap);
    });
  }

  let servoProfileKey = '';

  async function renderServoTestPanel() {
    const box = D.$('#servo-test-panel');
    if (!box) return;
    let groups;
    try {
      const library = await D.getJSON('/api/actions?device_id=' + encodeURIComponent(REMOTE_DEVICE_ID));
      groups = library.servos || [];
      const key = JSON.stringify(groups);
      if (key === servoProfileKey) return;
      servoProfileKey = key;
    } catch (e) { remoteLog('读取管理员舵机参数失败：' + e); return; }
    box.innerHTML = '';
    const head = document.createElement('div');
    head.className = 'servo-test-title';
    head.textContent = '舵机测试（修改速度后点“保存为默认”，AI与动作库同步使用；0为立即）';
    box.appendChild(head);

    groups.forEach((g) => {
      const row = document.createElement('div');
      row.className = 'servo-group-row';

      const lab = document.createElement('span');
      lab.className = 'servo-group-label';
      lab.textContent = `舵机${g.id} ${g.name || ''}（${g.min}~${g.max}°，正面${g.front}°）`;
      row.appendChild(lab);

      const speed = document.createElement('input');
      speed.type = 'number'; speed.min = '0'; speed.max = '360'; speed.step = '1';
      speed.value = String(g.speed); speed.className = 'remote-mini';
      speed.id = 'servo-speed-' + g.id; speed.title = '度/秒；0为立即到目标';
      const speedLabel = document.createElement('label');
      speedLabel.textContent = '速度(°/秒) '; speedLabel.appendChild(speed); row.appendChild(speedLabel);
      const saveSpeed = document.createElement('button');
      saveSpeed.type = 'button'; saveSpeed.className = 'qbtn';
      saveSpeed.textContent = '保存为默认'; saveSpeed.dataset.servoSpeedSave = g.id;
      saveSpeed.onclick = async () => {
        saveSpeed.disabled = true;
        try {
          const value = Number(speed.value);
          if (!Number.isInteger(value) || value < 0 || value > 360 || speed.value === '') throw new Error('速度只能是0~360度/秒');
          const result = await D.postJSON('/api/admin/servo-speeds', {speeds: {[g.id]: value}});
          if (!result.ok) throw new Error(result.error || '保存失败');
          remoteLog(`舵机${g.id}默认速度已保存为${value}°/秒；下一次AI和动作库使用新速度。`);
          servoProfileKey = ''; await renderServoTestPanel();
        } catch (err) { remoteLog('默认速度保存失败：' + err); }
        finally { saveSpeed.disabled = false; }
      };
      row.appendChild(saveSpeed);
      const mk = (name, frame) => {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = 'qbtn';
        b.textContent = name;
        b.title = frame;
        b.addEventListener('click', async () => {
          try {
            const value = Number(speed.value);
            if (!Number.isInteger(value) || value < 0 || value > 360) throw new Error('速度只能是0~360度/秒');
            const command = /^@MOVE:(servo|home),/.test(frame) ? frame.slice(0, -1) + ',' + value + '#' : frame;
            await enqueueRemote({ raw: normalizeAtHashFrames(command), note: `舵机${g.id} ${name}` });
          } catch (err) {
            remoteLog('入队失败: ' + err);
          }
        });
        row.appendChild(b);
      };

      mk(`↻ 扫动 ${g.min}↔${g.max}`, `@SWEEP:servo,${g.id}#`);
      mk(`${g.min}°`, `@MOVE:servo,${g.id},${g.min}#`);
      mk(`正面 ${g.front}°`, `@MOVE:home,${g.id}#`);
      mk(`${g.max}°`, `@MOVE:servo,${g.id},${g.max}#`);
      box.appendChild(row);
    });
  }

  // 传感器预设：点一下就改写 #in-sensor，方便一键造出想要的异常
  const SENSOR_PRESETS = [
    { name: '正常',        v: '温度:28.5,湿度:65' },
    { name: '🔥 高温',     v: '温度:35.5,湿度:65' },
    { name: '❄ 低温',      v: '温度:2,湿度:65' },
    { name: '💧 高湿',     v: '温度:28.5,湿度:90' },
    { name: '📏 距离过近', v: '温度:28.5,湿度:65,距离:8' },
    { name: '👤 有人',     v: '温度:28.5,湿度:65,距离:42,人:有' },
    { name: '🚪 无人',     v: '温度:28.5,湿度:65,距离:42,人:无' },
    { name: '⚠ 无数据',   v: '无数据' },
  ];

  function renderSensorPresets() {
    const box = D.$('#sensor-presets');
    if (!box) return;
    box.innerHTML = '';
    SENSOR_PRESETS.forEach((p) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'qbtn';
      b.textContent = p.name;
      b.title = p.v;
      b.addEventListener('click', () => {
        const el = D.$('#in-sensor');
        if (el) { el.value = p.v; el.focus(); }
      });
      box.appendChild(b);
    });
  }

  /* ================= 设备状态 ================= */

  async function refreshDevices() {
    try {
      const j = await D.getJSON('/api/devices');
      const list = j.devices || [];
      const d = list.find((x) => x.device_id === REMOTE_DEVICE_ID) || list[0];
      if (window.MOTION_TIMELINE) window.MOTION_TIMELINE.status(d);
      const online = D.$('#remote-online');
      const queue = D.$('#remote-queue');
      const ack = D.$('#remote-ack');
      const idEl = D.$('#remote-device-id');
      if (idEl) idEl.textContent = (d && d.device_id) || REMOTE_DEVICE_ID;
      if (queue) queue.textContent = d ? d.queue_len : 0;
      if (ack) ack.textContent = (d && d.last_ack) || '—';
      if (online) {
        const on = !!(d && d.online);
        const fresh = on && d.poll_fresh !== false;
        online.textContent = fresh ? '在线' : (on ? '上报间断' : '离线');
        online.className = 'remote-pill ' + (fresh ? 'on' : 'off');
        online.title = d && d.poll_age_sec != null
          ? `距设备上报 ${d.poll_age_sec} 秒；录音上传期间也可能暂停上报。这是设备到网页的状态，屏幕 ESP:LOST 是板间通信状态。`
          : '还没有设备上报';
      }
    } catch (e) {
      /* 未登录等，忽略 */
    }
  }

  async function enqueueRemote(payload) {
    const body = Object.assign({ device_id: REMOTE_DEVICE_ID }, payload);
    // 隔离：丢掉 text/moves，只发白名单内的 raw
    delete body.text;
    delete body.moves;
    if (body.raw) body.raw = filterAllowedFrames(body.raw);
    if (!body.raw) throw new Error('仅允许 @MOVE:servo,id,angle[,speed]#、@SWEEP:servo,id#、@LED_ON/OFF,LED1|LED2#、@MIC:start/stop/rec,<1~10># 或 @TEXT:<文本>#');
    const j = await D.postJSON('/api/command', body);
    if (j.error) throw new Error(j.error);
    remoteLog(`入队 ok queue=${j.queue_len} ${JSON.stringify(j.command)}`);
    refreshDevices();
    return j;
  }

  /* ================= ① 模拟异常 ================= */

  function frameBlocksHtml(j) {
    return `<div class="block-label">① 云端决策（信封 JSON ＝ F407 将收到的决策）</div>`
      + `<pre>${D.escapeHtml(JSON.stringify(j, null, 2))}</pre>`
      + `<div class="block-label">② ESP 将转发给 F407 的类型化帧（逐条 moves 拆帧）</div>`
      + `<pre class="frame-box">${D.escapeHtml(D.framesPreview(j))}</pre>`;
  }

  function bindAlert() {
    const btn = D.$('#btn-alert');
    if (!btn) return;
    btn.addEventListener('click', async () => {
      const box = D.$('#alert-result');
      const text = D.$('#in-alert').value.trim();
      if (!text) { alert('先填异常 / 事件描述'); return; }
      const useMock = !!D.$('#in-alert-mock').checked;
      btn.disabled = true;
      btn.textContent = '上报中…';
      const t0 = performance.now();
      try {
        // 模拟 F407 检测到异常 → 经 ESP 上报云端
        // ★ no_pop：这是**浏览器**发的请求，不是 ESP 的轮询 ⇒ 不许领走命令队列。
        //   领了就是丢（ESP 拿不到、浏览器也不转发）—— 见 问题日志_AI速查.md 抢单条。
        const j = await D.postJSON('/api/data', {
          sensor: (D.$('#in-sensor') || {}).value || '',
          user: text,
          mock: useMock,
          need_ai: !useMock,
          no_pop: true,
        });
        const ms = Math.round(performance.now() - t0);
        box.className = 'result ok';
        box.innerHTML = `<div class="head">✔ ${useMock ? '测试模式已返回' : 'AI 已决策'} · ${ms}ms · ` +
          `${D.escapeHtml(D.movesSummary(j))}</div>${frameBlocksHtml(j)}`;
      } catch (e) {
        box.className = 'result err';
        box.innerHTML = `<div class="head">✘ 上报失败</div><pre>${D.escapeHtml(String(e))}</pre>`;
      } finally {
        box.classList.remove('hidden');
        btn.disabled = false;
        btn.textContent = '上报异常 → 请求 AI 决策';
        D.refreshAll();
      }
    });
  }

  /* ================= ③ 用户请求 / 模拟上报 ================= */

  function bindTestSend() {
    const btn = D.$('#btn-send');
    if (!btn) return;
    btn.addEventListener('click', async () => {
      const box = D.$('#test-result');
      btn.disabled = true;
      btn.textContent = '调用中…';
      const t0 = performance.now();
      try {
        const j = await D.postJSON('/api/data', {
          sensor: D.$('#in-sensor').value,
          user: D.$('#in-user').value,
          mock: D.$('#in-mock').checked,
          need_ai: !D.$('#in-mock').checked,   // 非测试模式就要求AI（空用户也能自动判断）
          no_pop: true,   // ★ 浏览器请求不许领命令队列（同上）
        });
        const ms = Math.round(performance.now() - t0);
        const fired = j.alerts || [];
        const alertLine = fired.length
          ? '<div class="block-label">命中的阈值规则（可在「异常」页看到）</div><pre class="frame-box">' +
            D.escapeHtml(fired.map(function (a) { return '[' + a.severity + '] ' + a.code + ' — ' + (a.message || ''); }).join('\n')) +
            '</pre>'
          : '<p class="hint">本次没有命中任何阈值规则。</p>';
        box.className = 'result ok';
        box.innerHTML = `<div class="head">✔ 成功 · ${ms}ms · ${D.escapeHtml(D.movesSummary(j))}</div>` +
          alertLine + `<pre>${D.escapeHtml(JSON.stringify(j, null, 2))}</pre>`;
      } catch (e) {
        box.className = 'result err';
        box.innerHTML = `<div class="head">✘ 请求失败</div><pre>${D.escapeHtml(String(e))}</pre>`;
      } finally {
        box.classList.remove('hidden');
        btn.disabled = false;
        btn.textContent = '发送测试请求';
        D.refreshAll();
      }
    });
  }

  /* ================= ② 舵机自由发送 / 添加快捷 ================= */

  function bindRemote() {
    const btnAddQ = D.$('#btn-remote-add-q');
    if (btnAddQ) {
      btnAddQ.addEventListener('click', () => {
        const name = (D.$('#remote-q-name').value || '').trim();
        const frame = filterAllowedFrames((D.$('#remote-q-frame').value || '').trim());
        if (!name || !frame) {
          alert('快捷名必填；帧只能是舵机定点/扫动/双LED/@MIC 白名单帧');
          return;
        }
        const list = loadQuickList();
        list.push({ name, frame });
        saveQuickList(list);
        D.$('#remote-q-name').value = '';
        D.$('#remote-q-frame').value = '';
        renderQuickButtons();
        remoteLog('已添加快捷: ' + name + ' → ' + frame);
      });
    }

    const btnSend = D.$('#btn-remote-send');
    if (btnSend) {
      btnSend.addEventListener('click', async () => {
        const el = D.$('#remote-raw');
        const raw = filterAllowedFrames((el && el.value) || '');
        if (!raw) {
          alert('只接受白名单帧，例如 @MOVE:servo,1,100# / @SWEEP:servo,1# / @LED_ON,LED1# 或 @MIC:rec,2#');
          return;
        }
        btnSend.disabled = true;
        try {
          await enqueueRemote({ raw, note: '舵机测试' });
          if (el) el.value = '';
        } catch (err) {
          remoteLog('入队失败: ' + err);
        } finally {
          btnSend.disabled = false;
        }
      });
    }

    /* === [03网页端] BEGIN === 语音链路触发按钮（帧对齐 App/ailink.c::do_mic）
     * ★ 这一期做两件事：① 发帧；② 录音期间显一条「屏面暂停刷新，不是死机」提示。
     *   `/api/audio/status` **已经实现了**（app.py → audio_rx.status()），所以能知道
     *   这一轮到底在不在推流 —— 不再只是"已发送"。
     *   ★ 但仍然**别去解析串口原始数据** —— F407 的结果打在它自己的调试口上，网页看不到。 */

    /** 秒数上下限 = 固件 miclink.h 的 MICLINK_SEC_MAX_PLANNED。
     *  ★ 线缆协议本身收 1~99，所以上面那条白名单正则**故意不动**（和固件逐字一致）；
     *    但 >10 会被固件**静默夹到 10**（miclink.c，不是报错）⇒ 在输入框这层就夹住，
     *    免得用户以为录了 15 秒、实际只有 10 秒。 */
    const MIC_SEC_MIN = 1;
    const MIC_SEC_MAX = 10;

    /** 读 #in-mic-sec → 合法秒数。空/乱填返回 0，由调用方拦下来（不猜）。 */
    function readMicSec() {
      const el = D.$('#in-mic-sec');
      const n = Number(String((el && el.value) || '').trim());
      if (!isFinite(n) || n <= 0) return 0;
      return Math.min(MIC_SEC_MAX, Math.max(MIC_SEC_MIN, Math.round(n)));
    }

    /** 「屏面暂停刷新 / 录音中」提示的轮询把手。
     *  ★ 代次号：连点两次时，**旧的那一轮必须自己退场**，否则它超时撤掉的会是新提示。 */
    let micHintTimer = null;
    let micHintGen = 0;

    /** 起录之后盯一会儿 `/api/audio/status`，在本轮结束时把提示撤掉。
     *  ★ 判据来自 决策协议v2.md「网页端判"录完了"用 `live` 长度 1→0」：
     *    `live` 是**列表**，AudioSession 由 finish() 摘除 ⇒ 先出现、后清空（不是布尔量）。
     *  ⚠️ 但它只覆盖推流那条路：**N≤2 是 RAM 版（miclink.c），根本不建会话** ⇒ `live` 永远 `[]`。
     *     只等"1→0"的话，录 2 秒也能把提示吊到硬超时才撤 ⇒ 补一条兜底：
     *     起跑后 `n+3` 秒仍没见过 `live` ⇒ 认定 RAM 版，直接撤。
     *  ⚠️ 但那道兜底**必须以"真读到过状态"为前提**（`everRead`）：接口要是全程 500，
     *     "没见到 `live`"只是"没看成"，不是"没有会话" —— 拿它当 RAM 版的判据就会把一条
     *     本来就推流的提示在 `n+3` 秒撤掉。看不成 ⇒ 只吃 `n + 15` 硬超时。
     *  ⚠️ 两条路都套 `n + 15` 秒客户端硬超时（协议文档定死的数：服务端 SESSION_IDLE_SEC=30
     *     才回收卡住的会话，客户端必须比它早）。
     *  ★ 撤得不早：`live` 清空必然晚于**屏面恢复**（会话结束时推流早完了）⇒ 提示只会撤晚。
     *    ⚠️ 这句**只对推流路成立**，RAM 路没有会话、"晚于"无从谈起 —— 别当全局判据引用。 */
    function watchMicStream(sec) {
      /* 先把上一轮（可能还在跑的）提示和轮询收干净 */
      micHintGen += 1;
      if (micHintTimer) { clearTimeout(micHintTimer); micHintTimer = null; }
      const gen = micHintGen;
      const hint = D.$('#mic-hint');
      const stop = () => {
        if (gen !== micHintGen) return;              /* 我这轮已被更新的一轮取代 */
        if (micHintTimer) { clearTimeout(micHintTimer); micHintTimer = null; }
        if (hint) { hint.classList.add('hidden'); hint.innerHTML = ''; }
      };

      if (hint) {
        hint.innerHTML = (sec > 2)
          ? `🎙 已请求录音 ${sec} 秒，等待下方出现“正在接收录音”后说话。<b>录音期间保留画面，底部显示录音和上传状态</b>。`
          : `🎙 录音 ${sec} 秒中（RAM 版）：结果只在 F407 调试口，不产生 wav。`;
        hint.classList.remove('hidden');
      }
      remoteLog(sec > 2
        ? `录音 ${sec} 秒开始（推流版：观察屏幕底部状态）`
        : `录音 ${sec} 秒开始（RAM 版：不产生 wav，识别那一套对它没意义）`);

      const t0 = Date.now();
      const ramStop = t0 + (sec + 3) * 1000;
      const hardStop = t0 + (sec + 15) * 1000;
      let sawLive = false;
      let everRead = false;
      let busy = false;

      const tick = async () => {
        if (gen !== micHintGen) return;
        if (busy) return;                            /* 上一拍还没回来就跳过，别叠请求 */
        busy = true;
        try {
          const j = await D.getJSON('/api/audio/status');
      window.ROBOT_AVATAR?.audio(j);
      window.COMPANION_AUDIO_VIEW?.render(j);
          const live = (j && j.live) || [];
          everRead = true;
          if (live.length >= 1) sawLive = true;
          if (sawLive && live.length === 0) { stop(); return; }
        } catch (e) {
          /* 接口报错不能把提示吊死 —— 交给下面的超时兜底 */
        } finally {
          busy = false;
        }
        const now = Date.now();
        /* ★ 只有"真读到过状态、且里面始终没有会话"才敢判 RAM 版 */
        if (sec <= 2 && everRead && !sawLive && now >= ramStop) { stop(); return; }
        if (now >= hardStop) { stop(); return; }
        micHintTimer = setTimeout(tick, 1000);
      };
      micHintTimer = setTimeout(tick, 1000);
    }

    /** 通用：把一条 @MIC: 帧挂到按钮上（同样走白名单，防手抄走形）。
     *  `frameOrFn` 传函数时在**点击那一刻**才算帧（自定义秒数用），返回空串 = 已提示过、别发。 */
    function bindMicBtn(id, frameOrFn, label) {
      const b = D.$('#' + id);
      if (!b) return;
      b.addEventListener('click', async () => {
        let raw;
        try {
          const f = (typeof frameOrFn === 'function') ? frameOrFn() : frameOrFn;
          if (!f) return;
          raw = filterAllowedFrames(f);
          if (!raw) throw new Error('内部错误：' + f + ' 没过白名单');
        } catch (err) {
          alert(String((err && err.message) || err));
          return;
        }
        b.disabled = true;
        try {
          await enqueueRemote({ raw, note: label });
          remoteLog(`录音指令已入队 ${raw}，请等待设备开始接收。`);
          const mr = raw.match(/^@MIC:rec,(\d+)#$/);
          if (mr) watchMicStream(Number(mr[1]));
        } catch (err) {
          remoteLog('入队失败: ' + err);
        } finally {
          b.disabled = false;
        }
      });
    }
    bindMicBtn('btn-mic-rec2', '@MIC:rec,2#', '语音：录音 2 秒');
    /* ★ 自定义秒数：卡 1~10；不合法就当场拦住，不猜、也不发"看着发了其实没用"的帧。
     *   夹过的值写回输入框，让用户看见实际会录几秒。 */
    bindMicBtn('btn-mic-recN', () => {
      const sec = readMicSec();
      if (!sec) { alert(`先填秒数（${MIC_SEC_MIN}~${MIC_SEC_MAX}）`); return ''; }
      const el = D.$('#in-mic-sec');
      if (el) el.value = String(sec);
      return `@MIC:rec,${sec}#`;
    }, '语音：按自定义秒数录音');
    bindMicBtn('btn-mic-stop', '@MIC:stop#', '语音：取消录音（仅红点亮起前有效）');

    /* 输入框失焦就夹一次：`min`/`max` 属性只挡步进箭头，手打 15 照样进得来 */
    const micSecEl = D.$('#in-mic-sec');
    if (micSecEl) {
      micSecEl.addEventListener('blur', () => {
        const sec = readMicSec();
        if (sec) micSecEl.value = String(sec);
      });
    }

    /* ===== 屏幕文字：@TEXT: 直接上屏 =====
     * ★ 这一期同样只做"发帧"：帧走 网页 → Flask 入队 → ESP 取令 → F407，
     *   F407 执行 @TEXT: 时只在自己调试口打 [CAP] ...，**网页看不到上屏结果**。
     *   所以这里只报"已发送"，别去解析串口原始数据。 */
    function bindScreenText() {
      const inp = D.$('#in-screen-text');
      const btn = D.$('#btn-screen-text');
      const warn = D.$('#screen-text-warn');
      if (!inp || !btn) return;

      /* 边打边报：多少字/多少字节 + 哪几个字**不一定**有字形（分"一定没有 / 取决于字库"两档） */
      const refreshWarn = () => {
        if (!warn) return;
        const t = inp.value.trim();
        if (!t) { warn.classList.add('hidden'); return; }
        warn.classList.remove('hidden');
        const miss = screenMissingChars(t);
        const bytes = new TextEncoder().encode(t).length;
        const cells = screenTextCells(t);
        /* ★ 边打边报就把超限说清楚：等点了发送才弹窗，用户已经白写一段了 */
        const parts = [cells > SCREEN_TEXT_MAX_CELLS || bytes > SCREEN_TEXT_MAX_BYTES
          ? `❌ ${t.length} 字 / ${bytes} 字节 / ${cells} 格 —— 超上限（${SCREEN_TEXT_MAX_BYTES} 字节 / ${SCREEN_TEXT_MAX_CELLS} 格），发出去 F407 会整行丢掉`
          : `${t.length} 字 / ${bytes} 字节 / ${cells} 格`];
        if (!miss.certain.length && !miss.maybe.length) {
          parts.push('✅ 每个字屏上都有字形');
        }
        if (miss.certain.length) {
          parts.push(`❌ 这几个<b>一定</b>画成 <code>?</code>（扩展字库也没有它们）：`
            + `<b>${D.escapeHtml(miss.certain.join(''))}</b>`);
        }
        if (miss.maybe.length) {
          parts.push(`⚠️ 这几个不在<b>片内</b>那 32 字里：<b>${D.escapeHtml(miss.maybe.join(''))}</b>`
            + ` —— <b>机上灌过扩展字库就正常显示，没灌就画 <code>?</code></b>`
            + `（扩展字库只有 3755 个一级汉字，<b>繁体 / 生僻字即使灌了也没有</b>，那种只能靠片内那 32 字）。`
            + `★ 网页看不到灌没灌，看板子调试口：<code>FONT:W25Q 3755</code>=绿（有）/ `
            + `<code>FONT:FLASH 32</code>=黄（没有）`);
        }
        if (t.indexOf('#') >= 0) parts.push('❌ 不能含 <code>#</code>，会把句子截断');
        warn.innerHTML = parts.join(' · ');
      };

      const send = async () => {
        let frame;
        try {
          frame = buildScreenTextFrame(inp.value);
        } catch (err) {
          alert(String((err && err.message) || err));
          return;
        }
        btn.disabled = true;
        try {
          await enqueueRemote({ raw: frame, note: '上屏文字' });
          remoteLog(`已发送 ${frame}（上屏结果在机器人那边，网页看不到）`);
        } catch (err) {
          remoteLog('入队失败: ' + err);
        } finally {
          btn.disabled = false;
        }
      };

      inp.addEventListener('input', refreshWarn);
      inp.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { e.preventDefault(); send(); }
      });
      btn.addEventListener('click', send);
      refreshWarn();
    }
    bindScreenText();
    /* === [03网页端] END === */
  }

  /* ================= 组装 ================= */

  let audioSelectedId = '', audioSelectionPinned = false, audioHistoryOffset = 0, audioHistory = [];
  let audioCurrentRecord = null;
  let retentionLoaded = false, retentionDirty = false, retentionNote = '';
  function asrFailureKind(record) {
    if (!record || !record.complete || record.text || !record.err) return '';
    const code = record.asr_error_code || '';
    // Old saved recordings have no error code; keep their actual cause visible.
    if (code === 'dependency_missing' || /(?:ModuleNotFoundError|No module named|ImportError)/.test(record.err)) return 'dependency';
    if (['model_load_failed', 'model_unavailable', 'file_missing', 'transcription_failed'].includes(code) ||
        /(?:模型|识别失败|处理失败)/.test(record.err)) return 'service';
    if (code === 'no_speech' || /(?:未检测到清晰人声|识别结果为空)/.test(record.err)) return 'silence';
    return '';
  }
  function asrServiceMessage(model) {
    if (!model) return '云端识别状态暂不可用。';
    if (model.loaded) return '云端语音识别已就绪（' + (model.name || 'Whisper') + '）。';
    if (model.error) return model.error_code === 'dependency_missing' || /No module named|ImportError/.test(model.error)
      ? '云端语音识别不可用：缺少识别组件，请运行新版网页一键更新安装依赖和模型。'
      : '云端语音识别不可用：模型加载失败。请检查部署日志。';
    return '云端语音识别正在准备模型，首次使用可能需要下载，请稍候。';
  }
  async function loadAudioHistory(offset) {
    const result = await D.getJSON('/api/audio/history?offset=' + Math.max(0, offset));
    audioHistoryOffset = result.offset; audioHistory = result.items || [];
    refreshAudio();
  }
  function renderAudioHistory(j) {
    const box = D.$('#audio-history'); if (!box) return;
    box.replaceChildren();
    const stateNames = { queued: '排队识别', processing: '识别中', pending: '待确认',
      sending: '发送中', manual: '已发送', auto: '自动已发送', dropped: '已取消发送',
      skipped: '未识别', interrupted: '处理被中断', imported: '历史录音',
      manual_failed: '发送失败可重试', auto_failed: '自动发送失败' };
    const summary = document.createElement('p');
    summary.textContent = '共保存 ' + (j.history_total || 0) + ' 条 · 待确认 ' + (j.pending_count || 0) + ' 条 · 等待识别 ' + (j.processing_queue || 0) + ' 条';
    box.appendChild(summary);
    const active=D.$('#audio-active-queue');
    if(active){
      active.replaceChildren();
      const pending=new Set(['queued','processing','pending','sending','manual_failed','auto_failed']);
      const latest=j.history||[];
      const recent=latest.filter((r,i)=>pending.has(r.inject)||i<3||r.id===audioSelectedId);
      const selected=audioHistory.find(r=>r.id===audioSelectedId);
      if(selected&&!recent.some(r=>r.id===selected.id))recent.push(selected);
      D.$('#audio-queue-count').textContent=(j.pending_count||0)+' 条待确认';
      if(!recent.length){const empty=document.createElement('p');empty.className='queue-empty';empty.textContent='录完一段，声音就会留在这里。';active.append(empty);}
      recent.forEach(record=>{
        const row=document.createElement('button');row.type='button';row.className='audio-record-card';row.setAttribute('aria-pressed',String(record.id===audioSelectedId));
        const top=document.createElement('div'),title=document.createElement('strong'),state=document.createElement('span');
        title.textContent='#'+record.number+' · '+(record.dur||'—')+' 秒';state.textContent=stateNames[record.inject]||record.inject;top.append(title,state);
        const text=document.createElement('p'),time=document.createElement('small');text.textContent=record.text||record.err||'等待识别文字';time.textContent=record.at;
        row.append(top,text,time);row.onclick=()=>{audioSelectedId=record.id;audioSelectionPinned=true;refreshAudio();};active.append(row);
      });
    }
    audioHistory.forEach(record => {
      const row = document.createElement('button'); row.type = 'button'; row.className = 'qbtn';
      row.style.display = 'block'; row.style.marginBottom = '6px';
      row.textContent = (record.id === audioSelectedId ? '▶ ' : '') + '#' + record.number + ' ' + record.at +
        ' · ' + record.dur + ' 秒 · ' + (record.inject === 'skipped'
          ? !record.complete ? '录音不完整' : ['dependency', 'service'].includes(asrFailureKind(record))
            ? '识别服务故障' : asrFailureKind(record) === 'silence' ? '未检测到人声' : '未识别'
          : stateNames[record.inject] || record.inject);
      row.onclick = () => { audioSelectedId = record.id; audioSelectionPinned = true; refreshAudio(); };
      box.appendChild(row);
    });
    D.$('#audio-history-prev').disabled = audioHistoryOffset === 0;
    D.$('#audio-history-next').disabled = audioHistoryOffset + audioHistory.length >= (j.history_total || 0);
  }
  let controlsLoaded = false, controlsDirty = false, controlsBusy = false, audioSilenceMs = 800;
  async function refreshControls() {
    if (controlsBusy) return; controlsBusy = true;
    try {
      const j = await D.getJSON('/api/device/controls?device_id=' + encodeURIComponent(REMOTE_DEVICE_ID));
      const c = j.controls; audioSilenceMs = c.silence_ms;
      if (!controlsLoaded && !controlsDirty) {
        D.$('#vad-enabled').checked = c.enabled;
        ['minimum', 'margin', 'onset_ms', 'silence_ms', 'servo_speed'].forEach(k => { D.$('#control-' + k).value = c[k]; });

        controlsLoaded = true;
      }
      D.$('#controls-status').textContent = controlsDirty ? '参数已修改，点击保存后生效。' :
        c.applied ? 'F407已确认应用这些参数；重启后会自动恢复。' : '已保存，等待ESP转发及F407确认（需新版两板固件）。';
    } catch(e) { D.$('#controls-status').textContent = '读取参数失败：' + e; }
    finally { controlsBusy = false; }
  }
  function bindDeviceControls() {
    D.$('#device-controls').addEventListener('input', () => { controlsDirty = true; });
    D.$('#vad-enabled').addEventListener('input', () => { controlsDirty = true; });
    D.$('#controls-save').onclick = async () => {
      const button = D.$('#controls-save'); button.disabled = true;
      const controls = { enabled: D.$('#vad-enabled').checked };
      ['minimum', 'margin', 'onset_ms', 'silence_ms', 'servo_speed'].forEach(k => { controls[k] = Number(D.$('#control-' + k).value); });
      try {
        const r = await D.postJSON('/api/device/controls', { device_id: REMOTE_DEVICE_ID, controls });
        if (!r.ok) throw new Error(r.err || '保存失败');
        controlsDirty = false; controlsLoaded = false; await refreshControls();
        servoProfileKey = ''; await renderServoTestPanel();
      } catch(e) { D.$('#controls-status').textContent = '保存失败：' + e; }
      finally { button.disabled = false; }
    };
    D.$('#audio-history-prev').onclick = () => loadAudioHistory(audioHistoryOffset - 100).catch(remoteLog);
    D.$('#audio-history-next').onclick = () => loadAudioHistory(audioHistoryOffset + 100).catch(remoteLog);
    D.$('#audio-retention-limit').addEventListener('input', () => { retentionDirty = true; });
    D.$('#audio-retention-save').onclick = async () => {
      const button = D.$('#audio-retention-save'); button.disabled = true;
      try {
        const r = await D.postJSON('/api/audio/retention', { limit: Number(D.$('#audio-retention-limit').value) });
        if (!r.ok) throw new Error(r.err || '保存失败');
        retentionDirty = false; retentionLoaded = false; audioHistoryOffset = 0;
        audioSelectedId = ''; audioCurrentRecord = null; audioSelectionPinned = false;
        retentionNote = '本次清理 ' + r.retention.deleted + ' 条旧录音。' +
          (r.retention.failed ? '有 ' + r.retention.failed + ' 条暂未能清理。' : '');
        await refreshAudio();
      } catch(e) { D.$('#audio-retention-status').textContent = '保存失败：' + e.message; }
      finally { button.disabled = false; }
    };
    refreshControls();
  }
  let audioStatusBusy = false;
  let audioSession = '';
  let audioActionBusy = false;
  let audioMode = '';        // refreshAudio() 每次从 /api/audio/status 刷新；切换按钮按它决定切哪边
  async function refreshAudio() {
    if (audioStatusBusy) return;
    audioStatusBusy = true;
    const status = D.$('#audio-result-status');
    try {
      const j = await D.getJSON('/api/audio/status');
      D.$('#audio-asr-status').textContent = asrServiceMessage(j.model);
      const transportRow = D.$('#audio-transport-status');
      const transports = j.transport || {};
      const deviceInput = D.$('#remote-device-id');
      const selectedDevice = deviceInput ? deviceInput.textContent.trim() : '';
      const report = transports[selectedDevice] || transports['esp-bridge'];
      if (transportRow) {
        const phases = { recording: '录音中', uploading: '上传中', ready: '等待录音', failed: '录音中止' };
        const reasons = {
          'MCU reported lost microphone samples': 'MCU 报告麦克风数据丢失',
          'MCU ended before all blocks confirmed': 'MCU 提前结束，块数未对齐',
          'MCU frames timed out 2500ms': 'ESP 等不到 MCU 后续音频帧',
          'flash write failed': 'ESP Flash 写入失败',
          'capture interrupted': '录音被中止',
        };
        transportRow.textContent = report ? `ESP ${report.version || ''} · ${phases[report.phase] || report.phase || '未知状态'} · 接收 ${report.accepted || 0}/${report.total || 0} 字节 · 上传 ${report.sent || 0} 字节` +
          (report.error && report.error !== 'none' ? ' · ' + (reasons[report.error] || report.error) : '') +
          (report.uart_overruns ? ` · 串口重传事件 ${report.uart_overruns}` : '') : '设备上传状态尚未回报；新 ESP 固件会自动回报，无需串口。';
      }
      const retention = j.retention || { enabled: false, limit: 200 };
      if (!retentionLoaded && !retentionDirty) {
        D.$('#audio-retention-limit').value = retention.limit; retentionLoaded = true;
      }
      D.$('#audio-retention-status').textContent = retentionDirty ? '数量已修改，保存后生效并清理旧录音。' :
        (retention.enabled ? '保留最近 ' + retention.limit + ' 条；更早的待处理或待确认记录额外保留。' :
          '尚未启用数量上限，现有历史录音保留；点击保存后才开始清理。') + retentionNote;
      const live = j.live || [];
      if (audioHistoryOffset === 0) audioHistory = j.history || [];
      else audioHistory = (await D.getJSON('/api/audio/history?offset=' + audioHistoryOffset)).items || [];
      const latest = j.last;
      const playing = D.$('#audio-player') && !D.$('#audio-player').paused;
      if (!audioSelectionPinned && !playing && latest) audioSelectedId = latest.id;
      const selected = audioHistory.find(r => r.id === audioSelectedId);
      const last = selected || ((audioSelectionPinned || playing) && audioCurrentRecord &&
        audioCurrentRecord.id === audioSelectedId ? audioCurrentRecord : latest);
      audioCurrentRecord = last;
      renderAudioHistory(j);
      const player = D.$('#audio-player');
      const link = D.$('#audio-download');
      if (live.length) {
        const s = live[0];
        status.textContent = s.vad_short
          ? `正在接收自动短句：${s.received} 字节（说完静音约 ${audioSilenceMs} ms 自动提交）`
          : `正在接收固定录音：${Math.round(100 * s.received / s.total)}%（${s.received}/${s.total} 字节）`;
      } else if (last) {
        status.textContent = last.complete
          ? `${last.vad_short ? '自动短句已收齐' : '固定录音已收齐'} · ${last.dur || '—'} 秒 · ${last.received}/${last.total} 字节${['queued', 'processing'].includes(last.inject) ? ' · 正在识别…' : ''}`
          : `录音不完整，请重录：${last.err || '传输中断'}`;
        if ((last.complete || last.playable || last.received > 0) && audioSession !== (last.id || last.sess)) {
          audioSession = last.id || last.sess;
          player.src = '/api/audio/latest.wav?id=' + encodeURIComponent(last.id || last.sess);
          link.href = player.src;
        }
      } else status.textContent = '等待录音；自动监听按已保存的触发阈值和静音时长运行，也可手动录制。';
      const playable = !!(last && (last.complete || last.playable || last.received > 0));
      player.classList.toggle('hidden', !playable);
      link.classList.toggle('hidden', !playable);
      D.$('#audio-transcript').textContent = !last ? '尚无识别结果。对着板子说一句完整的话即可。'
        : ['queued', 'processing'].includes(last.inject) ? '录音已保存，正在识别文字。首次使用需加载模型，请稍候。'
        : last.text ? '已识别：' + last.text
        : asrFailureKind(last) === 'dependency' ? '音频已收齐并保存，但这条录音识别时缺少云端识别组件，没有生成文字；可先试听。'
        : asrFailureKind(last) === 'service' ? '音频已收齐并保存，但云端识别处理失败；可先试听并查看错误。'
        : last.complete ? '本次没有识别到文字。请先试听，确认能听清说话声。' : '录音不完整，本次没有进行识别。';
      if (last && last.err) D.$('#audio-transcript').textContent += ' 提示：' + last.err;
      if (j.sending || (last && last.inject === 'sending')) D.$('#audio-transcript').textContent += ' 正在发送给机器人，请等待处理结果。';
      else if (last && ['manual', 'auto'].includes(last.inject)) D.$('#audio-transcript').textContent += ' 已进入机器人指令队列，等待设备领取执行。';
      /* ★ 2026-09-23：模式由单向"切成手动"改成**自动⇄手动两态**。
       *   默认值现在也是 auto（`audio_state.json`），所以老的单向按钮会出现
       *   "当前已是手动、按钮仍写切换为手动"的怪状态。按钮文案一律描述**点了会发生什么**。 */
      audioMode = String(j.mode || '');
      D.$('#audio-mode-label').textContent = j.mode === 'auto'
        ? '当前为自动发送模式：识别完自动交给 AI，回复也自动上屏，不用点确认。'
        : '当前为手动模式：识别完挂在这里等你点「确认发送」。';
      const mbtn = D.$('#audio-manual');
      if (mbtn) {
        mbtn.textContent = j.mode === 'auto' ? '切换为手动确认' : '切换为自动发送';
        mbtn.title = j.mode === 'auto'
          ? '改成：识别完挂起，等你点确认再发给机器人'
          : '改成：识别完自动发给机器人，回复自动上屏';
      }
      const pending = !!(last && ['pending', 'manual_failed', 'auto_failed'].includes(last.inject));
      const sending = !!j.sending || audioActionBusy;
      const send = D.$('#audio-send');
      send.disabled = !pending || !!live.length || sending;
      send.textContent = sending ? '正在发送…' : pending ? '发送选中录音的文字' : '暂无待发送文字';
      send.setAttribute('aria-busy', String(sending));
      send.title = sending ? '正在发送，处理结果会显示在识别文字下方' : pending ? '发送这条录音的识别文字' : '识别完成且有文字后才可发送';
      D.$('#audio-drop').disabled = !pending || sending;

      ['btn-mic-rec2', 'btn-mic-recN'].forEach(id => { const button=D.$('#' + id); if(button) button.disabled = !!live.length; });
    } catch (err) {
      if (status) status.textContent = '读取录音状态失败，请确认本地服务在线。';
    } finally { audioStatusBusy = false; }
  }

  function bindAudioResults() {
    [['audio-send', '/api/audio/send', {}], ['audio-drop', '/api/audio/drop', {}]
    ].forEach(([id, url, body]) => {
      D.$('#' + id).addEventListener('click', async () => {
        if (audioActionBusy) return;
        const button = D.$('#' + id); button.disabled = true; audioActionBusy = true;
        try {
          const r = await D.postJSON(url, Object.assign({}, body, { id: audioCurrentRecord && audioCurrentRecord.id }));
          if (!r.ok) throw new Error(r.err || r.error || r.detail || '操作失败');
          remoteLog(id === 'audio-send' ? '识别文字已提交，正在处理。' : '录音操作已完成。');
        }
        catch (err) { D.$('#audio-transcript').textContent = '操作失败：' + err; remoteLog('录音操作失败：' + err); }
        finally { audioActionBusy = false; button.disabled = false; refreshAudio(); }
      });
    });

    /* 模式切换：按**当前**模式决定切到哪边（不是写死的 manual）。
     * ★ 服务端 set_mode 会把选择写进 audio_state.json ⇒ 重启后仍生效。 */
    const mbtn = D.$('#audio-manual');
    if (mbtn) {
      mbtn.addEventListener('click', async () => {
        if (audioActionBusy) return;
        const target = (audioMode === 'auto') ? 'manual' : 'auto';
        audioActionBusy = true; mbtn.disabled = true;
        try {
          const r = await D.postJSON('/api/audio/mode', { mode: target });
          if (!r.ok) throw new Error(r.err || r.error || r.detail || '操作失败');
          remoteLog('音频模式已切到 ' + (r.mode || target)
            + (target === 'auto' ? '（识别完自动发给机器人）' : '（识别完等你确认）'));
        } catch (err) {
          D.$('#audio-transcript').textContent = '切换模式失败：' + err;
          remoteLog('切换模式失败：' + err);
        } finally { audioActionBusy = false; mbtn.disabled = false; refreshAudio(); }
      });
    }
  }

  let initialized = false;
  function init() {
    if (initialized) return; initialized = true;
    if (window.MOTION_TIMELINE) window.MOTION_TIMELINE.init();
    renderServoTestPanel();
    renderQuickButtons();
    renderSensorPresets();
    bindAlert();
    bindTestSend();
    bindRemote();
    bindAudioResults();
    bindDeviceControls();
    refreshAudio();
    refreshDevices();

    // 设备状态刷得比页面轮询快一点；不在测试页时直接跳过，省请求
    setInterval(function () {
      if (['test','audio'].includes(D.currentTab())) { refreshDevices(); refreshAudio(); refreshControls(); }
    }, 1000);
  }

  function refresh() {
    renderServoTestPanel();
    refreshAudio();
    return Promise.all([
      refreshDevices(),
      window.DEVICE_SETTINGS_UI
        ? window.DEVICE_SETTINGS_UI.init().then(function () { return window.DEVICE_SETTINGS_UI.refresh(); })
        : Promise.resolve()
    ]);
  }

  window.PAGES = window.PAGES || {};
  window.PAGES.test = { title: '测试', init: init, refresh: refresh };
  window.PAGES.audio = {title:'录音与识别',init,refresh:()=>Promise.all([refreshAudio(),refreshDevices(),refreshControls()])};

  // 别的页面也可能要写日志（比如以后把舵机指令挪走），留个口子
  D.remoteLogSafe = remoteLog;
})();
