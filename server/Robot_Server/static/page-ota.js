// 公网 OTA 默认由 ESP 主动向云端领取任务。云端不反连 NAT 后的 ESP:8080。
(function () {
  'use strict';
  var D = window.DASH_API, POLL_MS = 1000;
  var job = null, jobTimer = null, busy = false, selectedFw = '', fwFingerprint = '', devFingerprint = '', resultSeenAt = 0;
  function $(id) { return D.$('#' + id); }
  function setText(id, s) { var e = $(id); if (e) e.textContent = s; }
  function mode() { var e = D.$('input[name="ota-transport"]:checked'); return e ? e.value : 'device_pull'; }
  function isPull(j) { return (j || job || {}).transport === 'device_pull'; }
  function fmt(n) { return n === null || n === undefined ? '—' : n < 1024 ? n + ' B' : (n / 1024).toFixed(1) + ' KB'; }
  function api(url, opt) {
    return fetch(url, opt || {}).then(function (r) {
      if (r.status === 401) { window.location.href = '/login'; throw new Error('未登录'); }
      return r.json().catch(function () { return null; }).then(function (j) {
        if (!r.ok) throw new Error((j && (j.error || j.reason || j.err)) || ('HTTP ' + r.status));
        return j || {};
      });
    });
  }

  function updateMode() {
    var pull = mode() === 'device_pull';
    D.$$('.ota-lan-only').forEach(function (e) { e.classList.toggle('hidden', pull); });
    setText('ota-target-heading', pull ? '目标设备' : '目标设备 / IP');
    setText('ota-target-note', pull
      ? '公网任务只会被这里选中的 device_id 领取。若显示离线，请先确认这块 ESP 能每秒上报云端。'
      : '局域网模式也可从这里选设备；云端无法反连 NAT 后的 ESP。');
    setText('ota-flow-title', '板上按 PE3 或 K-UP 确认');
    setText('ota-mode-note', pull
      ? 'ESP 联网领取任务，板上确认后才开始。'
      : '网页服务与 ESP 需在同一局域网。');
    updateButton();
  }

  function renderDevices(devices) {
    var sel = $('ota-devices'); if (!sel) return;
    var fp = JSON.stringify((devices || []).map(function (d) { return [d.device_id, d.ip, d.online]; }));
    if (fp === devFingerprint) return;
    devFingerprint = fp; var keep = sel.value;
    sel.innerHTML = '<option value="">— 从已知设备里选（或填写 IP）—</option>' + (devices || []).map(function (d) {
      return '<option value="' + D.escapeHtml(d.device_id) + '">' + D.escapeHtml(d.device_id) + '（'
        + D.escapeHtml(d.ip || 'IP 未知') + '，' + (d.online ? '在线' : '离线') + '）</option>';
    }).join('');
    if (keep) sel.value = keep;
  }
  function loadDevices() { return api('/api/devices').then(function (j) { renderDevices(j.devices || []); }); }

  function renderFirmware(items) {
    var box = $('ota-fw-list'); if (!box) return;
    var fp = JSON.stringify((items || []).map(function (i) { return [i.name, i.size, i.crc32, i.valid, i.reason, i.uploaded]; }));
    if (fp === fwFingerprint) return;
    fwFingerprint = fp;
    if (!items || !items.length) {
      box.className = 'empty-box';
      box.innerHTML = '固件库为空。上传一个链接到 <code>0x08008000</code> 的 F407 Slot A <code>.bin</code>。';
      updateButton(); return;
    }
    if (selectedFw && !items.some(function (i) { return i.name === selectedFw && i.valid; })) selectedFw = '';
    if (!selectedFw) { var first = items.filter(function (i) { return i.valid; })[0]; if (first) selectedFw = first.name; }
    box.className = '';
    box.innerHTML = '<table class="ota-table"><thead><tr><th></th><th>文件名</th><th>大小</th><th>CRC32</th><th>上传时间</th><th></th></tr></thead><tbody>'
      + items.map(function (i) {
        return '<tr class="' + (i.valid ? '' : 'ota-bad-row') + '"><td><input type="radio" name="ota-fw" value="' + D.escapeHtml(i.name)
          + '"' + (i.name === selectedFw ? ' checked' : '') + (i.valid ? '' : ' disabled') + '></td><td><code>' + D.escapeHtml(i.name)
          + '</code>' + (i.valid ? '' : '<div class="ota-reason">✘ ' + D.escapeHtml(i.reason) + '</div>') + '</td><td>' + fmt(i.size)
          + '</td><td><code>' + D.escapeHtml(i.crc32) + '</code></td><td>' + D.escapeHtml(i.uploaded || '—') + '</td><td><button type="button" class="qbtn-del ota-del" data-name="'
          + D.escapeHtml(i.name) + '" title="从固件库删除">×</button></td></tr>';
      }).join('') + '</tbody></table>';
    D.$$('#ota-fw-list input[name="ota-fw"]').forEach(function (e) { e.addEventListener('change', function () { selectedFw = e.value; updateButton(); }); });
    D.$$('#ota-fw-list .ota-del').forEach(function (e) { e.addEventListener('click', function () {
      if (!window.confirm('从固件库删除 ' + e.getAttribute('data-name') + '？')) return;
      api('/api/otactl/fw/' + encodeURIComponent(e.getAttribute('data-name')), {method: 'DELETE'}).then(function () { fwFingerprint = ''; return loadFirmware(); }).catch(function (x) { alert('删除失败：' + (x.message || x)); });
    }); });
    updateButton();
  }
  function loadFirmware() {
    return api('/api/otactl/fw').then(function (j) { renderFirmware(j.items || []); setText('ota-max-bytes', '单个固件上限 ' + fmt(j.max_bytes)); })
      .catch(function (x) { var b = $('ota-fw-list'); if (b) { b.className = 'hint ota-bad'; b.textContent = '读取固件库失败：' + (x.message || x); } });
  }

  function bindControls() {
    D.$$('input[name="ota-transport"]').forEach(function (e) { e.addEventListener('change', updateMode); });
    var sel = $('ota-devices'); if (sel) sel.addEventListener('change', function () { if (sel.value) $('ota-target').value = sel.value; updateButton(); });
    var pre = $('btn-ota-preflight');
    if (pre) pre.addEventListener('click', function () {
      var target = ($('ota-target').value || '').trim(), box = $('ota-preflight');
      if (!target) { box.className = 'hint ota-bad'; box.textContent = '先选择设备或填写局域网 IP'; return; }
      box.className = 'hint'; box.textContent = '预检中…';
      api('/api/otactl/preflight?target=' + encodeURIComponent(target)).then(function (j) {
        box.className = 'hint ' + (j.warn ? 'ota-warn' : 'ota-ok');
        box.innerHTML = D.escapeHtml(['目标 ' + j.esp_ip, '本机 ' + (j.host_ip || '未知'), j.same_subnet ? '同 /24 网段' : '不同网段'].join(' · ')) + (j.warn ? '<br>⚠️ ' + D.escapeHtml(j.warn) : '');
      }).catch(function (x) { box.className = 'hint ota-bad'; box.textContent = '✘ ' + (x.message || x); });
    });
    var upload = $('btn-ota-upload'), input = $('ota-fw-file');
    if (upload && input) upload.addEventListener('click', function () {
      var f = input.files && input.files[0], box = $('ota-upload-result');
      if (!f) { alert('先选一个 .bin 文件'); return; }
      var fd = new FormData(); fd.append('firmware', f, f.name);
      upload.disabled = true; upload.textContent = '上传中…'; box.className = 'hint'; box.textContent = '正在上传 ' + f.name + ' …';
      api('/api/otactl/fw', {method: 'POST', body: fd}).then(function (j) {
        box.className = 'hint ' + (j.valid ? 'ota-ok' : 'ota-bad'); box.textContent = j.valid ? '✔ 已入库 ' + j.name + '（CRC32 ' + j.crc32 + '）' : '✘ 未入库：' + (j.reason || '镜像体检不通过');
        if (j.valid) selectedFw = j.name; fwFingerprint = ''; return loadFirmware();
      }).catch(function (x) { box.className = 'hint ota-bad'; box.textContent = '✘ 上传失败：' + (x.message || x); })
        .then(function () { upload.disabled = false; upload.textContent = '上传到固件库'; input.value = ''; });
    });
  }

  function phases(j) {
    return isPull(j)
      ? [{key:'waiting_device',label:'等待 ESP 领取'}, {key:'waiting_user',label:'等 PE3 或 K-UP'}, {key:'uploading',label:'拉取并写入'}, {key:'verifying',label:'重启校验'}]
      : [{key:'probing',label:'探测设备'}, {key:'waiting_user',label:'等 PE3 或 K-UP'}, {key:'uploading',label:'写入 Flash'}, {key:'verifying',label:'重启验证'}];
  }
  function title(p, j) {
    if (p === 'waiting_device') return '等待 ESP 主动领取任务';
    if (p === 'queued') return '已排队，即将开始…';
    if (p === 'probing') return '正在探测局域网 ESP :8080…';
    if (p === 'waiting_user') return '⚠️ 请按板子上的 PE3 或 K-UP 键';
    if (p === 'uploading') return isPull(j) ? 'ESP 正在按块拉取并交给 F407 写入' : '正在上传并写入 Flash';
    if (p === 'verifying') return '镜像已提交，正在等设备重启自检…';
    if (p === 'done') return '✔ PASS —— 新程序已在运行';
    if (p === 'failed') return '✘ FAIL —— 没有完成更新';
    return p || '';
  }
  function sub(p, j) {
    if (p === 'waiting_device') return '任务已在云端等待「' + (j.target_device || '目标设备') + '」。它联网后每秒会领取一次；这里不会连接 ESP :8080，也不会写 Flash。';
    if (p === 'waiting_user') return 'F407 屏幕应该已经显示包长和 PRESS PE3 或 K-UP。现在还没有上传或擦写 Flash。';
    if (p === 'uploading' && isPull(j)) return '已领取 ' + (j.claimed_by || '设备') + '：已拉取 ' + (j.got_blocks || 0) + '/' + (j.blocks || '—') + ' 块。别断电。';
    if (p === 'uploading') return '整包正在传输；别断电、别重启 Flask。';
    if (p === 'verifying') return 'F407 正在重启并校验 Slot A。';
    if (p === 'done') return '服务端已收到 Slot A CRC 校验一致的回报。';
    return p === 'failed' ? ((j.result && j.result.message) || '任务失败，请查看日志。') : '';
  }
  function updateButton() {
    var b = $('btn-ota-flash'); if (!b) return;
    var running = !!(job && job.state === 'running');
    var targetReady = mode() !== 'device_pull' || !!(($('ota-devices') || {}).value || '');
    b.disabled = running || busy || !selectedFw || !targetReady;
    b.textContent = running ? '任务进行中…' : (mode() === 'device_pull' ? '发起公网 OTA' : '开始局域网烧录');
    var abort = $('btn-ota-abort'); if (abort) abort.classList.toggle('hidden', !running);
  }
  function renderJob() {
    if (!job) return;
    renderProgress(job);
    var ps = phases(job), idx = ps.map(function (p) { return p.key; }).indexOf(job.phase), steps = $('ota-steps');
    if (steps) steps.innerHTML = ps.map(function (p, i) {
      var c = 'ota-step';
      if (job.state === 'done') c += ' ok';
      else if (job.state === 'failed') c += i === Math.max(0, idx) ? ' bad' : (i < Math.max(0, idx) ? ' ok' : '');
      else if (i < idx) c += ' ok'; else if (i === idx || (idx < 0 && i === 0)) c += ' now';
      return '<span class="' + c + '">' + (i + 1) + '. ' + p.label + '</span>';
    }).join('<span class="ota-arrow">→</span>');
    setText('ota-phase-title', title(job.phase, job)); setText('ota-phase-sub', sub(job.phase, job));
    var card = $('ota-phase-card'); if (card) card.className = 'card ota-phase ota-phase-' + (job.state === 'running' ? job.phase : job.state);
    var log = $('ota-log'); if (log) { log.textContent = (job.log || []).map(function (x) { return '[' + x.t + '] ' + x.text; }).join('\n'); log.scrollTop = log.scrollHeight; }
    var result = $('ota-result');
    if (result && job.state !== 'running' && job.result) {
      var dest = isPull(job) ? (job.claimed_by || '设备主动拉取') : (job.esp_ip || '局域网设备');
      result.className = 'result ' + (job.result.ok ? 'ok' : 'err'); result.classList.remove('hidden');
      result.innerHTML = '<div class="head">' + (job.result.ok ? '✔ PASS' : '✘ FAIL') + ' · ' + D.escapeHtml(job.name || '') + ' → ' + D.escapeHtml(dest) + '</div><pre>' + D.escapeHtml(job.result.message || '') + '</pre>';
    }
    updateButton();
  }
  function renderProgress(j) {
    var bar = $('ota-progress'); if (!bar) return;
    var blocks = Number(j.blocks), got = Number(j.got_blocks || 0);
    var measured = isPull(j) && blocks > 0 && Number.isFinite(got);
    var percent = measured ? Math.max(0, Math.min(100, Math.floor(got / blocks * 100))) : 0;
    var done = j.state === 'done' && j.result && j.result.ok === true;
    var failed = j.state !== 'running' && !done;
    var transferring = j.phase === 'uploading' || j.phase === 'verifying';
    bar.classList.toggle('failed', failed);
    if (done) { bar.value = 100; setText('ota-progress-value','100% · 校验通过'); }
    else if (measured) { bar.value = percent; setText('ota-progress-value',percent + '% · ' + got + '/' + blocks + ' 块'); }
    else if (j.state === 'running' && transferring) { bar.removeAttribute('value'); setText('ota-progress-value','进行中'); }
    else { bar.value=0; setText('ota-progress-value',failed ? '未完成' : '尚未传输'); }
    setText('ota-progress-label',done ? '更新完成' : failed ? '更新未完成 / 已取消' : transferring ? '固件传输进度' : title(j.phase,j));
    setText('ota-progress-note',done ? '设备重启后回报的 Slot A CRC 校验一致，确认更新成功。'
      : failed ? '进度只表示已传输的数据，未收到成功校验；请查看下方结果与日志。'
      : j.phase === 'waiting_user' ? '等待板上 PE3 或 K-UP 确认；尚未开始传输和擦写 Flash。'
      : j.phase === 'waiting_device' || j.phase === 'queued' ? '等待目标设备领取任务；尚未开始传输和擦写 Flash。'
      : j.phase === 'verifying' || (measured && got >= blocks) ? '数据已传输，仍需等待设备重启与 CRC 校验；当前还不能认定升级成功。'
      : measured ? '按 ESP 已拉取块数计算传输进度；最终以设备重启校验结果为准。'
      : '局域网整包传输不提供分块百分比，传输时显示活动进度；等待实体按键期间不会虚增进度。');
  }
  function jobUrl() { return (isPull(job) ? '/api/iapl/job/' : '/api/otactl/job/') + encodeURIComponent(job.id); }
  function poll() {
    if (!job) return;
    api(jobUrl()).then(function (j) {
      job = j.job; resultSeenAt = Date.now(); renderJob();
      if (job.state === 'running') jobTimer = setTimeout(poll, POLL_MS);
      else { jobTimer = null; busy = false; fwFingerprint = ''; updateButton(); loadDevices(); }
    }).catch(function (x) { var l = $('ota-log'); if (l) l.textContent += '\n[页面] 读取任务失败：' + (x.message || x); jobTimer = setTimeout(poll, POLL_MS * 2); });
  }
  function bindJob() {
    var start = $('btn-ota-flash'), abort = $('btn-ota-abort');
    if (start) start.addEventListener('click', function () {
      var pull = mode() === 'device_pull', target = ($('ota-target').value || '').trim();
      var targetDevice = (($('ota-devices') || {}).value || '').trim();
      if (!selectedFw) { alert('先在固件库里选一个固件'); return; }
      if (pull && !targetDevice) { alert('先在“目标设备”中选择要测试的 ESP'); return; }
      if (!pull && !target) { alert('局域网直连需要填写目标设备 IP 或名字'); return; }
      if (!window.confirm(pull ? '创建公网 OTA 任务：' + selectedFw + ' → ' + targetDevice + '？\n\n只有这块 ESP 会领取；F407 出现 PRESS PE3 或 K-UP 后才会写入。' : '把 ' + selectedFw + ' 烧到 ' + target + '？\n\nF407 出现 PRESS PE3 或 K-UP 后才会写入。')) return;
      busy = true; updateButton(); var result = $('ota-result'); result.className = 'result hidden'; result.innerHTML = '';
      api(pull ? '/api/iapl/job' : '/api/otactl/flash', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(pull ? {name:selectedFw,device_id:targetDevice} : {target:target,name:selectedFw})}).then(function (j) {
        job = {id:j.job_id, state:'running', phase:pull ? 'waiting_device' : 'queued', transport:j.transport || (pull ? 'device_pull' : 'lan_direct'), name:j.name || selectedFw, target_device:j.target_device || targetDevice, bytes:j.bytes, crc32:j.crc32, blocks:j.blocks || 0, got_blocks:0, log:[], result:null};
        renderJob(); if (jobTimer) clearTimeout(jobTimer); jobTimer = setTimeout(poll, POLL_MS);
      }).catch(function (x) { busy = false; updateButton(); result.className = 'result err'; result.classList.remove('hidden'); result.textContent = '✘ 没能开始任务：' + (x.message || x); });
    });
    if (abort) abort.addEventListener('click', function () {
      if (!job || job.state !== 'running' || !window.confirm('取消当前任务？未开始写入的公网任务会直接作废。')) return;
      api(jobUrl() + '/abort', {method:'POST'}).then(function (j) { job = j.job; busy = false; if (jobTimer) clearTimeout(jobTimer); jobTimer = null; renderJob(); }).catch(function (x) { alert('取消失败：' + (x.message || x)); });
    });
  }
  // 任务在服务端线程里跑：刷新/切页回来要把「在跑的那个」接回来，否则页面永远
  // 等不到 PASS（板子照样更新完，这里却还是一片未开始）。
  function adopt(j) {
    if (!j || !j.id || job) return false;
    job = j; resultSeenAt = Date.now();
    if (j.state === 'running') { busy = true; if (jobTimer) clearTimeout(jobTimer); jobTimer = setTimeout(poll, POLL_MS); }
    renderJob();
    return true;
  }
  function recent(j) {
    if (!j) return false;
    if (j.state === 'running') return true;
    var t = Date.parse(String(j.finished || j.started || '').replace(' ', 'T'));
    return isFinite(t) && (Date.now() - t) < 30 * 60 * 1000;
  }
  function resume() {
    if (job) return;
    api('/api/otactl/jobs').then(function (r) {
      var items = (r && r.jobs) || [];
      var pick = items.filter(function (j) { return j.state === 'running'; })[0] || items[0];
      if (pick && recent(pick)) adopt(pick);
    }).catch(function () {});
    api('/api/iapl/jobs').then(function (r) {
      if (job) return;
      var items = (r && r.jobs) || [];
      var pick = items.filter(function (j) { return j.state === 'running'; })[0] || items[0];
      if (pick && recent(pick)) adopt(pick);
    }).catch(function () {});
  }
  function init() {
    bindControls(); bindJob(); updateMode(); loadFirmware(); loadDevices(); updateButton(); resume();
    window.addEventListener('beforeunload', function (e) { if (job && job.state === 'running') { e.preventDefault(); e.returnValue = 'OTA 任务仍在进行。'; return e.returnValue; } });
  }
  function refresh() { return (job && job.state === 'running') || busy || (resultSeenAt && Date.now() - resultSeenAt < 30000) ? Promise.resolve() : Promise.all([loadFirmware(), loadDevices()]); }
  window.PAGES = window.PAGES || {}; window.PAGES.ota = {title:'远程OTA', init:init, refresh:refresh};
})();
