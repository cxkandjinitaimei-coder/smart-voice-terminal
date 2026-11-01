(function () {
  'use strict';
  const D = window.DASH_API;
  let busy = false;
  const result = text => { D.$('#timeline-result').textContent = text; };
  const input = () => JSON.parse(D.$('#timeline-editor').value);
  async function preview(timeline) {
    const j = await D.postJSON('/api/timeline/preview', { timeline, device_id: 'esp-bridge' });
    if (!j.ok) throw new Error(j.error || '时间表无效');
    return j;
  }
  async function fill() {
    const kind = D.$('#timeline-example').value;
    const repeat = Number(D.$('#timeline-repeat').value);
    const speed = Number(D.$('#timeline-speed').value);
    const gap = Math.round(Number(D.$('#timeline-gap').value) * 1000);
    let timeline;
    if (kind === 'faces') {
      const library = await D.getJSON('/api/actions');
      const faces = (library.expressions || []).slice(0, 3);
      if (!faces.length) throw new Error('请先在管理员页面登记表情');
      timeline = { repeat, duration_ms: faces.length * gap, steps: faces.map((f, i) =>
        ({ at_ms: i * gap, move: { type: 'expression', face: f.id } })) };
    } else {
      const move = name => ({ type: 'gesture', name, speed });
      timeline = { repeat, steps: [{ at_ms: 0, move: move(kind === 'mixed' ? 'nod' : kind) }] };
      if (kind === 'mixed') {
        const first = await preview({ repeat: 1, steps: timeline.steps });
        timeline.steps.push({ at_ms: first.timeline.duration_ms, move: move('shake') });
      }
    }
    const checked = await preview(timeline);
    // Retain semantic gestures so server always recompiles from current limits.
    timeline.duration_ms = checked.timeline.duration_ms;
    D.$('#timeline-editor').value = JSON.stringify(timeline, null, 2);
    result(`时间表已生成：${checked.steps} 步，周期 ${timeline.duration_ms / 1000} 秒，重复 ${repeat} 次。尚未下发。`);
  }
  let initialized = false;
  function init() {
    if (!D.$('#timeline-editor')) return;
    if (initialized) return;
    initialized = true;
    ['fill', 'preview', 'send', 'stop'].forEach(action => {
      D.$('#timeline-' + action).addEventListener('click', async () => {
        if (busy) return;
        busy = true;
        const button = D.$('#timeline-' + action); button.disabled = true;
        try {
          if (action === 'fill') await fill();
          else if (action === 'preview') {
            const j = await preview(input());
            result(`检查通过：${j.steps} 步，周期 ${j.timeline.duration_ms / 1000} 秒，重复 ${j.timeline.repeat} 次。尚未下发。`);
          } else {
            const j = await D.postJSON('/api/command', { device_id: 'esp-bridge',
              timeline: action === 'stop' ? { stop: true } : input() });
            if (!j.ok) throw new Error(j.error || '未入队');
            result(action === 'stop' ? '停止请求已入队，等待设备确认。' : `整段已入队（${j.queue_len}），等待 F407 执行状态。`);
          }
        } catch (e) { result('未下发：' + e.message); }
        finally { busy = false; button.disabled = false; }
      });
    });
    fill().catch(e => result('生成示范失败：' + e.message));
  }
  function status(device) {
    const box = D.$('#timeline-status'); if (!box) return;
    if (!device || !device.online) { box.textContent = '设备离线，时间任务状态待确认'; return; }
    const m = device.motion || {}, parts = String(m.state || '').split(',');
    const states = { running: '执行中', paused: '录音/升级暂停中', done: '已完成指令执行',
      stopped: '已停止', invalid: '设备拒绝：时间表无效', limits_error: '设备拒绝：角度超出限位' };
    const state = states[parts[0]] || '暂无时间任务回执';
    const home = m.home === 'waiting_profile' ? '等待配置后归位' : (m.home ? '归位目标 ' + m.home : '暂无归位回执');
    box.textContent = `F407：${state}${parts.length >= 6 ? `，步骤 ${parts[2]}/${parts[3]}，轮次 ${parts[4]}/${parts[5]}` : ''}；舵机限位：${m.limits_applied ? '已同步到 MCU' : '等待 MCU 确认'}；ESP断电缓存：${m.profile_cached ? '已保存' : '待确认'}；${home}。`;
  }
  window.MOTION_TIMELINE = { init, status };
})();
