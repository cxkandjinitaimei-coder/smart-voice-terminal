// ============================================================
// 调试页（page-debug.js）—— AI 以「项目开发伙伴」身份对话
// ------------------------------------------------------------
// ★ 这一页和后端 /api/debug_chat 是**独立的一条通路**，别并到互动页去：
//   互动页走 /api/data，带决策信封 + decision_log.txt 的多轮记忆，而那份
//   记忆里全是「成品机器人」的腔调 —— 模型抄上文的腔调远比抄 persona 强，
//   所以那边无论怎么要求，它都会把自己带入成品项目里（用户踩过这个坑）。
//   这里 system 只由 debug_chat.persona + project_brief 组成，历史读写走
//   debug_chat_log.txt，两边互不污染。
//
// ★ refresh() 会被 5 秒轮询反复调（app.js 的 refreshAll 只刷当前页签）：
//   必须**幂等** —— 靠"内容指纹"判断，没变就一个节点都不动。直接重画会把
//   用户读到一半的滚动位置冲掉。
// ============================================================
(function () {
  'use strict';
  var D = window.DASH_API;

  var lastFp = null;    // 已渲染内容的指纹（null = 强制下次重画）
  var sending = false;  // 提问在飞的时候不许轮询重画（否则刚发的气泡被抹掉）
  var busyNew = false;  // 「新对话」在飞的时候同理

  function boxEl() { return D.$('#debug-box'); }

  function showResult(head, ok, body) {
    var el = D.$('#debug-result');
    if (!el) return;
    el.className = 'result ' + (ok ? 'ok' : 'err');
    el.innerHTML = '<div class="head">' + head + '</div>' +
      (body ? '<pre>' + D.escapeHtml(body) + '</pre>' : '');
    el.classList.remove('hidden');
  }

  function hideResult() {
    var el = D.$('#debug-result');
    if (el) el.classList.add('hidden');
  }

  function addBubble(role, text, meta) {
    var b = boxEl();
    if (!b) return;
    var row = document.createElement('div');
    row.className = 'bubble-row ' + role;
    var bub = document.createElement('div');
    // 开发伙伴的回复是多行 Markdown ⇒ 靠 .pre 的 pre-wrap 保住换行。
    // ❌ 别改成 innerHTML：没装 Markdown 库，而且那是 XSS 口子。
    bub.className = 'bubble pre';
    bub.textContent = text;
    if (meta) {
      var tag = document.createElement('div');
      tag.className = 'bubble-meta';
      tag.textContent = meta;
      bub.appendChild(tag);
    }
    row.appendChild(bub);
    b.appendChild(row);
    b.scrollTop = b.scrollHeight;
  }

  function render(turns, enabled) {
    var b = boxEl();
    if (!b) return;
    b.innerHTML = '';
    b.scrollTop = 0;
    if (enabled === false) {
      addBubble('ai', '（config.json 里 debug_chat.enabled = false，调试模式已关掉）');
      return;
    }
    if (!turns.length) {
      addBubble('ai', '（新会话）直接问吧 —— 比如「你现在有哪些表情和动作？你觉得还缺什么？」');
      return;
    }
    turns.forEach(function (t) {
      addBubble('user', t.user || '');
      addBubble('ai', t.reply || '（没有回复）',
        ((t.time || '') + (t.provider ? '  ·  ' + t.provider : '')).trim());
    });
  }

  // 指纹 = 条数 + 每轮的 时间/提问长度/回复长度。够用且便宜。
  function fingerprint(turns) {
    var parts = [];
    turns.forEach(function (t) {
      parts.push((t.time || '') + ':' + (t.user || '').length +
                 ':' + (t.reply || '').length);
    });
    return turns.length + '|' + parts.join(',');
  }

  function refresh() {
    if (sending || busyNew) return Promise.resolve();
    return D.getJSON('/api/debug_chat/history').then(function (j) {
      if (sending || busyNew) return;   // 请求回来时用户已经点了发送
      var turns = (j && j.turns) || [];
      var fp = fingerprint(turns);
      if (fp === lastFp) return;        // ★ 幂等：没变就一个节点都不动
      lastFp = fp;
      render(turns, j && j.enabled);
    });
  }

  function sendChat() {
    var input = D.$('#debug-input');
    var btn = D.$('#btn-debug-send');
    if (!input || sending || busyNew) return;
    var text = input.value.trim();
    if (!text) return;

    input.value = '';
    sending = true;
    addBubble('user', text);            // 先乐观画上，等 refresh 用服务端版本重画
    hideResult();
    if (btn) { btn.disabled = true; btn.textContent = '思考中…'; }

    D.postJSON('/api/debug_chat', { message: text })
      .then(function (j) {
        // ⚠️ D.postJSON 只在 401 时抛，其余错误码是**返回** {error:...}
        if (j && j.error) throw new Error(j.error);
      })
      .catch(function (e) {
        showResult('✘ 提问失败', false,
                   String(e && e.message ? e.message : e));
      })
      .then(function () {
        sending = false;
        if (btn) { btn.disabled = false; btn.textContent = '发送'; }
        // 单一事实来源：清指纹再拉一次，成功/失败都按服务端为准重画
        lastFp = null;
        return refresh();
      });
  }

  function newConversation() {
    if (sending || busyNew) return;
    busyNew = true;
    hideResult();
    D.postJSON('/api/debug_chat/new', {})
      .then(function (j) {
        if (j && j.error) throw new Error(j.error);
        var f = (j && j.archived) || '';
        showResult('✔ 调试会话已归档' +
                   (f ? ' → <code>_debug_archive\\' + D.escapeHtml(f) + '</code>' : '（本来就是空的）'),
                   true);
      })
      .catch(function (e) {
        showResult('✘ 归档失败', false, String(e && e.message ? e.message : e));
      })
      .then(function () {
        busyNew = false;
        lastFp = null;
        return refresh();
      });
  }

  function resetMemory() {
    if (sending || busyNew) return;
    // 这件事"看起来像删除"，所以必须问一句：用户得知道日志其实没被删
    if (!window.confirm(
      '让机器人忘掉之前聊过的话？\n\n' +
      '做法是给 decision_log.txt 打一条「记忆起点」标记 ——\n' +
      '日志与统计一条都不会少，但 AI 从此不再记得之前的内容。\n\n' +
      '（本页的调试会话是另一份记忆，不受影响）')) return;
    busyNew = true;
    hideResult();
    D.postJSON('/api/memory/new', {})
      .then(function (j) {
        if (j && j.error) throw new Error(j.error);
        showResult('✔ 已让机器人忘掉之前的话（日志没删，只是 AI 不再读它之前的）', true);
      })
      .catch(function (e) {
        showResult('✘ 重置失败', false, String(e && e.message ? e.message : e));
      })
      .then(function () { busyNew = false; });
  }

  function init() {
    // ⚠️ 绑定只做一次。放进 refresh() 会让每 5 秒的轮询重复绑一遍监听。
    var btn = D.$('#btn-debug-send');
    if (btn) btn.addEventListener('click', sendChat);
    var input = D.$('#debug-input');
    if (input) {
      input.addEventListener('keydown', function (e) {
        if (e.key === 'Enter') sendChat();
      });
    }
    var nb = D.$('#btn-debug-new');
    if (nb) nb.addEventListener('click', newConversation);
    var mb = D.$('#btn-memory-new');
    if (mb) mb.addEventListener('click', resetMemory);
  }

  window.PAGES = window.PAGES || {};
  window.PAGES.debug = { title: '调试', init: init, refresh: refresh };
})();
