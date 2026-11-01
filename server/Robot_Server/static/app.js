// ============================================================
// 智能家居 AI 中转站 · 前端核心（app.js）
// ------------------------------------------------------------
// 这里只放"每个页面都要用的东西"：
//   · 工具函数（$ / getJSON / postJSON / buildTable / escapeHtml / 决策字段格式化）
//   · 页签路由 + 懒加载调度
//   · 轮询调度
//   · 新增模型弹窗（弹窗在页签外，属于全局）
//   · window.DASH_API 桥（各 page-*.js 通过它取工具）
//
// 各页自己的渲染逻辑在 page-*.js 里。加载顺序必须是：
//   ui-charts.js → app.js → page-*.js
//
// ⚠️ 为什么要显式挂 window.DASH_API：
//   顶层 `function foo(){}` 会挂到 window，但 `const bar = ...` **不会**。
//   以前踩过这个坑（escapeHtml 是 const，页面文件里就是 undefined）。
//   所以下面不靠隐式提升，统一从 DASH_API 取。
// ============================================================

const $ = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.prototype.slice.call(document.querySelectorAll(sel));

const DATA_PER_PAGE = 10;              // 实时数据流每页10条
const DECISION_PER_PAGE = 20;          // 决策历史每页20条

/* ---------- 页签表 ---------- */
// showTab / 路由都按这张表走。加页签只改这里 + 加一个 pane + 一个 page-*.js。
const TABS = [
  {id:'overview',name:'陪伴首页',icon:'⌂'},
  {id:'audio',name:'录音与识别',icon:'♩'},
  {id:'presence',name:'人员相机',icon:'◎'},
  {id:'env',name:'环境记录',icon:'⌁'},
  {id:'trainer',name:'动作与姿态',icon:'◇'},
  {id:'test',name:'设备测试',icon:'⚒'},
  {id:'ota',name:'固件更新',icon:'↥'},
  {id:'interact',name:'互动记录',icon:'♧',hidden:true},
  {id:'anomaly',name:'异常记录',icon:'◇',hidden:true},
];

let currentTab = 'overview';
const _tabInited = {};                 // 页签首次显示才 init（Chart.js 不能在 display:none 里建图）

/* ---------- 通用工具 ---------- */

async function getJSON(url) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 10000);
  let r;
  try { r = await fetch(url, {signal: controller.signal}); }
  finally { clearTimeout(timeout); }
  if (r.status === 401) {
    window.location.href = '/login';     // 会话过期 → 跳回登录页
    throw new Error('未登录');
  }
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}

async function postJSON(url, body) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), url.startsWith('/api/audio/') ? 15000 : 90000);
  let r;
  try { r = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal: controller.signal,
  }); } finally { clearTimeout(timeout); }
  if (r.status === 401) { window.location.href = '/login'; throw new Error('未登录'); }
  return r.json();
}

function timeShort(full) {
  return String(full || '').slice(11, 16);
}

function parseSensor(s) {
  const t = String(s).match(/温度:\s*([\d.+-]+)/);
  const h = String(s).match(/湿度:\s*([\d.+-]+)/);
  return {
    temp: t ? parseFloat(t[1]) : null,
    humi: h ? parseFloat(h[1]) : null,
  };
}

