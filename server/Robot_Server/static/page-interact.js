/**
 * @file page-interact.js
 * 用途与使用：AI 对话、动作库、三轴姿态预览及交互投递；读取管理员档案，入队不等于执行完成。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
// ============================================================
// 互动页（page-interact.js）—— 对话（打字 + 语音）+ 需求统计 + 决策历史
// ------------------------------------------------------------
// 统计来自 relay.db 的 interactions 表；决策历史表仍读 decision_log.txt
// （那是主链路的流水账，格式没动）。
//
//  三种互动要分清，混在一起看会误以为"AI 很忙"：
//     real_ai   真实调用了模型（花钱的）
//     mock      测试模式（provider=='mock'，不花钱）
//     auto_tick 定时唤醒（AI 自己找话说）
//
// === 对话/互动正式上屏 ===
// 用户原话：「所有的网页端的对话、互动都要正式发送到屏幕上面」。
//
//  上屏为什么必须绕一圈（这一页最要紧的认识）：
//   浏览器 POST /api/data 的**响应体只有浏览器看得见**，ESP 看不见 ⇒
//   "云端调了 AI"和"帧到了屏上"是两件事。中间只有一条路（NAT 穿透就是它）：
//   `deliver:true` → 云端 `enqueue_command` → ESP 每秒轮询取走（响应里的 command）
//   → forwardCommandToF407 → F407。所以 sendChat() 必须带 `deliver:true`。
//
//  状态不许撒谎：F407 **不实现 @ACK**（不回执）⇒ 本页**永远不写"已显示在屏上"**，
//   只说两件可观测的事：`已入队`（云端排进队列）/ `设备已取走`（ESP 轮询领走）。
//   领走 ≠ 屏上画出来了 —— 中间还可能被 F407 的白名单丢掉。
//
//  轮询必须**幂等**（5 秒一次反复调，见 app.js refreshAll）：
//   靠"已见轮次 key 集合"增量追加，**不整表重画** —— 重画会毁掉滚动位置、
//   也会把刚发出去的本地回显冲掉（老代码用 chatReplayed 一次性保护，效果是
//   进来之后新对话永远不出现，等于把用户要的功能关掉了）。
// ============================================================
(function () {
  'use strict';
  var D = window.DASH_API;

  var chartMove = null, chartTime = null;
  var decisionPage = 1;

  /* 一次拉多少轮对话（和表格分页无关：对话流永远看最新的） */
  var CHAT_ROWS = 20;
  /* 与 page-test.js 的 REMOTE_DEVICE_ID 同值（那边也是写死的字面量） */
  var DEVICE_ID = 'esp-bridge';

  /* ---------- 幂等：已画过的轮次 ---------- */
  var seenRounds = Object.create(null);      // key -> true
  /* 本地回显的投递单号 → 气泡上的状态节点（拿到 /api/deliveries 后升级文案） */
  var pendingDids = Object.create(null);     // did -> { el, key }

  function roundKey(d) {
    return String(d && d.time || '') + '|' + String(d && d.user || '') +
      '|' + String(d && d.message || '');
  }
  /* 投递账本里认人也用 time+user（app.py::_note_delivery 存的就是这两个字段） */
  function deliverKey(d) {
    return String(d && d.time || '') + '|' + String(d && d.user || '');
  }

  function statTile(k, v) {
    return '<div class="stat"><span class="k">' + D.escapeHtml(k) +
      '</span><span class="v">' + D.escapeHtml(v) + '</span></div>';
  }

  function renderStats(it) {
    var box = D.$('#int-stats');
    if (!box) return;
    box.innerHTML =
      statTile('互动总数（24h）', String(it.total || 0)) +
      statTile('真实 AI', String(it.real_ai || 0)) +
      statTile('测试模式', String(it.mock || 0)) +
      statTile('定时唤醒', String(it.auto_tick || 0));
  }

  function renderMoveChart(it) {
    var mt = it.by_move_type || {};
    // 固定顺序，别让图例随对象键序跳来跳去
    var order = ['servo', 'gesture', 'expression'];
    var items = [];
    order.forEach(function (k) {
      if (mt[k]) items.push({ label: k, value: mt[k], color: D.paletteColor(order.indexOf(k)) });
    });
    Object.keys(mt).forEach(function (k) {
      if (order.indexOf(k) < 0) items.push({ label: k, value: mt[k], color: '#8a8f98' });
    });
    if (!chartMove) {
      chartMove = D.mkDoughnut('#chart-int-move', items, {
        empty: '还没有 AI 返回过动作\n（moves[] 统计的是最近 50 次互动）',
      });
    } else {
      D.setDoughnutData(chartMove, items);
    }
  }

  function renderTimeChart(it) {
    var b = it.buckets || [];
    var labels = b.map(function (x) { return x.label; });
    var series = [{ label: '互动数', color: '--accent', data: b.map(function (x) { return x.count; }) }];
    if (!chartTime) {
      chartTime = D.mkBarChart('#chart-int-time', labels, series, {
        legend: false, beginAtZero: true, maxTicks: 6, empty: '这段时间没有互动',
      });
    } else {
      D.setChartData(chartTime, labels, series);
    }
  }

  /* ---------- 决策历史表 ---------- */

  function renderDecisions(h) {
    var totalEl = D.$('#int-total');
    if (totalEl) totalEl.textContent = h.decision_total;
    D.buildTable('#tbl-decision', ['时间', 'AI 回复', '动作 (moves)', '扩展字段'],
      h.decisions.map(function (d) {
        return [d.time, d.message, D.movesSummary(d), D.extrasString(d) || '—'];
      }));
    D.renderPager('#pager-decision', h.dpage, h.decision_pages, function (p) {
      decisionPage = p;
      refresh();
    });
  }

  /* ---------- 对话 ---------- */

  /** 这一轮的来源。**只认明确标记，不猜**：
   *  · `tick`  —— app.py::auto_tick_once 写的（定时唤醒）
   *  · `voice` —— app.py 在 `enqueue_decision` 分支写的（PC 音频回灌 = 语音）
   *  · 其余带 user 的 —— 网页打字
   *  ❌ 别拿"user 是不是空"判语音：打字也是 user 非空。 */
  function srcTag(d) {
    if (d && d.tick) return '⏰ 定时唤醒';
    if (d && d.voice) return '🎤 语音（板子麦克风）';
    return '⌨️ 打字';
  }

  function addBubble(role, text, meta, opts) {
    var box = D.$('#chat-box');
    if (!box) return null;
    opts = opts || {};
    var row = document.createElement('div');
    row.className = 'bubble-row ' + role;
    var bub = document.createElement('div');
    bub.className = 'bubble';
    if (opts.src) {
      var src = document.createElement('div');
      src.className = 'bubble-src';
      src.textContent = opts.src;
      bub.appendChild(src);
    }
    /*  正文一律 textContent —— 大模型的回复直接进 innerHTML 等于白送一个 XSS 口子 */
    var body = document.createElement('div');
    body.textContent = text;
    bub.appendChild(body);
    if (meta) {
      var tag = document.createElement('div');
      tag.className = 'bubble-meta';
      tag.textContent = meta;
      bub.appendChild(tag);
    }
    if (opts.state) {
      var st = document.createElement('div');
      st.className = 'bubble-state ' + (opts.stateCls || 'queued');
      st.textContent = opts.state;
      st.dataset.resolved = opts.resolved ? '1' : '0';
      bub.appendChild(st);
      if (opts.did != null) {
        /* 有投递单号：后面只认 id（精确） */
        pendingDids[opts.did] = { el: st, key: opts.key || '' };
        st.dataset.key = 'did';
      } else if (opts.dkey) {
        /* 没有单号（历史流画出来的）：退而用 time+user 去账本里认 */
        row.dataset.dkey = opts.dkey;
      }
    }
    row.appendChild(bub);
    box.appendChild(row);
    box.scrollTop = box.scrollHeight;
    return st || null;
  }

  function setState(el, text, cls) {
    if (!el) return;
    el.textContent = text;
    el.className = 'bubble-state ' + (cls || 'queued');
  }

  /** 增量回放：新轮次追加到**底部**，已画过的一个都不动。
   *  `/api/history.decisions` 是"最新在前"，所以先反成"旧→新"再画。 */
  function renderFeed(decisions) {
    var fresh = [];
    (decisions || []).slice().reverse().forEach(function (d) {
      var k = roundKey(d);
      if (seenRounds[k]) return;
      seenRounds[k] = true;
      fresh.push(d);
    });
    if (!fresh.length) return;
    var box = D.$('#chat-box');
    var atBottom = box
      ? (box.scrollHeight - box.scrollTop - box.clientHeight < 40) : true;
    fresh.forEach(function (d) {
      var m = (d.message || '').trim();
      /* 历史流里的轮次**没有投递单号**，先摆一个"待对账"的状态位，
       * 由 syncDeliveries() 拿 time+user 去账本里认；认不到就写明"没记录"。 */
      var st = { state: '⌛ 投递状态：对账中…', stateCls: 'queued',
                 dkey: deliverKey(d) };
      if (d.tick) {
        /* 定时唤醒的 `user` 是内部提示词，不是用户说的话 ⇒ 不画成用户气泡。
         * 它也不走 _note_delivery（auto_tick 直接 enqueue_command）⇒ 账本里没有它。 */
        /*  措辞只说确定的事：auto_tick 不走 _note_delivery ⇒ 账本里**一定没有**它，
         *    但"有没有真入队"要看帧是否为空（AI 说了话却没给帧时它不入队）
         *    ⇒ 不能写"已入队"，只能写"查不到投递记录"。 */
        if (m) addBubble('ai', m, D.decisionMeta(d),
          { src: srcTag(d), state: '— 定时唤醒不记投递账本，这里查不到下发结果',
            stateCls: 'queued', resolved: true });
        return;
      }
      var u = (d.user || '').trim();
      if (u) addBubble('user', u, '', { src: srcTag(d) });
      if (m) addBubble('ai', m, D.decisionMeta(d), st);
    });
    /* 用户正在看底部就跟到底；翻旧账时不打断 */
    if (box && atBottom) box.scrollTop = box.scrollHeight;
  }

  /* ---------- 上屏状态（诚实面）---------- */

  function stateFromDeliver(j, userText) {
    switch (j && j.deliver_state) {
      case 'queued':
        return { text: '📤 已入队（队列 ' + (j.queue_len != null ? j.queue_len : '?') + '）',
                 cls: 'queued' };
      case 'refused_offline':
        return { text: '⚠️ 未下发：设备离线', cls: 'refused' };
      case 'refused_no_ai':
        return { text: '⚠️ 未下发：没调用 AI', cls: 'refused' };
      case 'refused_empty_frame':
        return { text: '⚠️ 未下发：AI 没产生可下发内容', cls: 'refused' };
      case 'refused_queue_full':
        return { text: '⚠️ 未下发：设备前面积压了命令', cls: 'refused' };
      default:
        return { text: '⚠️ 未下发：' + (j && j.deliver_err || '未知原因'), cls: 'refused' };
    }
  }

  /** 把账本的最新状态刷到气泡上。**只画账本里真写着的东西**。 */
  function syncDeliveries() {
    return D.getJSON('/api/deliveries?limit=50').then(function (j) {
      var byId = {}, byKey = {};
      (j.deliveries || []).forEach(function (x) {
        byId[x.id] = x;
        byKey[x.time + '|' + (x.user || '')] = x;
      });
      Object.keys(pendingDids).forEach(function (did) {
        var rec = byId[did];
        var p = pendingDids[did];
        if (!rec || !p || !p.el || !p.el.isConnected) { delete pendingDids[did]; return; }
        if (rec.state === 'taken') {
          setState(p.el, '✅ 设备已取走（' + (rec.pop_at || '') + '）', 'taken');
          delete pendingDids[did];
        } else if (rec.state !== 'queued') {
          setState(p.el, '⚠️ 未下发：' + (rec.err || rec.state), 'refused');
          delete pendingDids[did];
        }
      });
      /* 从历史流里画出来的气泡（语音 / 别的页发起的）没有单号，用 time+user 认 */
      var box = D.$('#chat-box');
      if (!box) return j;
      var rows = box.querySelectorAll('.bubble-row.ai');
      Array.prototype.forEach.call(rows, function (row) {
        var st = row.querySelector('.bubble-state');
        if (!st || st.dataset.resolved === '1' || st.dataset.key) return;
        var rec = byKey[row.dataset.dkey];
        if (!rec) {
          /* 账本里没有 ⇒ **不能**停在"对账中"装作还在查。两种真原因：
           *  这一轮不是网页发起的（设备自己那条路，云端不记账）
           *  账本只有 50 条、且是进程内的（Flask 重启就空了）⇒ 滚出窗口了 */
          setState(st, '— 账本里没有这轮的记录（设备自己那条路，或账本已滚出/重启过）',
            'queued');
          st.dataset.resolved = '1';
          return;
        }
        if (rec.state === 'taken') {
          setState(st, '✅ 设备已取走（' + (rec.pop_at || '') + '）', 'taken');
          st.dataset.resolved = '1';
        } else if (rec.state !== 'queued') {
          setState(st, '⚠️ 未下发：' + (rec.err || rec.state), 'refused');
          st.dataset.resolved = '1';
        }
      });
      return j;
    }).catch(function () { /* 账本读不到就不显示，不猜 */ });
  }

  async function sendChat() {
    var input = D.$('#chat-input');
    if (!input) return;
    var text = input.value.trim();
    if (!text || D.$('#btn-chat').disabled) return;
    input.value = '';
    addBubble('user', text, '', { src: '⌨️ 打字' });

    var btn = D.$('#btn-chat');
    if (btn) { btn.disabled = true; btn.textContent = '思考中…'; }
    try {
      /*  `sensor` 发空串，**不再**去读测试页那个 #in-sensor 冒充传感器：
       *   那会往 decision_log / 统计里灌一条根本没发生过的"温度:28.5,湿度:65"。
       *   本页要的是对话，不是伪造上报（测试页自己有造异常的入口）。 */
      var j = await D.postJSON('/api/data', {
        sensor: '',
        user: text,
        mock: false,                 // 对话始终走真实 AI
        need_ai: true,
        deliver: true,               //  正式下发到屏上（走命令队列）
      });
      var st = j && j.deliver_state ? stateFromDeliver(j) : null;
      var badAi = j && j.ai_status && j.ai_status !== 'ok';
      var reply = j && j.message ? j.message : 'AI 没有返回文字回复';
      var meta = D.decisionMeta(j);
      if (badAi) meta = (meta ? meta + ' · ' : '') + '⚠ ' + (j.ai_error || '模型/解析异常');
      var el = addBubble('ai', reply, meta, {
        state: st ? st.text : '',
        stateCls: badAi ? 'refused' : (st ? st.cls : ''),
        did: j && j.deliver_id != null ? j.deliver_id : null,
        key: deliverKey(j),
      });
      /* 云端连投递都没判（老版本/异常）⇒ 说白了，别装作发了 */
      if (!st && el) setState(el, '⚠️ 未下发：云端没返回投递结果', 'refused');
      if (j && j.servo_clamped && j.servo_clamped.length) {
        addBubble('ai', '⚠️ 动作被限位夹过：' + j.servo_clamped.join('；'), '（避免舵机撞限位）');
      }
      if (j && j.timeline_error) {
        addBubble('ai', '时间动作未执行：' + j.timeline_error, '请调整时间或角度后重试');
      }
      /* 本地回显已画过 ⇒ 记进"已见"，免得 5 秒后轮询又画一遍 */
      seenRounds[roundKey({ time: j.time, user: text, message: j.message })] = true;
      D.refreshAll();
    } catch (e) {
      addBubble('ai', '请求失败: ' + e, '');
    } finally {
      if (btn) { btn.disabled = false; btn.textContent = '发送'; }
    }
  }

  /* ---------- 动作库 ---------- */
  var actionLibraryKey = '', actionLoading = false, actionBusy = false;

  function actionResult(text, ok) {
    var box = D.$('#action-library-result');
    if (!box) return;
    box.className = 'result ' + (ok ? 'ok' : 'err');
    box.textContent = text;
    box.classList.remove('hidden');
  }

  async function runAction(action, btn) {
    if (!action.ready) {
      actionResult('“' + action.name + '”不可用：' + (action.unavailable_reason || '请检查管理员参数'), false);
      return;
    }
    if (actionBusy) return;
    actionBusy = true;
    D.$('#action-library').querySelectorAll('button').forEach(function (b) { b.disabled = true; });
    try {
      var j = await D.postJSON('/api/actions', { action_id: action.id, device_id: DEVICE_ID });
      if (!j.ok || j.error) throw new Error(j.error || '动作未入队');
      var current = j.action || action;
      actionResult('“' + current.name + '”已入队（队列 ' + j.queue_len + '），等待设备执行。'
        + (current.duration_ms ? '预计 ' + (current.duration_ms / 1000).toFixed(1) + ' 秒。' : '')
        + ' ' + (current.summary || ''), true);
      D.refreshAll();
    } catch (e) {
      actionResult('❌ “' + action.name + '”未下发：' + e, false);
    } finally {
      actionBusy = false;
      D.$('#action-library').querySelectorAll('button').forEach(function (b) { b.disabled = b.dataset.ready !== '1'; });
    }
  }

  async function loadActions() {
    var box = D.$('#action-library');
    if (!box || actionLoading) return;
    actionLoading = true;
    try {
      var j = await D.getJSON('/api/actions?device_id=' + encodeURIComponent(DEVICE_ID));
      if (j.error) throw new Error(j.error);
      var key = JSON.stringify([j.actions, j.servos, j.servo_speed]);
      if (key === actionLibraryKey) return;
      actionLibraryKey = key;
      var params = D.$('#action-library-params');
      if (params) params.textContent = '当前保存的参数：' + (j.servos || []).map(function (s) {
        return '舵机' + s.id + ' ' + s.min + '~' + s.max + '°，正面' + s.front + '°，默认速度' + s.speed + '°/秒';
      }).join('；') + '。';
      box.innerHTML = '';
      (j.actions || []).forEach(function (action) {
        var btn = document.createElement('button');
        btn.type = 'button'; btn.className = 'qbtn';
        btn.textContent = (action.ready ? '▶ ' : '不可用 ') + (action.name || action.id);
        btn.title = action.unavailable_reason || action.summary || '';
        btn.dataset.actionId = action.id; btn.dataset.ready = action.ready ? '1' : '0';
        btn.disabled = !action.ready || actionBusy;
        btn.addEventListener('click', function () { runAction(action, btn); });
        box.appendChild(btn);
      });
    } catch (e) {
      actionResult('动作库加载失败：' + e, false);
    } finally { actionLoading = false; }
  }

  /* ---------- 语音：让板子自己听（现有 P3 链路）----------
   * 点一下 = 往命令队列塞一条 `@MIC:rec,N#`（与 page-test.js 白名单产出同格式，
   * 秒数在浏览器这层先夹到 1~10，和固件上限一致 —— 超了固件是**静默夹到 10**）。
   * 之后**不解析任何串口数据**（F407 的结果打在它自己的调试口上，网页看不到），
   * 只读 `/api/audio/status` 这个已经实现好的接口判"识别/提交"到哪一步了。 */
  var MIC_SEC_MIN = 1, MIC_SEC_MAX = 10;
  var voiceTimer = null, voiceGen = 0;

  function readMicSec() {
    var el = D.$('#in-chat-mic-sec');
    var n = Number(String((el && el.value) || '').trim());
    if (!isFinite(n) || n <= 0) return 0;
    var sec = Math.min(MIC_SEC_MAX, Math.max(MIC_SEC_MIN, Math.round(n)));
    if (el) el.value = String(sec);       // 夹过的值写回去，让用户看见实际会录几秒
    return sec;
  }

  function voiceHint(html) {
    var hint = D.$('#chat-mic-hint');
    if (!hint) return;
    hint.innerHTML = html;
    hint.classList.remove('hidden');
  }

  function voiceHintDone(html) {
    voiceHint(html);
    var hint = D.$('#chat-mic-hint');
    if (hint) setTimeout(function () { hint.classList.add('hidden'); }, 20000);
  }

  function injectText(last) {
    switch (last && last.inject) {
      case 'processing': return '⏳ 录音已收齐，正在转文字（首次加载模型要几秒）…';
      case 'sending':    return '⏳ 文字已识别，正在提交给 AI…';
      case 'auto':        return '✅ 已识别并自动交给 AI，回复稍后自动上屏。';
      case 'auto_failed': return '❌ 自动提交失败：' + (last.err || '（原因没回传）');
      case 'manual':
      case 'pending':     return '⏳ 现在是**手动模式**：识别到「' + (last.text || '') +
        '」，去「测试」页点「确认发送」。';
      case 'skipped':     return '⚠️ 这次没识别到文字（可能没说清 / 全是静音）。';
      default:            return '';
    }
  }

  function pollVoice(sec, baselineSess) {
    voiceGen += 1;
    if (voiceTimer) { clearTimeout(voiceTimer); voiceTimer = null; }
    var gen = voiceGen;
    var t0 = Date.now();
    var hardStop = t0 + (sec + 20) * 1000;   // 比服务端 SESSION_IDLE_SEC=30 早得多

    var tick = async function () {
      if (gen !== voiceGen) return;
      var j = null;
      try { j = await D.getJSON('/api/audio/status'); } catch (e) { /* 读不到就等下一拍 */ }
      if (gen !== voiceGen) return;
      if (j) {
        renderAudioMode(j);
        var live = j.live || [];
        var last = j.last;
        if (last && last.sess && last.sess !== baselineSess) {
          var msg = injectText(last);
          if (msg) { voiceHintDone(msg); return; }        // 这一轮有结论了，收工
        }
        if (live.length) {
          var s = live[0];
          var pct = s.total ? Math.round(100 * s.received / s.total) : 0;
          voiceHint('🎙 板子正在推流录音（' + pct + '%）—— <b>录音期间屏面暂停刷新</b>，是正常的，不是死机。');
        } else if (Date.now() - t0 > 2000) {
          voiceHint('🎙 已请求录音 ' + sec + ' 秒，等板子开始推流…（命令要等 ESP 下一次轮询取走）');
        }
      }
      if (Date.now() >= hardStop) {
        voiceHintDone('⚠️ 没等到录音结果。查一下：板子在线吗（「测试」页设备状态）？'
          + '麦克风接线 / 供电对吗？也可以去「测试」页看录音状态与波形。');
        return;
      }
      voiceTimer = setTimeout(tick, 1000);
    };
    voiceTimer = setTimeout(tick, 800);
  }

  /** 模式胶囊：语音默认全自动（audio_state.json），这里只是**显示事实**。 */
  function renderAudioMode(j) {
    var el = D.$('#chat-audio-mode');
    if (!el || !j) return;
    var auto = j.mode === 'auto';
    el.textContent = auto ? '自动短句：直接说话，静音后识别并交给 AI' : '自动短句：直接说话，识别后等你确认';
    el.className = 'remote-pill ' + (auto ? 'on' : 'off');
    el.title = auto ? '板子自动检测人声；说完停顿约 0.8 秒即可（在「测试」页可切发送方式）'
      : '板子仍自动检测人声；识别完会挂起等确认（在「测试」页可切成自动）';
  }

  function pollAudioMode() {
    return D.getJSON('/api/audio/status').then(renderAudioMode)
      .catch(function () {
        var el = D.$('#chat-audio-mode');
        if (el) { el.textContent = '语音模式：读不到状态'; el.className = 'remote-pill off'; }
      });
  }

  async function startVoice() {
    var sec = readMicSec();
    if (!sec) {
      voiceHintDone('⚠️ 先填录音秒数（' + MIC_SEC_MIN + '~' + MIC_SEC_MAX + '）。');
      return;
    }
    var btn = D.$('#btn-chat-mic');
    if (btn) btn.disabled = true;
    try {
      /* 先记下当前这一轮是哪个会话，这样才能认出"新的一轮"（不是拿旧结果充数） */
      var base = '';
      try {
        var st = await D.getJSON('/api/audio/status');
        renderAudioMode(st);
        base = (st.last && st.last.sess) || '';
      } catch (e) { /* 读不到就用空 baseline：只要拿到 last 就当新轮 */ }
      var j = await D.postJSON('/api/command', {
        device_id: DEVICE_ID,
        raw: '@MIC:rec,' + sec + '#',
        note: '互动页：让机器人听',
      });
      if (j.error) throw new Error(j.error);
      voiceHint('🎙 录音指令已入队（队列 ' + j.queue_len + '），等 ESP 取走后板子开始录 '
        + sec + ' 秒…');
      pollVoice(sec, base);
    } catch (err) {
      voiceHintDone('❌ 没能让板子开始录音：' + err);
    } finally {
      if (btn) btn.disabled = false;
    }
  }

  var conversationInited = false;
  function initConversation() {
    if (conversationInited) return;
    conversationInited = true;
    var btn = D.$('#btn-chat');
    if (btn) btn.addEventListener('click', sendChat);
    var input = D.$('#chat-input');
    if (input) {
      input.addEventListener('keydown', function (e) { if (e.key === 'Enter' && !e.isComposing) sendChat(); });
    }
    var mic = D.$('#btn-chat-mic');
    if (mic) mic.addEventListener('click', startVoice);
    var secEl = D.$('#in-chat-mic-sec');
    if (secEl) secEl.addEventListener('blur', readMicSec);
    pollAudioMode();
  }

  window.ROBOT_CHAT = {
    init: initConversation,
    refresh: function () {
      return Promise.all([
        D.getJSON('/api/history?per_page=1&page=1&dper_page=' + CHAT_ROWS + '&dpage=1'),
        pollAudioMode()
      ]).then(function (r) { renderFeed(r[0].decisions); return syncDeliveries(); });
    }
  };

  function init() {
    initConversation();
    loadActions();
  }

  function refresh() {
    var stats = D.getJSON('/api/stats/interaction?hours=24&limit=50');
    /* 对话流永远看**最新一页**；表格才跟着 decisionPage 走 */
    var chat = D.getJSON('/api/history?per_page=1&page=1&dper_page=' + CHAT_ROWS + '&dpage=1');
    var table = (decisionPage === 1)
      ? chat
      : D.getJSON('/api/history?per_page=1&page=1&dper_page=' + D.DECISION_PER_PAGE +
                  '&dpage=' + decisionPage);
    /*  顺序要紧：**先画气泡，再对账**。
     *  反过来的话，新气泡要等下一轮 5 秒才被认到；更糟的是"认不到就写死结论"那条
     *  可能把这一轮判成"账本里没有"（虽然实际几乎不会撞上，但没有理由留这个缝）。 */
    return Promise.all([stats, chat, table, pollAudioMode(), loadActions()])
      .then(function (r) {
        var it = r[0], chatH = r[1], tblH = r[2];
        if (it && !it.error) {
          renderStats(it);
          renderMoveChart(it);
          renderTimeChart(it);
        }
        if (chatH) renderFeed(chatH.decisions);
        if (tblH) renderDecisions(tblH);
        return syncDeliveries();
      })
      .then(function () { D.markUp(); });
  }

  window.PAGES = window.PAGES || {};
  window.PAGES.interact = { title: '互动', init: init, refresh: refresh };
})();
