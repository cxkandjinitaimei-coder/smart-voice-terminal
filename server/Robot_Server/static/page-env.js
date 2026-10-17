/**
 * @file page-env.js
 * 用途与使用：温湿度历史曲线与区间查询；按页面筛选加载数据，不改变采样周期。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
// ============================================================
// 环境页（page-env.js）—— 温湿度趋势 + 区间统计 + 原始数据流
// ------------------------------------------------------------
//  数据源是 relay.db 的 sensor_readings（只算 valid=1），**不是** data_history.txt。
//   原始 .txt 里有 7000+ 行 sensor 是"无数据"，直接画出来就是一条 7600 个假点的曲线
//   （这正是改造前那张图的问题）。下面这两张图画的是有效采样。
//   最底下那张表展示的仍是原始 .txt —— 那是"流水账"，本来就是给排查用的。
// ============================================================
(function () {
  'use strict';
  var D = window.DASH_API;

  var trendHours = 3;
  var dataPage = 1;
  var chartTemp = null, chartHumi = null;

  /*
   * 采集、原始记录和统计都保持逐条不变；这里只控制“画出来的密度”。
   * 同一分钟内可能有很多上报，图上只留该分钟最后一条，既能看清趋势，
   * 又让末端读数与“最新读数”一致。epoch 不存在时退回 ts 的 HH:MM。
   */
  function minutePoints(points) {
    var chosen = [];
    var lastMinute = null;
    (points || []).forEach(function (p) {
      var epoch = Number(p && p.epoch);
      var minute = isFinite(epoch) && epoch > 0
        ? Math.floor(epoch / 60)
        : String((p && p.ts) || '').slice(0, 16);
      if (minute === lastMinute && chosen.length) chosen[chosen.length - 1] = p;
      else {
        chosen.push(p);
        lastMinute = minute;
      }
    });
    return chosen;
  }

  function statTile(k, v) {
    return '<div class="stat"><span class="k">' + D.escapeHtml(k) +
      '</span><span class="v">' + D.escapeHtml(v) + '</span></div>';
  }

  function renderEnvStats(se) {
    var box = D.$('#env-stats');
    if (!box) return;
    var rng = function (o) {
      if (!o || o.min === null || o.min === undefined) return '—';
      return D.fmtNum(o.min, 1) + ' ~ ' + D.fmtNum(o.max, 1);
    };
    var now = se.now;
    box.innerHTML =
      statTile('温度范围 °C', rng(se.temp)) +
      statTile('温度均值 °C', se.temp && se.temp.avg != null ? D.fmtNum(se.temp.avg, 1) : '—') +
      statTile('湿度范围 %', rng(se.humi)) +
      statTile('湿度均值 %', se.humi && se.humi.avg != null ? D.fmtNum(se.humi.avg, 1) : '—') +
      statTile('有效采样', String(se.samples || 0)) +
      statTile('最新读数', now
        ? (D.timeShort(now.ts) + '  ' + D.fmtNum(now.temp, 1) + '°C / ' + D.fmtNum(now.humi, 1) + '%')
        : '—');
  }

  function renderTrend(se) {
    var pts = minutePoints(se.points);
    var labels = pts.map(function (p) { return D.timeShort(p.ts); });
    // 线仍连续，但不画每个点的圆标；鼠标靠近该分钟时仍可看数值。
    var tempSeries = [{ label: '温度 °C', color: '--temp', data: pts.map(function (p) { return p.temp; }), fill: true, pointRadius: 0, pointHoverRadius: 4 }];
    var humiSeries = [{ label: '湿度 %', color: '--humi', data: pts.map(function (p) { return p.humi; }), fill: true, pointRadius: 0, pointHoverRadius: 4 }];
    var emptyMsg = se.samples
      ? ''
      : ('本时段暂无有效环境采样\n收到温湿度上报后显示曲线');

    if (!chartTemp) {
      chartTemp = D.mkLineChart('#chart-temp', labels, tempSeries, { empty: emptyMsg, maxTicks: 8 });
      chartHumi = D.mkLineChart('#chart-humi', labels, humiSeries, { empty: emptyMsg, maxTicks: 8 });
    } else {
      D.setChartData(chartTemp, labels, tempSeries);
      D.setChartData(chartHumi, labels, humiSeries);
    }

    var box = D.$('#env-empty');
    if (box) {
      if (se.samples) {
        box.classList.add('hidden');
      } else {
        box.classList.remove('hidden');
        box.textContent = '本时段暂无有效环境采样。请检查设备是否在线、温湿度是否正常上报，或切换更长时间范围。';
      }
    }
  }

  function init() {
    var tabs = D.$('#range-tabs');
    if (tabs) {
      tabs.addEventListener('click', function (e) {
        var btn = e.target.closest('.rtab');
        if (!btn) return;
        Array.prototype.forEach.call(tabs.querySelectorAll('.rtab'), function (b) {
          b.classList.remove('active');
        });
        btn.classList.add('active');
        trendHours = parseInt(btn.dataset.hours, 10) || 3;
        refresh();
      });
    }
  }

  function refresh() {
    return Promise.all([
      D.getJSON('/api/stats/sensor?hours=' + trendHours + '&limit=400'),
      D.getJSON('/api/history?per_page=' + D.DATA_PER_PAGE + '&page=' + dataPage +
                '&dper_page=1&dpage=1'),
    ]).then(function (r) {
      var se = r[0], h = r[1];
      if (se && !se.error) {
        renderEnvStats(se);
        renderTrend(se);
      }
      if (h) {
        var totalEl = D.$('#env-total');
        if (totalEl) totalEl.textContent = h.data_total;
        D.buildTable('#tbl-data', ['时间', '传感器数据', '用户请求'],
          h.data.map(function (d) { return [d.time, d.sensor, d.user || '—']; }));
        D.renderPager('#pager-data', h.page, h.data_pages, function (p) {
          dataPage = p;
          refresh();
        });
      }
      D.markUp();
    });
  }

  window.PAGES = window.PAGES || {};
  window.PAGES.env = { title: '环境', init: init, refresh: refresh };
})();
