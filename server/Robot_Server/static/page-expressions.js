/* Full animation catalogue for the current F407 face_anim.c; no command on load. */
(function () {
  'use strict';
  var D = window.DASH_API;
  var clips = [
    ['blink', '眨眼待机', '◡'], ['shy', '害羞', '☺'], ['happy', '开心', '☀'],
    ['glasses', '戴眼镜', '⌐■'], ['angry', '生气', '≧'], ['funny', '搞怪', '✦'],
    ['wink', '眨一只眼', '◕'], ['pout', '嘟嘴', 'ω'], ['laugh', '大笑', '⌣'],
    ['starry', '星星眼', '★'], ['right', '看右边', '→'], ['sleep', '睡觉', '☾'],
    ['doubt', '疑惑', '？'], ['look', '左右看看', '↔'], ['smile', '微笑', '⌒'],
    ['surprise', '惊讶', '！'], ['think', '思考', '…']
  ];
  var busy = false, loading = false, initialized = false, registeredKey = '', selected = '';
  var registered = [];

  function result(text) { D.$('#expression-result').textContent = text; }
  function syncButtons() {
    document.querySelectorAll('#expression-grid button, #expression-registered button, #expression-send').forEach(function (b) {
      b.disabled = busy;
      if (b.dataset.expression) b.setAttribute('aria-pressed', String(b.dataset.expression === selected));
    });
  }
  async function send(id, name) {
    if (busy) return;
    if (!/^[a-z0-9_]{1,24}$/i.test(id)) {
      result('表情标识只能包含 1～24 个英文字母、数字或下划线。'); return;
    }
    busy = true; syncButtons();
    try {
      var r = await D.postJSON('/api/command', {
        device_id: 'esp-bridge', raw: '@MOVE:expression,' + id.toLowerCase() + '#', note: 'expression_test:' + id
      });
      if (!r.ok || r.error) throw new Error(r.error || '未能入队');
      selected = id;
      result('“' + name + '”已入队（队列 ' + r.queue_len + '），等待设备执行。请观察屏幕确认表情。');
    } catch (e) { result('表情发送失败：' + e.message); }
    finally { busy = false; syncButtons(); }
  }
  function button(id, name, icon) {
    var b = document.createElement('button'); b.type = 'button';
    b.dataset.expression = id; b.setAttribute('aria-pressed', 'false');
    var symbol = document.createElement('span'); symbol.className = 'expression-icon'; symbol.textContent = icon || '☺'; symbol.setAttribute('aria-hidden', 'true');
    var label = document.createElement('strong'); label.textContent = name;
    var code = document.createElement('code'); code.textContent = id;
    b.append(symbol, label, code); b.title = '@MOVE:expression,' + id + '#';
    b.addEventListener('click', function () { send(id, name); });
    return b;
  }
  async function refresh() {
    if (loading) return;
    loading = true;
    try {
      var r = await D.getJSON('/api/actions?device_id=esp-bridge');
      if (r.error) throw new Error(r.error);
      // Keep the saved file mapping for custom static BMPs; never overwrite the profile.
      var items = (r.expressions || []).filter(function (f) { return !clips.some(function (c) { return c[0] === f.id; }); });
      var key = JSON.stringify(items);
      registered = items;
      if (key !== registeredKey) {
        registeredKey = key;
        var box = D.$('#expression-registered'); box.replaceChildren();
        items.forEach(function (f) {
          var wire = String(f.file || '').replace(/\.bmp$/i, '').toLowerCase();
          if (/^[a-z0-9_]{1,24}$/.test(wire)) box.appendChild(button(wire, f.name || f.id));
        });
        syncButtons();
      }
      D.$('#expression-count').textContent = '17 组动画 · ' + registered.length + ' 个其他登记项';
    } catch (e) { result('17 组动画按钮可用；登记表情加载失败：' + e.message); }
    finally { loading = false; }
  }
  function init() {
    if (initialized) return; initialized = true;
    var box = D.$('#expression-grid');
    clips.forEach(function (c) { box.appendChild(button(c[0], c[1], c[2])); });
    D.$('#expressions-refresh').addEventListener('click', refresh);
    D.$('#expression-send').addEventListener('click', function () {
      var id = D.$('#expression-custom').value.trim(); send(id, id);
    });
    document.querySelectorAll('[data-test-jump]').forEach(function (b) {
      b.addEventListener('click', function () { document.getElementById(b.dataset.testJump).scrollIntoView({ behavior: 'smooth', block: 'start' }); });
    });
    refresh();
  }
  var page = window.PAGES.test, originalInit = page.init, originalRefresh = page.refresh;
  page.init = function () { originalInit(); init(); };
  page.refresh = function () { refresh(); return originalRefresh(); };
})();
