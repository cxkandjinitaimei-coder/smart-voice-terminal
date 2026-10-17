/**
 * @file ui-charts.js
 * 用途与使用：图表绘制与时间轴聚合；分钟绘线与设备采样独立，缺失点不能补成零读数。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
// ============================================================
// 图表工具（ui-charts.js）
// ------------------------------------------------------------
// 两个目的：
//    所有颜色**现读 CSS 变量**，不写死 hex —— 换主题时图能跟着变；
//    统一处理 Chart.js 的两个已知坑：
//      · 在 display:none 的容器里建图会量到 0 尺寸（配合 maintainAspectRatio:false
//        会把画布钉死在 0 宽），所以必须懒加载 + 切回来时 resize（见 resizeAll）；
//      · 没数据时要显式画"暂无数据"，不能留一条平线假装有数据。
//
// 加载顺序：必须在 app.js 之前（app.js 用这里的 hexA / cssVar）。
// ============================================================
(function () {
  'use strict';

  /* ================= 颜色 ================= */

  /** 读 CSS 变量（带 -- 前缀），拿不到就返回空串 */
  function cssVar(name) {
    try {
      return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    } catch (e) { return ''; }
  }

  /** 统一成浏览器认得的色值。接受：
   *    '--temp' / 'var(--temp)'  → 读变量
   *    '#abc' / '#aabbcc'        → 原样（#abc 由 hexA 展开）
   *    'rgba(...)' / 'red'       → 原样
   *  认不出来也原样返回：宁可颜色不对，也不能返回 NaN 变成透明。 */
  function resolveColor(c) {
    var s = String(c == null ? '' : c).trim();
    if (!s) return '';
    if (s.slice(0, 2) === '--') return cssVar(s) || '#888888';
    if (s.slice(0, 4) === 'var(') return cssVar(s.slice(4, -1).trim()) || '#888888';
    return s;
  }

  /** 色值 → rgba(..., alpha)。
   *   旧版直接 parseInt(hex.slice(1), 16)：传进来一个变量名或 rgba() 就得到 NaN，
   *  rgba(NaN, NaN, NaN, .1) 是无效色值 → 整条填充变透明，且不报错、难以排查。这里防住。 */
  function hexA(color, alpha) {
    var c = resolveColor(color);
    var a = (alpha == null) ? 1 : alpha;
    if (!c) return 'rgba(0, 0, 0, 0)';

    var m = c.match(/^#([0-9a-f]{3})$/i);
    if (m) c = '#' + m[1].split('').map(function (ch) { return ch + ch; }).join('');

    m = c.match(/^#([0-9a-f]{6})$/i);
    if (m) {
      var n = parseInt(m[1], 16);
      return 'rgba(' + ((n >> 16) & 255) + ', ' + ((n >> 8) & 255) + ', ' + (n & 255) + ', ' + a + ')';
    }

    m = c.match(/^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)/i);
    if (m) return 'rgba(' + m[1] + ', ' + m[2] + ', ' + m[3] + ', ' + a + ')';

    return c;   // 认不出来（比如 'red'）就原样给，浏览器自己会处理
  }

  /** 当前主题下的一套图表配色 */
  function chartColors() {
    return {
      temp:    cssVar('--temp')    || '#2a78d6',
      humi:    cssVar('--humi')    || '#eb6834',
      accent:  cssVar('--accent')  || '#2a78d6',
      grid:    cssVar('--grid')    || '#e1e0d9',
      muted:   cssVar('--muted')   || '#898781',
      ink2:    cssVar('--ink-2')   || '#52514e',
      surface: cssVar('--surface') || '#ffffff',
      good:    cssVar('--good')    || '#0ca30c',
      bad:     cssVar('--bad')     || '#d03b3b',
    };
  }

  /** 分类色板：都是中间调，亮底暗底都看得清，所以不随主题变 */
  var PALETTE = ['#2a78d6', '#eb6834', '#0ca30c', '#b07de0', '#e0b400', '#d03b3b', '#12a5a5', '#8a8f98'];
  function paletteColor(i) { return PALETTE[((i % PALETTE.length) + PALETTE.length) % PALETTE.length]; }

  /* ================= 空状态 ================= */

  /** 所有 dataset 都空时，在图上居中写一行提示 —— 免得看起来像"有数据但是平的" */
  var emptyStatePlugin = {
    id: 'dashEmptyState',
    afterDraw: function (chart) {
      var opt = (chart.options.plugins || {}).dashEmptyState;
      var msg = opt && opt.text;
      if (!msg) return;
      var ds = chart.data.datasets || [];
      var has = false;
      for (var i = 0; i < ds.length; i++) {
        if (ds[i] && ds[i].data && ds[i].data.length) { has = true; break; }
      }
      if (has) return;

      var area = chart.chartArea;
      if (!area) return;
      var ctx = chart.ctx;
      ctx.save();
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillStyle = (opt.color || chartColors().muted);
      ctx.font = '12px system-ui, -apple-system, "Segoe UI", sans-serif';
      wrapText(ctx, msg, (area.left + area.right) / 2, (area.top + area.bottom) / 2, area.right - area.left - 16, 16);
      ctx.restore();
    },
  };

  function wrapText(ctx, text, cx, cy, maxW, lineH) {
    // 先按显式换行切开，再各自折行 —— canvas 的 fillText 不认 '\n'，
    // 不处理的话它会被当成一个字符画出来（或者直接看不见）。
    var lines = [];
    String(text).split('\n').forEach(function (para) {
      var cur = '';
      for (var i = 0; i < para.length; i++) {
        var t = cur + para[i];
        if (ctx.measureText(t).width > maxW && cur) { lines.push(cur); cur = para[i]; }
        else cur = t;
      }
      lines.push(cur);
    });
    var y = cy - (lines.length - 1) * lineH / 2;
    for (var j = 0; j < lines.length; j++) {
      ctx.fillText(lines[j], cx, y + j * lineH);
    }
  }

  /* ================= 建图 ================= */

  var REGISTRY = [];                  // 所有建过的图：换主题 / 切页签时统一处理

  function register(chart, kind) {
    REGISTRY.push({ chart: chart, kind: kind });
    return chart;
  }

  function isHidden(el) {
    // offsetParent 为 null 说明自己或祖先 display:none
    return !el || el.offsetParent === null;
  }

  function cartesianScales(opts) {
    var C = chartColors();
    var s = {
      x: {
        grid: { display: false },
        border: { color: C.grid },
        ticks: { color: C.muted, maxTicksLimit: opts.maxTicks || 8, maxRotation: 0, font: { size: 11 } },
      },
      y: {
        grid: { color: C.grid, width: 1 },
        border: { display: false },
        ticks: { color: C.muted, font: { size: 11 } },
        beginAtZero: !!opts.beginAtZero,
      },
    };
    if (opts.unit) s.y.title = { display: true, text: opts.unit, color: C.muted, font: { size: 11 } };
    return s;
  }

  function datasetFrom(seriesItem, i, type) {
    var token = seriesItem.color || (i === 0 ? '--accent' : paletteColor(i));
    var col = resolveColor(token) || paletteColor(i);
    var isBar = (type === 'bar');
    var ds = {
      label: seriesItem.label || '',
      data: seriesItem.data || [],
      __token: token,                                  // 记住"从哪来的色"，换主题时重取
      __fill: !!seriesItem.fill,
      borderColor: col,
      backgroundColor: seriesItem.fill ? hexA(col, 0.14) : col,
      borderWidth: isBar ? 0 : 2,
      borderRadius: isBar ? 3 : 0,
      tension: 0.25,
      fill: !!seriesItem.fill,
      spanGaps: true,                                  // 中间缺点就断开连线，别连成假的
    };
    if (!isBar) {
      // 页面可按需隐藏密集点位；hover 点仍独立保留，不能把图做成无法读数。
      ds.pointRadius = seriesItem.pointRadius == null ? 3 : seriesItem.pointRadius;
      ds.pointHoverRadius = seriesItem.pointHoverRadius == null ? 5 : seriesItem.pointHoverRadius;
      ds.pointBackgroundColor = col;
      //  原来这里写死 '#ffffff'：暗色主题下是刺眼白边，亮色主题下又和白卡片同色、等于没边。
      //    改成卡片底色，两种主题下都是"从卡片上抠出来一个点"的效果。
      ds.pointBorderColor = chartColors().surface;
      ds.pointBorderWidth = 2;
    }
    return ds;
  }

  function baseConfig(type, labels, series, opts) {
    opts = opts || {};
    var C = chartColors();
    var ds = (series || []).map(function (s, i) { return datasetFrom(s, i, type); });
    var cfg = {
      type: type,
      data: { labels: labels || [], datasets: ds },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: {
            display: !!opts.legend,
            labels: { color: C.ink2, boxWidth: 10, boxHeight: 10, font: { size: 11 } },
          },
          tooltip: { displayColors: !!opts.legend },
          dashEmptyState: { text: opts.empty || '暂无数据', color: C.muted },
        },
      },
    };
    if (type === 'doughnut' || type === 'pie') {
      cfg.options.cutout = type === 'doughnut' ? '58%' : 0;
      cfg.options.scales = {};
    } else {
      cfg.options.scales = cartesianScales(opts);
      // 'y' = 横条图：类型名字长（TEMP_HIGH 这种）时竖着排会挤成一团
      if (opts.indexAxis) cfg.options.indexAxis = opts.indexAxis;
    }
    return cfg;
  }

  /**
   * 建一个折线/柱状图。
   * @param canvasOrSel   canvas 元素或选择器
   * @param labels        x 轴标签数组
   * @param series        [{label, color, data, fill}]，color 可给 '--temp' 这类变量名
   * @param opts          {unit, legend, empty, maxTicks, beginAtZero}
   */
  function mkLineChart(canvasOrSel, labels, series, opts) {
    return build('line', canvasOrSel, labels, series, opts);
  }
  function mkBarChart(canvasOrSel, labels, series, opts) {
    return build('bar', canvasOrSel, labels, series, opts);
  }

  function build(type, canvasOrSel, labels, series, opts) {
    var el = (typeof canvasOrSel === 'string') ? document.querySelector(canvasOrSel) : canvasOrSel;
    if (!el) { console.warn('[ui-charts] canvas 不存在:', canvasOrSel); return null; }
    if (typeof Chart === 'undefined') { console.warn('[ui-charts] Chart.js 没加载'); return null; }
    if (isHidden(el)) {
      // 不拦着，只是留个线索：这种情况图可能量到 0 尺寸，要靠 resizeAll() 救
      console.warn('[ui-charts] 在隐藏容器里建图，切页签后请调 DASH.resizeAll()');
    }
    return register(new Chart(el, baseConfig(type, labels, series, opts)), type);
  }

  /**
   * 环形/饼图。items: [{label, value}] 或 [{label, value, color}]
   */
  function mkDoughnut(canvasOrSel, items, opts) {
    opts = opts || {};
    var el = (typeof canvasOrSel === 'string') ? document.querySelector(canvasOrSel) : canvasOrSel;
    if (!el) { console.warn('[ui-charts] canvas 不存在:', canvasOrSel); return null; }
    if (typeof Chart === 'undefined') { console.warn('[ui-charts] Chart.js 没加载'); return null; }

    var list = items || [];
    var cfg = {
      type: 'doughnut',
      data: {
        labels: list.map(function (x) { return x.label; }),
        datasets: [{
          data: list.map(function (x) { return x.value; }),
          __tokens: list.map(function (x, i) { return x.color || paletteColor(i); }),
          backgroundColor: list.map(function (x, i) { return resolveColor(x.color || paletteColor(i)); }),
          borderColor: chartColors().surface,
          borderWidth: 2,
        }],
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        cutout: opts.cutout || '58%',
        plugins: {
          legend: {
            display: !!(opts.legend !== false),
            position: 'right',
            labels: { color: chartColors().ink2, boxWidth: 10, boxHeight: 10, font: { size: 11 } },
          },
          tooltip: { displayColors: true },
          dashEmptyState: { text: opts.empty || '暂无数据', color: chartColors().muted },
        },
      },
    };
    return register(new Chart(el, cfg), 'doughnut');
  }

  /* ================= 更新 ================= */

  /** 就地换数据（图已存在时不要重建，否则闪一下且丢交互状态） */
  function setChartData(chart, labels, series) {
    if (!chart) return;
    chart.data.labels = labels || [];
    (series || []).forEach(function (s, i) {
      var ds = chart.data.datasets[i];
      if (!ds) return;
      ds.data = s.data || [];
      if (s.label != null) ds.label = s.label;
      if (s.color) {
        ds.__token = s.color;
        var col = resolveColor(s.color);
        ds.borderColor = col;
        ds.pointBackgroundColor = col;
        ds.backgroundColor = ds.__fill ? hexA(col, 0.14) : col;
      }
    });
    chart.update('none');
  }

  /** 环形图换数据（标签/颜色都重算） */
  function setDoughnutData(chart, items) {
    if (!chart) return;
    var list = items || [];
    chart.data.labels = list.map(function (x) { return x.label; });
    chart.data.datasets[0].data = list.map(function (x) { return x.value; });
    chart.data.datasets[0].__tokens = list.map(function (x, i) { return x.color || paletteColor(i); });
    chart.data.datasets[0].backgroundColor = list.map(function (x, i) {
      return resolveColor(x.color || paletteColor(i));
    });
    chart.update('none');
  }

  /* ================= 主题 / 尺寸 ================= */

  function restyleOne(entry) {
    var chart = entry.chart;
    var C = chartColors();
    (chart.data.datasets || []).forEach(function (ds) {
      // 线/柱：一个色
      if (ds.__token) {
        var col = resolveColor(ds.__token) || C.accent;
        if (chart.config.type === 'doughnut' || chart.config.type === 'pie') {
          ds.backgroundColor = col;
        } else {
          ds.borderColor = col;
          ds.pointBackgroundColor = col;
          ds.backgroundColor = ds.__fill ? hexA(col, 0.14) : col;
        }
        ds.pointBorderColor = C.surface;
      }
      // 环形：一组色
      if (ds.__tokens) {
        ds.backgroundColor = ds.__tokens.map(function (t) { return resolveColor(t); });
        ds.borderColor = C.surface;
      }
    });

    var o = chart.options;
    if (o.scales) {
      Object.keys(o.scales).forEach(function (k) {
        var sc = o.scales[k];
        if (sc.grid && sc.grid.color) sc.grid.color = C.grid;
        if (sc.border) sc.border.color = C.grid;
        if (sc.ticks) sc.ticks.color = C.muted;
        if (sc.title) sc.title.color = C.muted;
      });
    }
    if (o.plugins) {
      if (o.plugins.legend && o.plugins.legend.labels) o.plugins.legend.labels.color = C.ink2;
      if (o.plugins.dashEmptyState) {
        o.plugins.dashEmptyState.color = C.muted;
        o.plugins.dashEmptyState.text = o.plugins.dashEmptyState.text;   // 触发重绘
      }
    }
    chart.update('none');
  }

  /** 换主题后调用：所有图重取色 */
  function restyleAll() {
    REGISTRY.forEach(function (e) {
      try { restyleOne(e); } catch (err) { console.warn('图表重着色失败', err); }
    });
  }

  /** 切页签 / 窗口变化后调用：把隐藏期间量错尺寸的图重新量一遍。
   *  必须等一帧，否则容器刚 display:block、布局还没算完。 */
  function resizeAll() {
    requestAnimationFrame(function () {
      REGISTRY.forEach(function (e) {
        var cv = e.chart && e.chart.canvas;
        if (!cv || isHidden(cv)) return;
        try { e.chart.resize(); } catch (err) { /* 图被销毁了，忽略 */ }
      });
    });
  }

  /** 销毁某张图（页面重画时用，避免同一个 canvas 上叠两个实例） */
  function destroy(chart) {
    if (!chart) return;
    var i = REGISTRY.findIndex(function (e) { return e.chart === chart; });
    if (i >= 0) REGISTRY.splice(i, 1);
    try { chart.destroy(); } catch (e) { /* ignore */ }
  }

  window.addEventListener('themechange', restyleAll);

  /* ================= 导出 ================= */

  window.UIChart = {
    cssVar: cssVar,
    resolveColor: resolveColor,
    hexA: hexA,
    chartColors: chartColors,
    paletteColor: paletteColor,
    PALETTE: PALETTE,
    mkLineChart: mkLineChart,
    mkBarChart: mkBarChart,
    mkDoughnut: mkDoughnut,
    setChartData: setChartData,
    setDoughnutData: setDoughnutData,
    restyleAll: restyleAll,
    resizeAll: resizeAll,
    destroy: destroy,
    registry: REGISTRY,
  };

  // 顶层再放一份，方便老代码和新页面直接用的 `hexA(...)` / `hexA` 写法
  window.hexA = hexA;
  window.cssVar = cssVar;
  window.chartColors = chartColors;
})();
