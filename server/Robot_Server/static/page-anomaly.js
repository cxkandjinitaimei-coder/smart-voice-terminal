/**
 * @file page-anomaly.js
 * 用途与使用：阈值规则与异常记录的展示和编辑；服务端负责规则校验及冷却。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
// ============================================================
// 异常页（page-anomaly.js）—— 按类型/等级/时间统计 + 记录列表 + 当前阈值
// ------------------------------------------------------------
// 数据源是 relay.db 的 events 表，三种来源都在这张表里：
//   source='device'    设备自己报的（F407 发 @ALERT / 或者 POST 带 alert 字段）
//   source='threshold' 服务端按 config.json 的 thresholds 判定出来的
//   source='legacy'    从老 decision_log.txt 回填的（没有结构化等级）
// kind 字段区分：sensor_alert / device_alert / ...
// ============================================================
(function () {
  'use strict';
  var D = window.DASH_API;

  var hours = 24;
  var severity = '';
  var chartCode = null, chartSev = null, chartTime = null;

  function statTile(k, v) {
    return '<div class="stat"><span class="k">' + D.escapeHtml(k) +
      '</span><span class="v">' + D.escapeHtml(v) + '</span></div>';
  }

  function renderStats(a) {
    var box = D.$('#anom-stats');
    if (!box) return;
    var sev = a.by_severity || {};
    var last = (a.recent && a.recent[0]) ? a.recent[0].ts : null;
    var codes = (a.by_code || []).length;
    var nErr = sev.error || 0, nWarn = sev.warn || 0;
    box.innerHTML =
      statTile('异常总数', String(a.total || 0)) +
      statTile('error / warn', nErr + ' / ' + nWarn) +
      statTile('涉及类型', String(codes)) +
      statTile('最近一条', last ? D.timeShort(last) : '—');
  }

  function renderCodeChart(a) {
    var list = (a.by_code || []).slice(0, 10);
    var labels = list.map(function (x) { return x.code; });
    var series = [{ label: '次数', color: '--accent', data: list.map(function (x) { return x.n; }) }];
    if (!chartCode) {
      chartCode = D.mkBarChart('#chart-anom-code', labels, series, {
        indexAxis: 'y', legend: false, beginAtZero: true, maxTicks: 10,
        empty: '这段时间没有异常',
      });
    } else {
      D.setChartData(chartCode, labels, series);
    }
  }

  function renderSevChart(a) {
    var sev = a.by_severity || {};
    var order = ['error', 'warn', 'info'];
    var colorOf = { error: '--bad', warn: '#e0b400', info: '--accent' };
    var items = order
      .filter(function (k) { return sev[k]; })
      .map(function (k) { return { label: k, value: sev[k], color: colorOf[k] }; });
    Object.keys(sev).forEach(function (k) {
      if (order.indexOf(k) < 0) items.push({ label: k, value: sev[k], color: '#8a8f98' });
    });
    if (!chartSev) {
      chartSev = D.mkDoughnut('#chart-anom-sev', items, { empty: '这段时间没有异常' });
    } else {
      D.setDoughnutData(chartSev, items);
    }
  }

  function renderTimeChart(a) {
    var b = a.buckets || [];
    var labels = b.map(function (x) { return x.label; });
    var series = [{ label: '异常数', color: '--bad', data: b.map(function (x) { return x.count; }) }];
    if (!chartTime) {
      chartTime = D.mkBarChart('#chart-anom-time', labels, series, {
        legend: false, beginAtZero: true, maxTicks: 8,
        empty: '这段时间没有异常',
      });
    } else {
      D.setChartData(chartTime, labels, series);
    }
  }

  function renderTable(a) {
    var rows = (a.recent || []).map(function (r) {
      return [r.ts, r.code, D.sevBadge(r.severity), r.source || '', r.message || ''];
    });
    D.buildTable('#tbl-anom', ['时间', '类型', '等级', '来源', '说明'], rows);
  }

  function renderRules(th) {
    var box = D.$('#anom-rules');
    if (!box) return;
    var t = (th && th.thresholds) || {};
    var rules = t.rules || [];
    if (!t.enabled) {
      box.className = 'empty-box';
      box.innerHTML = '阈值判定当前是<b>关闭</b>的 —— 传感器超范围不会产生异常。去「设置」页打开。';
      return;
    }
    if (!rules.length) {
      box.className = 'empty-box';
      box.innerHTML = '还没有配置任何阈值规则。去「设置」页加一条。';
      return;
    }
    box.className = '';
    var head = '<p class="hint" style="margin:0 0 8px">共 ' + rules.length +
      ' 条 · 同一规则 ' + D.escapeHtml(t.cooldown_sec) + ' 秒内只报一次</p>';
    var body = rules.map(function (r) {
      var chg = r.on_change ? ' · <b>仅状态翻转时</b>' : '';
      var sev = '<span class="sev sev-' + (['info', 'warn', 'error'].indexOf(r.severity) >= 0 ? r.severity : 'info') +
        '">' + D.escapeHtml(r.severity) + '</span>';
      return '<div class="th-row">' +
        '<span class="th-code">' + D.escapeHtml(r.code) + '</span>' + sev +
        '<span class="th-rule">' + D.escapeHtml(r.label || '') + '：' +
        D.escapeHtml(r.field) + ' ' + D.escapeHtml(r.op) + ' ' + D.escapeHtml(r.value) + chg +
        '</span></div>';
    }).join('');
    box.innerHTML = head + body;
  }

  function init() {
    var rng = D.$('#anom-range');
    if (rng) {
      rng.addEventListener('click', function (e) {
        var btn = e.target.closest('.rtab');
        if (!btn) return;
        Array.prototype.forEach.call(rng.querySelectorAll('.rtab'), function (b) {
          b.classList.remove('active');
        });
        btn.classList.add('active');
        hours = parseInt(btn.dataset.hours, 10) || 24;
        refresh();
      });
    }
    var sevBox = D.$('#anom-sev');
    if (sevBox) {
      sevBox.addEventListener('click', function (e) {
        var btn = e.target.closest('.rtab');
        if (!btn) return;
        Array.prototype.forEach.call(sevBox.querySelectorAll('.rtab'), function (b) {
          b.classList.remove('active');
        });
        btn.classList.add('active');
        severity = btn.dataset.sev || '';
        refresh();
      });
    }
  }

  function refresh() {
    var url = '/api/stats/anomaly?hours=' + hours + '&limit=50' +
      (severity ? '&severity=' + encodeURIComponent(severity) : '');
    return Promise.all([
      D.getJSON(url),
      D.getJSON('/api/thresholds'),
    ]).then(function (r) {
      var a = r[0];
      if (a && !a.error) {
        renderStats(a);
        renderCodeChart(a);
        renderSevChart(a);
        renderTimeChart(a);
        renderTable(a);
      }
      renderRules(r[1]);
      D.markUp();
    });
  }

  window.PAGES = window.PAGES || {};
  window.PAGES.anomaly = { title: '异常', init: init, refresh: refresh };
})();
