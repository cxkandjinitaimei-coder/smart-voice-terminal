// ============================================================
// 设置页（page-settings.js）
//   · AI 性格（persona）—— 只能改说话方式
//   · 异常判定阈值 —— 存 config.json 的 thresholds 段，改完不用重启
//   · 定时唤醒 —— 走已有的 /api/auto_tick（GET 看状态 / POST 改配置）
//   · 模型管理 —— 只读列表 + 新增入口（弹窗在 app.js 里，属于全局）
// ------------------------------------------------------------
// ⚠️ 这一页的 refresh() **故意不重画输入框**。
//   轮询（5 秒一次）和用户正在打字是冲突的：每 5 秒把你刚改的值冲掉，
//   体验就是"改了没用"。所以只刷新只读的状态区（定时唤醒状态、模型表）。
// ============================================================
(function () {
  'use strict';
  var D = window.DASH_API;

  var thMeta = null;      // /api/thresholds 的 fields/ops/severities
  var thRules = [];       // 编辑器里的规则（内存态，保存时才写回）
  var loaded = false;
  var tickLoaded = false;
  var adminProfile = null;

  function statTile(k, v) {
    return '<div class="stat"><span class="k">' + D.escapeHtml(k) +
      '</span><span class="v sm">' + D.escapeHtml(v) + '</span></div>';
  }

  /* ================= AI 性格 ================= */

  async function loadPersona() {
    try {
      const j = await D.getJSON('/api/settings');
      const el = D.$('#in-persona');
      if (el && j.persona != null) el.value = j.persona;
      const box = D.$('#locked-list');
      if (box) {
        const items = [];
        items.push(j.locked && j.locked.note ? j.locked.note : '决策信封与硬件接口不可改');
        const mts = (j.locked && j.locked.move_types) || [];
        if (mts.length) items.push('move_types: ' + mts.join(', '));
        const servos = (j.locked && j.locked.hardware_servos) || [];
        servos.forEach((s) => {
          items.push(`舵机${s.id} ${s.name || ''}：${s.min}°~${s.max}°（中值${s.mid}°）`);
        });
        box.innerHTML = items.map((t) => `<li>${D.escapeHtml(t)}</li>`).join('');
      }
    } catch (e) {
      console.error('加载 AI 设定失败', e);
    }
  }

  function bindPersona() {
    const btn = D.$('#btn-save-persona');
    if (!btn) return;
    btn.addEventListener('click', async () => {
      const box = D.$('#persona-result');
      const persona = D.$('#in-persona').value.trim();
      if (!persona) { alert('persona 不能为空'); return; }
      btn.disabled = true;
      btn.textContent = '保存中…';
      try {
        const j = await D.postJSON('/api/settings', { persona });
        if (j.error) {
          box.className = 'result err';
          box.innerHTML = `<div class="head">✘ ${D.escapeHtml(j.error)}</div>`;
        } else {
          box.className = 'result ok';
          box.innerHTML = `<div class="head">✔ 已保存，下次 AI 调用即按新性格生效</div>`;
        }
      } catch (e) {
        box.className = 'result err';
        box.innerHTML = `<div class="head">✘ 保存失败</div><pre>${D.escapeHtml(String(e))}</pre>`;
      } finally {
        box.classList.remove('hidden');
        btn.disabled = false;
        btn.textContent = '保存性格设定';
      }
    });
  }

  /* ================= 管理员设备参数 ================= */

  function adminResult(text, ok) {
    const box = D.$('#admin-profile-result');
    if (!box) return;
    box.className = 'result ' + (ok ? 'ok' : 'err');
    box.textContent = text;
    box.classList.remove('hidden');
  }

  function adminInput(label, key, value, type) {
    const wrap = document.createElement('label');
    wrap.className = 'field';
    wrap.style.display = 'inline-flex'; wrap.style.width = 'auto'; wrap.style.marginRight = '8px';
    wrap.textContent = label + ' ';
    const input = document.createElement('input');
    input.type = type || 'text'; input.value = value == null ? '' : value;
    input.dataset.key = key;
    if (type === 'number') { input.min = '0'; input.max = key === 'speed' ? '360' : '180'; input.step = '1'; input.style.width = '76px'; }
    wrap.appendChild(input);
    return wrap;
  }

  function renderAdminProfile() {
    const box = D.$('#admin-servo-list');
    if (!box || !adminProfile) return;
    box.innerHTML = '';
    (adminProfile.servos || []).forEach(function (servo, index) {
      const row = document.createElement('div');
      row.className = 'locked-box'; row.style.marginBottom = '10px';
      const title = document.createElement('div'); title.className = 'locked-title';
      const fixed = window.ROBOT_SERVO_CONTRACT.servos.some(s=>s.id===servo.id);
      title.textContent = '舵机 ' + (servo.id || '?') + (fixed ? '（编号和作用固定；角度可校准）' : '（管理员可编辑）'); row.appendChild(title);
      [['编号','id','number'], ['名称','name'], ['角色','role'], ['最小','min','number'], ['中值','mid','number'],
       ['最大','max','number'], ['初始 / 回正','front','number'], ['默认速度(°/秒)','speed','number'], ['方向说明','direction'], ['备注','note']].forEach(function (x) {
        const el = adminInput(x[0], x[1], servo[x[1]], x[2]);
        if(fixed && ['id','role'].includes(x[1])) el.querySelector('input').readOnly=true;
        el.querySelector('input').addEventListener('input', function (ev) {
          servo[x[1]] = x[1] === 'speed' && ev.target.value === '' ? null : x[2] === 'number' ? Number(ev.target.value) : ev.target.value;
        });
        row.appendChild(el);
      });
      const del = document.createElement('button'); del.type = 'button'; del.className = 'qbtn'; del.textContent = '删除';
      del.disabled=fixed;
      del.addEventListener('click', function () { adminProfile.servos.splice(index, 1); renderAdminProfile(); });
      row.appendChild(del); box.appendChild(row);
    });
    const exp = D.$('#admin-expression-list');
    if (exp) exp.textContent = (adminProfile.expressions || []).map(function (x) {
      return x.name + ' = ' + x.id + ' → ' + x.file;
    }).join('　·　') || '尚未登记表情';
  }

  async function loadAdminProfile() {
    const j = await D.getJSON('/api/admin/device-profile');
    if (j.error) throw new Error(j.error);
    adminProfile = j.profile; renderAdminProfile();
  }

  function bindAdminProfile() {
    const add = D.$('#btn-admin-servo-add');
    if (add) add.addEventListener('click', function () {
      if (!adminProfile) return;
      const used = (adminProfile.servos || []).map(function (s) { return Number(s.id); });
      let id = 1; while (used.indexOf(id) >= 0 && id <= 16) id += 1;
      if (id > 16) { adminResult('最多可登记 16 个舵机。', false); return; }
      adminProfile.servos.push({id: id, name: '舵机' + id, role: 'user_defined', min: 0, mid: 90,
        max: 180, front: 90, direction: '待校准', note: ''});
      renderAdminProfile();
    });
    const save = D.$('#btn-admin-profile-save');
    if (save) save.addEventListener('click', async function () {
      if (!adminProfile) return;
      save.disabled = true; save.textContent = '保存中…';
      try {
        const j = await D.postJSON('/api/admin/device-profile', { profile: adminProfile, parameters_only:true });
        if (j.error || !j.ok) throw new Error(j.error || '保存失败');
        adminProfile = j.profile; renderAdminProfile();
        document.dispatchEvent(new CustomEvent('device-profile-saved'));
        adminResult('✅ 已保存。下一次 AI 对话和动作库使用新限位、正面角度和逐轴默认速度（留空则继承通用速度）；ESP 下次空闲轮询同步给 F407。测试页“舵机限位：已同步到 MCU”可确认。pitch=点头，yaw=摇头，各只绑定一个舵机。', true);
      } catch (e) { adminResult('❌ 保存失败：' + e, false); }
      finally { save.disabled = false; save.textContent = '保存管理员设备参数'; }
    });
  }

  /* ================= 阈值 ================= */

  function opts(list, cur, labelFn) {
    return list.map(function (o) {
      return '<option value="' + D.escapeHtml(o) + '"' + (String(o) === String(cur) ? ' selected' : '') + '>' +
        D.escapeHtml(labelFn ? labelFn(o) : o) + '</option>';
    }).join('');
  }

  function renderRules() {
    const box = D.$('#th-list');
    if (!box) return;
    const meta = thMeta || { fields: [], ops: [], severities: [], field_labels: {} };
    box.innerHTML = '';

    if (!thRules.length) {
      box.className = 'empty-box';
      box.innerHTML = '还没有规则。点下面的「＋ 加一条规则」开始。';
      return;
    }
    box.className = '';

    thRules.forEach(function (r, idx) {
      const row = document.createElement('div');
      row.className = 'th-row';

      const mk = (tag, cls, attrs, val) => {
        const el = document.createElement(tag);
        if (cls) el.className = cls;
        Object.keys(attrs || {}).forEach(function (k) { el.setAttribute(k, attrs[k]); });
        if (tag === 'select') el.innerHTML = val;
        else el.value = val;
        return el;
      };

      const codeEl = mk('input', 'th-edit', { type: 'text', placeholder: 'CODE' }, r.code || '');
      const labelEl = mk('input', 'th-edit', { type: 'text', placeholder: '名称' }, r.label || '');
      const fieldEl = mk('select', 'th-edit',
        {}, opts(meta.fields, r.field, function (f) { return (meta.field_labels || {})[f] || f; }));
      const opEl = mk('select', 'th-edit sm', {}, opts(meta.ops, r.op));
      const valEl = mk('input', 'th-edit sm', { type: 'number', step: 'any' }, r.value);
      const sevEl = mk('select', 'th-edit sm', {}, opts(meta.severities, r.severity));

      const chgWrap = document.createElement('label');
      chgWrap.className = 'chk';
      chgWrap.style.margin = '0';
      const chgEl = document.createElement('input');
      chgEl.type = 'checkbox';
      chgEl.checked = !!r.on_change;
      chgWrap.appendChild(chgEl);
      chgWrap.appendChild(document.createTextNode('仅翻转时报'));

      const del = document.createElement('button');
      del.type = 'button';
      del.className = 'th-del';
      del.textContent = '×';
      del.title = '删除这条规则';
      del.addEventListener('click', function () {
        thRules.splice(idx, 1);
        renderRules();
      });

      // 改动实时写回内存态，保存时统一提交
      codeEl.addEventListener('input', function () { r.code = codeEl.value; });
      labelEl.addEventListener('input', function () { r.label = labelEl.value; });
      fieldEl.addEventListener('change', function () { r.field = fieldEl.value; });
      opEl.addEventListener('change', function () { r.op = opEl.value; });
      valEl.addEventListener('input', function () { r.value = valEl.value; });
      sevEl.addEventListener('change', function () { r.severity = sevEl.value; });
      chgEl.addEventListener('change', function () { r.on_change = chgEl.checked; });

      [codeEl, labelEl, fieldEl, opEl, valEl, sevEl, chgWrap, del].forEach(function (el) {
        row.appendChild(el);
      });
      box.appendChild(row);
    });
  }

  async function loadThresholds() {
    const j = await D.getJSON('/api/thresholds');
    thMeta = {
      fields: j.fields || [], ops: j.ops || [], severities: j.severities || [],
      field_labels: j.field_labels || {},
    };
    const t = j.thresholds || {};
    const en = D.$('#th-enabled');
    if (en) en.checked = !!t.enabled;
    const cd = D.$('#th-cooldown');
    if (cd) cd.value = (t.cooldown_sec == null ? 60 : t.cooldown_sec);
    thRules = (t.rules || []).map(function (r) { return Object.assign({}, r); });
    renderRules();
  }

  function bindThresholds() {
    const add = D.$('#btn-th-add');
    if (add) {
      add.addEventListener('click', function () {
        thRules.push({
          code: 'NEW_RULE', label: '新规则', field: 'temp', op: '>',
          value: 30, severity: 'warn', on_change: false,
        });
        renderRules();
      });
    }

    const save = D.$('#btn-th-save');
    if (save) {
      save.addEventListener('click', async function () {
        const box = D.$('#th-result');
        // 前端先做一遍和 /api/thresholds 一样的校验，把错误定位到具体哪条
        for (let i = 0; i < thRules.length; i++) {
          const r = thRules[i];
          if (!String(r.code || '').trim()) {
            alert('第 ' + (i + 1) + ' 条的 code 不能为空');
            return;
          }
          if (r.value === '' || r.value == null || !isFinite(Number(r.value))) {
            alert('第 ' + (i + 1) + ' 条（' + r.code + '）的阈值必须是数字');
            return;
          }
        }
        save.disabled = true;
        save.textContent = '保存中…';
        try {
          const body = {
            enabled: D.$('#th-enabled').checked,
            cooldown_sec: parseInt(D.$('#th-cooldown').value, 10) || 0,
            rules: thRules.map(function (r) {
              return {
                code: String(r.code).trim(), label: String(r.label || '').trim() || String(r.code).trim(),
                field: r.field, op: r.op, value: Number(r.value),
                severity: r.severity, on_change: !!r.on_change,
              };
            }),
          };
          const j = await D.postJSON('/api/thresholds', body);
          if (j.error) {
            box.className = 'result err';
            box.innerHTML = `<div class="head">✘ ${D.escapeHtml(j.error)}</div>`;
          } else {
            // 服务端会把不合法的东西清掉，以它返回的为准回填
            const t = j.thresholds || {};
            thRules = (t.rules || []).map(function (r) { return Object.assign({}, r); });
            renderRules();
            box.className = 'result ok';
            box.innerHTML = `<div class="head">✔ 已保存 ${thRules.length} 条规则，下次上报即生效</div>`;
          }
        } catch (e) {
          box.className = 'result err';
          box.innerHTML = `<div class="head">✘ 保存失败</div><pre>${D.escapeHtml(String(e))}</pre>`;
        } finally {
          box.classList.remove('hidden');
          save.disabled = false;
          save.textContent = '保存阈值';
        }
      });
    }
  }

  /* ================= 定时唤醒 ================= */

  function renderTickStatus(s) {
    const box = D.$('#tick-status');
    if (!box) return;
    box.innerHTML =
      statTile('线程', s.running ? '运行中' : '未启动') +
      statTile('已唤醒', String(s.count == null ? 0 : s.count)) +
      statTile('上次唤醒', s.last_tick ? D.timeShort(s.last_tick) : '—') +
      statTile('下次唤醒', s.next_tick ? D.timeShort(s.next_tick) : '—');
  }

  async function loadTick(fillInputs) {
    const j = await D.getJSON('/api/auto_tick');
    renderTickStatus(j);
    const c = j.config || {};
    if (fillInputs && !tickLoaded) {
      tickLoaded = true;
      const en = D.$('#tick-enabled'); if (en) en.checked = !!c.enabled;
      const iv = D.$('#tick-interval'); if (iv) iv.value = (c.interval_sec == null ? 300 : c.interval_sec);
      const qs = D.$('#tick-quiet');    if (qs) qs.value = (c.quiet_sec == null ? 600 : c.quiet_sec);
      const md = D.$('#tick-mode');     if (md) md.value = c.mode || '';
    }
  }

  function bindTick() {
    const btn = D.$('#btn-tick-save');
    if (!btn) return;
    btn.addEventListener('click', async function () {
      const box = D.$('#tick-result');
      btn.disabled = true;
      btn.textContent = '保存中…';
      try {
        const j = await D.postJSON('/api/auto_tick', {
          enabled: D.$('#tick-enabled').checked,
          interval_sec: parseInt(D.$('#tick-interval').value, 10) || 300,
          quiet_sec: parseInt(D.$('#tick-quiet').value, 10) || 0,
          mode: D.$('#tick-mode').value,
        });
        if (j.error) {
          box.className = 'result err';
          box.innerHTML = `<div class="head">✘ ${D.escapeHtml(j.error)}</div>`;
        } else {
          renderTickStatus(j);
          // 服务端会把 interval_sec 夹到 >=5，以它返回的为准回填
          const c = j.config || {};
          const iv = D.$('#tick-interval'); if (iv) iv.value = c.interval_sec;
          box.className = 'result ok';
          box.innerHTML = `<div class="head">✔ 已保存，跑着的线程下一轮就读到，不用重启</div>`;
        }
      } catch (e) {
        box.className = 'result err';
        box.innerHTML = `<div class="head">✘ 保存失败</div><pre>${D.escapeHtml(String(e))}</pre>`;
      } finally {
        box.classList.remove('hidden');
        btn.disabled = false;
        btn.textContent = '保存定时唤醒设置';
      }
    });
  }

  /* ================= 模型列表 ================= */

  async function loadModels() {
    const h = await D.getJSON('/api/health');
    const list = Object.entries(h.providers || {});
    const rows = list.map(function (kv) {
      const name = kv[0], p = kv[1];
      const usable = p.enabled && p.key_count > 0;
      return [name, p.model || '—', p.key_count, p.enabled ? '启用' : '停用',
        usable ? '可用' : '不可用',
        (h.active_provider === name ? '← 当前' : (h.active_provider === 'auto' && usable ? 'auto 会选' : ''))];
    });
    D.buildTable('#tbl-models', ['名称', '模型', 'Key 数', '开关', '状态', ''], rows);
  }

  /* ================= 组装 ================= */

  async function init() {
    if (loaded) return;
    loaded = true;
    bindPersona();
    bindAdminProfile();
    if(window.IDLE_PET_UI)await window.IDLE_PET_UI.init().catch(function(e){console.error(e);});
    bindThresholds();
    bindTick();
    await Promise.all([
      loadPersona().catch(function (e) { console.error(e); }),
      loadAdminProfile().catch(function (e) { console.error('加载管理员设备档案失败', e); }),
      loadThresholds().catch(function (e) { console.error('加载阈值失败', e); }),
      loadTick(true).catch(function (e) { console.error(e); }),
      loadModels().catch(function (e) { console.error(e); }),
    ]);
  }

  /** 轮询只更新只读区；绝不碰用户正在编辑的输入框 */
  function refresh() {
    if (!loaded) return Promise.resolve();
    return Promise.all([
      loadTick(false).catch(function () {}),
      window.IDLE_PET_UI ? window.IDLE_PET_UI.refresh().catch(function(){}) : Promise.resolve(),
      loadModels().catch(function () {}),
    ]);
  }

  // Embedded device-test component. Share initialization across the first refresh.
  var initialization;
  window.DEVICE_SETTINGS_UI = {
    init: function () { return initialization || (initialization = init()); },
    refresh: refresh
  };
})();