function escapeHtml(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

/** 数字显示：null/undefined 统一显示 —，别显示 "null" */
function fmtNum(v, digits) {
  if (v === null || v === undefined || v === '') return '—';
  const n = Number(v);
  if (!isFinite(n)) return '—';
  return (digits === undefined) ? String(n) : n.toFixed(digits);
}

/** 秒 → "1小时23分" 这种人看的写法 */
function fmtSec(sec) {
  const s = Math.max(0, Math.round(Number(sec) || 0));
  if (s < 60) return s + ' 秒';
  const m = Math.floor(s / 60);
  if (m < 60) return m + ' 分' + (s % 60 ? (s % 60) + ' 秒' : '');
  const h = Math.floor(m / 60);
  return h + ' 小时' + (m % 60 ? (m % 60) + ' 分' : '');
}

/** 现在时刻 "HH:MM:SS"（本地） */
function clockStr() {
  const d = new Date();
  const p = (n) => String(n).padStart(2, '0');
  return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
}

/* ---------- 表格（用 textContent 防注入） ---------- */

const _tableSig = {};                  // 选择器 → 上次内容的指纹

function buildTable(sel, headers, rows) {
  const tbl = $(sel);
  if (!tbl) return;

  // 内容没变就整块跳过：5 秒一次的重建既会闪、又会把滚动位置顶回顶部
  const sig = JSON.stringify([headers, rows]);
  if (_tableSig[sel] === sig) return;
  _tableSig[sel] = sig;

  // 表格外面那层 .table-wrap 才是滚动容器；重建期间内容短暂为空，
  // scrollTop 会被浏览器夹到 0，重建完得手动还原，否则每 5 秒跳回顶部。
  const wrap = tbl.closest('.table-wrap');
  const keepTop = wrap ? wrap.scrollTop : 0;

  tbl.innerHTML = '';
  const thead = document.createElement('thead');
  const trh = document.createElement('tr');
  headers.forEach((h) => {
    const th = document.createElement('th');
    th.textContent = h;
    trh.appendChild(th);
  });
  thead.appendChild(trh);
  tbl.appendChild(thead);

  const tbody = document.createElement('tbody');
  rows.forEach((r) => {
    const tr = document.createElement('tr');
    r.forEach((c) => {
      const td = document.createElement('td');
      if (c instanceof Node) td.appendChild(c);
      else td.textContent = c == null ? '' : String(c);
      tr.appendChild(td);
    });
    tbody.appendChild(tr);
  });
  tbl.appendChild(tbody);

  if (wrap) wrap.scrollTop = keepTop;
}

/** 造一个等级徽章节点（表格里要用富文本，又不能拼 innerHTML 被注入） */
function sevBadge(sev) {
  const s = String(sev || 'info');
  const span = document.createElement('span');
  span.className = 'sev sev-' + (['info', 'warn', 'error'].indexOf(s) >= 0 ? s : 'info');
  span.textContent = s;
  return span;
}

function renderPager(sel, page, pages, onGo) {
  const box = $(sel);
  if (!box) return;
  const prev = page > 1;
  const next = page < pages;
  box.innerHTML =
    `<button class="pg" data-go="${page - 1}" ${prev ? '' : 'disabled'}>‹ 上一页</button>` +
    `<span class="pg-info">第 ${page} / ${pages} 页</span>` +
    `<button class="pg" data-go="${page + 1}" ${next ? '' : 'disabled'}>下一页 ›</button>`;
  box.querySelectorAll('button.pg').forEach((b) => {
    b.addEventListener('click', () => {
      if (!b.disabled) onGo(parseInt(b.dataset.go, 10));
    });
  });
}

/* ---------- 决策记录格式化（互动页 + 总览页都用） ---------- */

// 决策表里不当作"扩展字段"的标准键
/* `voice` / `tick` 也列进来：它们是 app.py 明确写的来源标记，互动页会把它们画成
 * 🎤 / ⏰ 角标，不该再当"未知扩展字段"重复显示一列。 */
const DECISION_KNOWN_KEYS = ['time', 'provider', 'model', 'key', 'sensor', 'user',
  'action', 'value', 'message', 'text', 'moves', 'timeline', 'raw', 'voice', 'tick'];

function extrasString(d) {
  const parts = [];
  Object.keys(d).forEach((k) => {
    if (DECISION_KNOWN_KEYS.includes(k)) return;
    const v = d[k];
    if (v === '' || v == null) return;
    parts.push(k + '=' + v);
  });
  return parts.join(' · ');
}

// 动作概要：新记录显示 moves[]，旧记录（无 moves）回退 action/value
function movesSummary(d) {
  if (d.timeline) {
    if (d.timeline.stop) return '停止时间任务';
    const tl = Array.isArray(d.timeline) ? { steps: d.timeline } : d.timeline;
    return `时间序列 ${(tl.steps || []).length} 步，重复 ${tl.repeat || 1} 次${tl.duration_ms ? `，每轮 ${tl.duration_ms / 1000} 秒` : ''}`;
  }
  if (Array.isArray(d.moves)) {
    if (!d.moves.length) return '(无动作)';
    return d.moves.map((m) => {
      const t = m.type || '';
      if (t === 'servo')   return `servo(${m.id}→${m.angle}°)`;
      if (t === 'gesture') return `gesture(${m.name})`;
      if (t === 'expression') return `expression(${m.face})`;
      const kv = Object.entries(m).filter(([k]) => k !== 'type')
        .map(([k, v]) => `${k}=${v}`).join(',');
      return `${t}${kv ? '(' + kv + ')' : ''}`;
    }).join(' · ');
  }
  if (d.action) return `${d.action}${(d.value !== '' && d.value != null) ? '=' + d.value : ''}`;
  return '—';
}

// 与 ESP parseAIResponse 拆帧规则一致：moves[] 逐条拆 @MOVE，屏幕文字取 text 优先、无则 message
function framesPreview(d) {
  const lines = [];
  if (d.timeline) {
    if (d.timeline.stop) return '@TIMELINE:STOP';
    const tl = Array.isArray(d.timeline) ? { steps: d.timeline } : d.timeline;
    return `F407 时间序列，重复 ${tl.repeat || 1} 次\n` +
      (tl.steps || []).map(s => `${s.at_ms} ms：${movesSummary({ moves: [s.move] })}`).join('\n') +
      '\n整段以二进制帧下发，舵机限位和手势时间由服务器校验。';
  }
  if (Array.isArray(d.moves)) {
    d.moves.forEach((m) => {
      if (!m || typeof m !== 'object') return;
      const t = m.type || '';
      if (!t) return;
      const params = [];
      if (t === 'servo')   { params.push(m.id, m.angle); }
      else if (t === 'gesture')    { params.push(m.name); }
      else if (t === 'expression') { params.push(m.face); }
      else {
        Object.entries(m).forEach(([k, v]) => {
          if (k !== 'type' && v != null && v !== '') params.push(v);
        });
      }
      lines.push('@MOVE:' + t + (params.length ? ',' + params.join(',') : ''));
    });
  } else if (d.action) {
    const v = (d.value !== '' && d.value != null) ? [d.value] : [];
    lines.push('@MOVE:' + d.action + (v.length ? ',' + v.join(',') : ''));
  }
  const screen = ((d.text || '').toString().trim()) || ((d.message || '').toString().trim());
  if (screen) lines.push('@TEXT:' + screen);
  return lines.length ? lines.join('\n') : '（没有可转发的帧）';
}

function decisionMeta(d) {
  const parts = [];
  const s = movesSummary(d);
  if (s && s !== '—') parts.push(s);
  if (d.text) parts.push('屏幕: ' + d.text);
  return parts.join(' · ');
}

/* ---------- 新增模型弹窗（在页签外，属于全局） ---------- */

function openModelModal() {
  const m = $('#modal-add');
  if (!m) return;
  m.classList.remove('hidden');
  $('#m-name').focus();
}
function closeModelModal() {
  const m = $('#modal-add');
  if (m) m.classList.add('hidden');
}

/* ---------- 状态指示（顶栏） ---------- */

function markUp() {
  const el = $('#st-refresh');
  if (el) el.textContent = '运行中 · ' + clockStr();
}
function markDown() {
  const el = $('#st-refresh');
  if (el) el.textContent = '连接中断';
}

/* ---------- 页签路由 ---------- */

function validTab(id) {
  return id === 'pose' || id === 'settings' || TABS.some((t) => t.id === id);
}

function renderTabs() {
  const bar = $('#projbar');
  if (!bar) return;
  bar.innerHTML = TABS.filter(t => !t.hidden).map((t) =>
    `<button type="button" class="proj-tab${t.id === currentTab ? ' active' : ''}" data-tab="${t.id}"><span class="nav-icon" aria-hidden="true">${t.icon}</span><span>${t.name}</span></button>`
  ).join('');
}

/**
 * 切页签。用 display 切换，**不**动 location.href（那会整页重载）。
 * @param opts.hash=false 用于"从 hash 同步过来"，避免自己又写一次 hash
 */
function showTab(id, opts) {
  if (id === 'pose') { id = 'trainer'; opts = {hash:true}; }
  const parameters = id === 'settings';
  if (parameters) id = 'test';
  if (!validTab(id)) id = 'overview';
  currentTab = id;
  const title = $('#current-page-title');
  if (title) title.textContent = TABS.find((t) => t.id === id).name;
  opts = opts || {};

  $$('#projbar .proj-tab').forEach((b) => {
    b.classList.toggle('active', b.dataset.tab === id);
    if (b.dataset.tab === id) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  });
  // pane 的 id 是 pane-<tab>，故意不等于 <tab>，否则地址栏 #env 会被当锚点、点页签时页面猛跳
  $$('.tabpane').forEach((p) => {
    p.classList.toggle('active', p.dataset.pane === id);
  });
  window.dispatchEvent(new CustomEvent('tabchange', {detail: {tab: id}}));

  if (opts.hash !== false) {
    try { history.replaceState(null, '', '#' + id); }
    catch (e) { location.hash = id; }
  }

  const page = window.PAGES && window.PAGES[id];
  if (page) {
    if (!_tabInited[id]) {
      // 首次显示才初始化 —— 必须的：Chart.js 在 display:none 的容器里量到 0 尺寸
      _tabInited[id] = true;
      try {
        if (page.init) page.init();
      } catch (e) {
        console.error('[页签 ' + id + '] 初始化失败', e);
      }
    }
    refreshTab(id).then(markUp).catch((e) => {
      markDown();
      console.error('页签数据读取失败', e);
    });
  } else {
    console.warn('[页签 ' + id + '] 没有对应的 page-*.js，只有空壳');
  }

  if (parameters) {
    const target = $('#test-parameters');
    requestAnimationFrame(() => target && target.scrollIntoView({ behavior: 'smooth', block: 'start' }));
    try { history.replaceState(null, '', '#test'); } catch (e) {}
  }

  // 切回来时把隐藏期间量错尺寸的图重新量一遍
  if (window.UIChart) window.UIChart.resizeAll();
}

/** 只刷新某个页签。轮询和切页都走它 */
function refreshTab(id) {
  const page = window.PAGES && window.PAGES[id];
  if (!page || !page.refresh) return;
  return Promise.resolve()
    .then(() => page.refresh())
    .catch((e) => { throw e; });
}

/** 轮询入口：**只刷当前可见的页签**，别让后台把 7 个页面全拉一遍 */
function refreshAll() {
  const page = window.PAGES && window.PAGES[currentTab];
  if (!page || !page.refresh) { markUp(); return; }
  Promise.resolve()
    .then(() => page.refresh())
    .then(() => { if (currentTab === (window.PAGES && window.DASH_API ? window.DASH_API.currentTab() : currentTab)) markUp(); })
    .catch((e) => { markDown(); console.error('刷新失败', e); });
}

/* ---------- 启动 ---------- */

window.addEventListener('DOMContentLoaded', () => {
  const bar = $('#projbar');
  if (bar) {
    bar.addEventListener('click', (e) => {
      const b = e.target.closest('.proj-tab');
      if (!b || !b.dataset.tab) return;
      showTab(b.dataset.tab);
    });
  }

  // 弹窗
  const mCancel = $('#m-cancel');
  if (mCancel) mCancel.addEventListener('click', closeModelModal);
  const mask = $('#modal-add');
  if (mask) {
    mask.addEventListener('click', (e) => {
      if (e.target === e.currentTarget) closeModelModal();
    });
  }
  const mSave = $('#m-save');
  if (mSave) {
    mSave.addEventListener('click', async () => {
      const name = $('#m-name').value.trim();
      const model = $('#m-model').value.trim();
      const key = $('#m-key').value.trim();
      const base = $('#m-base').value.trim();
      if (!name || !model || !key) { alert('显示名称 / 模型名称 / API Key 都要填'); return; }
      const j = await postJSON('/api/provider/add', { name, model, key, base_url: base });
      if (j.error) { alert(j.error); return; }
      closeModelModal();
      $('#m-name').value = $('#m-model').value = $('#m-key').value = $('#m-base').value = '';
      refreshAll();
      // 总览页的模型条也得跟着更新
      const ov = window.PAGES && window.PAGES.overview;
      if (ov && ov.refresh && currentTab !== 'overview') ov.refresh().catch(() => {});
    });
  }

  const openBtn = $('#btn-open-model-modal');
  if (openBtn) openBtn.addEventListener('click', openModelModal);

  if (typeof Chart === 'undefined') {
    console.warn('Chart.js 未加载（vendor/chart.umd.min.js 没找到？）——图表不可用，表格仍可用');
  }

  renderTabs();
  const start = (location.hash || '').replace(/^#/, '');
  // 首次进来也要把 hash 写上：地址栏和页签必须一致，否则一刷新就退回总览，
  // 收藏/发给别人也带不上页签。地址栏是 #乱七八糟 时这里会顺手改成 #overview。
  showTab(validTab(start) ? start : 'overview');

  // hash 回退/前进也认（用户按浏览器后退时）
  window.addEventListener('hashchange', () => {
    const id = (location.hash || '').replace(/^#/, '');
    if (validTab(id) && id !== currentTab) showTab(id, { hash: false });
  });

  setInterval(refreshAll, 5000);
});

/* ---------- 共享面（各 page-*.js 用这个，别依赖隐式全局） ---------- */

window.DASH_API = {
  // DOM / 请求
  $: $, $$: $$, getJSON: getJSON, postJSON: postJSON,
  escapeHtml: escapeHtml, buildTable: buildTable, renderPager: renderPager,
  sevBadge: sevBadge,
  // 格式化
  timeShort: timeShort, parseSensor: parseSensor,
  fmtNum: fmtNum, fmtSec: fmtSec, clockStr: clockStr,
  movesSummary: movesSummary, extrasString: extrasString,
  framesPreview: framesPreview, decisionMeta: decisionMeta,
  DECISION_KNOWN_KEYS: DECISION_KNOWN_KEYS,
  // 图表（实现在 ui-charts.js）
  mkLineChart: (c, l, s, o) => window.UIChart.mkLineChart(c, l, s, o),
  mkBarChart:  (c, l, s, o) => window.UIChart.mkBarChart(c, l, s, o),
  mkDoughnut:  (c, i, o)    => window.UIChart.mkDoughnut(c, i, o),
  setChartData: (c, l, s)   => window.UIChart.setChartData(c, l, s),
  setDoughnutData: (c, i)   => window.UIChart.setDoughnutData(c, i),
  resizeCharts: ()          => window.UIChart.resizeAll(),
  destroyChart: (c)         => window.UIChart.destroy(c),
  chartColors: ()           => window.UIChart.chartColors(),
  paletteColor: (i)         => window.UIChart.paletteColor(i),
  hexA: (c, a)              => window.UIChart.hexA(c, a),
  // 页签 / 状态
  showTab: showTab,
  currentTab: () => currentTab,
  refreshAll: refreshAll,
  markUp: markUp, markDown: markDown,
  // 弹窗
  openModelModal: openModelModal, closeModelModal: closeModelModal,
  // 常量
  DATA_PER_PAGE: DATA_PER_PAGE, DECISION_PER_PAGE: DECISION_PER_PAGE,
  TABS: TABS,
};

window.DASH = window.DASH_API;
window.PAGES = window.PAGES || {};      // 各 page-*.js 往这里注册自己
