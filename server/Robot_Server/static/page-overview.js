/**
 * @file page-overview.js
 * 用途与使用：总览页状态、最新读数和设备在线信息；API 返回后更新卡片。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
// ============================================================
// 总览页（page-overview.js）
// ------------------------------------------------------------
// 四张数字卡 + 模型条 + 最近异常 / 最近互动 / 环境快照。
// 数字全部来自 /api/stats/summary（relay.db），不是 .txt 行数。
// ============================================================
(function () {
  'use strict';
  var D = window.DASH_API;

  var chartOvTemp = null;

  function tileText(id, v) {
    var el = D.$(id);
    if (el) el.textContent = (v === null || v === undefined) ? '–' : v;
  }

  function renderSummary(s) {
    tileText('#st-today-readings', s.today_readings);
    tileText('#st-today-anomalies', s.today_anomalies);
    tileText('#st-today-interactions', s.today_interactions);
    tileText('#st-devices', s.devices_online + ' / ' + s.devices_total);
    tileText('#st-data-total', s.total_readings);
    tileText('#st-decision-total', s.total_interactions);
  }

  function renderHealth(h) {
    var active = h.active_provider;
    tileText('#st-mode', active);
    var list = Object.entries(h.providers || {});
    var on = list.filter(function (kv) { return kv[1].enabled && kv[1].key_count > 0; }).length;
    tileText('#st-models', on + ' / ' + list.length);

    var chips = [];
    chips.push('<span class="chip clickable ' + (active === 'auto' ? 'on' : '') +
      '" data-pro="auto" title="自动故障转移">⚙ auto</span>');
    list.forEach(function (kv) {
      var name = kv[0], p = kv[1];
      var cls = active === name ? 'on' : '';
      var off = (p.enabled && p.key_count > 0) ? '' : ' off';
      var mark = p.enabled ? '●' : '○';
      chips.push('<span class="chip clickable ' + cls + off + '" data-pro="' + D.escapeHtml(name) +
        '" title="点击切换">' + mark + ' ' + D.escapeHtml(name) + ' · ' +
        D.escapeHtml(p.model) + ' · ' + p.key_count + 'K</span>');
    });
    chips.push('<span class="chip add" id="btn-add-model" title="新增模型">＋ 新增模型</span>');
    var box = D.$('#provider-strip');
    if (box) box.innerHTML = chips.join('');
  }

  function renderRecentAnomaly(a) {
    var meta = D.$('#ov-anom-meta');
    if (meta) meta.textContent = '24 小时内共 ' + (a.total || 0) + ' 条';
    var rows = (a.recent || []).slice(0, 6).map(function (r) {
      return [D.timeShort(r.ts), r.code, D.sevBadge(r.severity), r.message || ''];
    });
    D.buildTable('#tbl-ov-anom', ['时间', '类型', '等级', '说明'], rows);
  }

  function renderRecentInteract(it) {
    var meta = D.$('#ov-inter-meta');
    if (meta) {
      meta.textContent = '24 小时内 ' + (it.total || 0) + ' 次（真实 ' + (it.real_ai || 0) +
        ' · 测试 ' + (it.mock || 0) + ' · 定时 ' + (it.auto_tick || 0) + '）';
    }
    var rows = (it.recent || []).slice(0, 6).map(function (r) {
      var tag = r.is_auto_tick ? '[定时] ' : (r.is_mock ? '[测试] ' : '');
      return [D.timeShort(r.ts), r.user_text || '—', tag + (r.ai_message || '—'), r.n_moves];
    });
    D.buildTable('#tbl-ov-inter', ['时间', '用户说', 'AI 回复', '动作数'], rows);
  }

  function renderEnvSnapshot(se) {
    var chartBox = D.$('#chart-ov-temp').parentElement;
    chartBox.classList.toggle('empty', !se.samples);
    var points = se.points || [];
    var latest = points.length ? points[points.length - 1] : null;
    tileText('#home-temperature', latest ? D.fmtNum(latest.temp, 1) : '—');
    tileText('#home-humidity', latest ? D.fmtNum(latest.humi, 0) : '—');
    tileText('#home-env-updated', latest ? '最近采样：' + latest.ts : '尚无环境采样');
    var meta = D.$('#ov-env-meta');
    if (meta) {
      meta.textContent = se.samples
        ? ('24 小时 · 有效采样 ' + se.samples + ' 条')
        : '24 小时 · 尚无有效采样';
    }
    var labels = (se.points || []).map(function (p) { return D.timeShort(p.ts); });
    var series = [{ label: '温度 °C', color: '--temp', data: (se.points || []).map(function (p) { return p.temp; }), fill: true }];

    if (!chartOvTemp) {
      chartOvTemp = D.mkLineChart('#chart-ov-temp', labels, series, {
        legend: false,
        maxTicks: 6,
        empty: se.samples ? '' : '等待设备上传环境数据',
      });
    } else {
      D.setChartData(chartOvTemp, labels, series);
    }
  }

  function init() {
    if (window.ROBOT_CHAT) window.ROBOT_CHAT.init();
    var box = D.$('#provider-strip');
    if (box) {
      box.addEventListener('click', async function (e) {
        var chip = e.target.closest('.chip');
        if (!chip) return;
        if (chip.id === 'btn-add-model') { D.openModelModal(); return; }
        var pro = chip.dataset.pro;
        if (!pro) return;
        var j = await D.postJSON('/api/provider/switch', { provider: pro });
        if (j.error) { alert(j.error); return; }
        D.refreshAll();
      });
    }
  }

  var actionsKey = '', actionsBusy = false;
  function renderActions(j) {
    var box = D.$('#home-actions');
    if (!box) return;
    var key = JSON.stringify(j.actions);
    if (key === actionsKey) return;
    actionsKey = key;
    var icons = { wave: '✋', nod: '↕', happy: '☺', home: '⤾' };
    box.replaceChildren();
    ['wave', 'nod', 'happy', 'home'].forEach(function (id) {
      var action = (j.actions || []).find(function (a) { return a.id === id; });
      var b = document.createElement('button');
      b.type = 'button'; b.dataset.actionId = id;
      b.dataset.ready = action && action.ready ? '1' : '0';
      b.disabled = b.dataset.ready !== '1' || actionsBusy;
      b.title = action ? action.unavailable_reason || action.summary || action.name : '尚未登记此动作';
      var icon = document.createElement('span'); icon.textContent = icons[id]; icon.setAttribute('aria-hidden', 'true');
      var name = document.createElement('strong'); name.textContent = action ? action.name : id;
      b.append(icon, name);
      b.addEventListener('click', async function () {
        if (actionsBusy || !action || !action.ready) return;
        actionsBusy = true;
        box.querySelectorAll('button').forEach(function (btn) { btn.disabled = true; });
        try {
          var r = await D.postJSON('/api/actions', { action_id: id, device_id: 'esp-bridge' });
          if (!r.ok || r.error) throw new Error(r.error || '动作未入队');
          tileText('#home-action-result', '“' + action.name + '”已入队，等待设备执行。');
        } catch (e) { tileText('#home-action-result', '发送失败：' + e.message); }
        finally {
          actionsBusy = false;
          box.querySelectorAll('button').forEach(function (btn) { btn.disabled = btn.dataset.ready !== '1'; });
        }
      });
      box.appendChild(b);
    });
    tileText('#home-action-result', '选一个动作，和它打个招呼吧。');
  }

  function renderDevice(j) {
    var device = (j.devices || []).find(function (d) { return d.device_id === 'esp-bridge'; });
    var el = D.$('#home-device-status');
    if (!el) return;
    var fresh = !!(device && device.online && device.poll_fresh !== false);
    el.dataset.online = String(fresh);
    el.textContent = fresh ? '● 设备在线' : device && device.online ? '◌ 上报间断' : '○ 设备离线';
    el.title = device && device.last_seen ? '最近上报：' + device.last_seen : '尚未收到设备上报';
  }

  function refresh() {
    var tasks = [
      D.getJSON('/api/stats/summary'),
      D.getJSON('/api/health'),
      D.getJSON('/api/stats/anomaly?hours=24&limit=6'),
      D.getJSON('/api/stats/interaction?hours=24&limit=6'),
      D.getJSON('/api/stats/sensor?hours=24&limit=120'),
      D.getJSON('/api/devices'),
      D.getJSON('/api/actions?device_id=esp-bridge'),
      window.ROBOT_CHAT ? window.ROBOT_CHAT.refresh() : Promise.resolve(),
    ];
    return Promise.allSettled(tasks).then(function (results) {
      var renderers = [renderSummary, renderHealth, renderRecentAnomaly,
        renderRecentInteract, renderEnvSnapshot, renderDevice, renderActions];
      var errors = [];
      results.forEach(function (r, i) {
        if (r.status === 'fulfilled' && r.value && !r.value.error) {
          if (renderers[i]) renderers[i](r.value);
        } else if (r.status === 'rejected' || (r.value && r.value.error)) {
          errors.push(r.reason || r.value.error);
          if (i === 5) tileText('#home-device-status', '设备状态读取失败');
          if (i === 6) tileText('#home-action-result', '动作列表读取失败，请稍后刷新。');
        }
      });
      if (errors.length) throw new Error('部分数据读取失败：' + errors.map(String).join('；'));
    });
  }

  window.PAGES = window.PAGES || {};
  window.PAGES.overview = { title: '总览', init: init, refresh: refresh };
})();
