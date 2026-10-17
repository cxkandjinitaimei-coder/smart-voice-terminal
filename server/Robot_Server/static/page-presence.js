/**
 * @file page-presence.js
 * 用途与使用：相机拍照、人脸人员库、识别结果与历史照片；请求/采集/上传/分析分阶段显示。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
/* Real camera workspace. No simulated device frames or invented analysis. */
(function () {
  'use strict';
  const D = window.DASH_API, $ = id => document.getElementById('camera-' + id);
  const STATES = {queued:'等待机器人领取拍照请求',capturing:'等待拍照上传',stored:'旧记录 · 点击重新检测',analyzing:'正在本地检测人脸…',done:'人脸检测完成',failed:'检测失败',expired:'上传超时',cancelled:'任务已取消'};
  let stream = null, busy = false, loading = false, settings = null, items = [], selected = '', dirty = false, generation = 0, activeDevice = '';
  let faceState = null, faceDirty = false, enrolling = false;
  const message = text => { $('message').textContent = text; };
  const endpoint = path => '/api/camera/' + path + '?device_id=' + encodeURIComponent(activeDevice || $('device').value.trim());
  function date(sec) { return new Date(sec * 1000).toLocaleString('zh-CN'); }
  async function request(url, options) {
    const controller = new AbortController(), timer = setTimeout(() => controller.abort(), 20000);
    try {
      const response = await fetch(url, {...options, signal:controller.signal});
      if (response.status === 401) { location.href = '/login'; throw Error('登录已过期'); }
      const value = await response.json();
      if (!response.ok || value.error) throw Error(value.error || '请求失败 ' + response.status);
      return value;
    } finally { clearTimeout(timer); }
  }
  const post = (url, data) => request(url, {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data || {})});
  function readSettings() {
    const windows = $('windows').value.split(/\n/).map(s => s.trim()).filter(Boolean).map(line => {
      const parts = line.split(/\s*[-—~～]\s*/);
      if (parts.length !== 2) throw Error('时段格式为 08:00-12:00，每行一个');
      return {start:parts[0],end:parts[1]};
    });
    return {enabled:$('enabled').checked,paused:settings ? settings.paused : false,source:$('source').value,
      interval_sec:Number($('interval').value),windows,analyze:true,provider:'local-face',
      horizontal_fov_deg:Number($('fov-h').value),vertical_fov_deg:Number($('fov-v').value),
      keep_count:Number($('keep').value),block_audio:$('block-audio').checked,block_ota:$('block-ota').checked,block_motion:$('block-motion').checked};
  }
  function setForm(s) {
    $('enabled').checked = s.enabled; $('source').value = s.source; $('interval').value = s.interval_sec;
    $('windows').value = s.windows.map(w => w.start + '-' + w.end).join('\n');
    $('keep').value = s.keep_count;
    $('fov-h').value = s.horizontal_fov_deg; $('fov-v').value = s.vertical_fov_deg;
    ['audio','ota','motion'].forEach(k => { $('block-' + k).checked = s['block_' + k]; });
  }
  function showSelected() {
    const item = items.find(i => i.id === selected);
    $('export').disabled = !items.length;
    $('photo').hidden = !item || !item.image_url; $('download').hidden = !item || !item.image_url;
    $('retry').disabled = !item || !item.image_url || item.state === 'analyzing';
    $('delete').disabled = !item || ['queued','capturing','analyzing'].includes(item.state);
    $('enroll-current').disabled = !item || !item.image_url || enrolling;
    $('boxes').replaceChildren();
    showTarget(item && item.analysis);
    if (!item) { $('detail').textContent = '上传图片后，将在这里显示真实处理结果。'; $('summary').textContent = ''; return; }
    $('detail').textContent = date(item.created) + ' · ' + (STATES[item.state] || item.state) + (item.width ? ' · ' + item.width + ' × ' + item.height : '') + (item.source === 'manual' ? ' · 手动上传测试' : '');
    if (item.image_url) {
      if ($('image').getAttribute('src') !== item.image_url) $('image').src = item.image_url;
      $('download').href = item.image_url; $('download').download = 'camera-' + item.id + '.jpg';
    }
    const result = item.analysis;
    $('summary').textContent = item.error || (result ? result.summary + (result.engine ? '\n' + result.engine + ' · ' + result.elapsed_ms + ' ms' : '\n旧版记录，请点重新检测使用本地人脸模型。') : item.state === 'queued' ? '请求已保存，等待机器人联网领取；无需保持本页打开。' : item.state === 'capturing' ? '机器人正在采集或上传照片，完成后会自动显示。' : item.state === 'analyzing' ? '图片已上传，正在本地检测和比对，请稍等…' : '点击“重新检测 / 识别”使用本地人脸模型处理这张旧图片。');
    (result && result.engine && result.boxes || []).forEach(box => {
      const el = document.createElement('span'); el.style.cssText = `left:${box.x*100}%;top:${box.y*100}%;width:${box.w*100}%;height:${box.h*100}%`;
      el.className = (box.recognized ? 'face-known' : 'face-unknown') + (box.selected ? ' face-selected' : '');
      const label = document.createElement('b'); label.textContent = (box.selected ? '★ 目标 · ' : '') + (box.name || '人脸') + (box.similarity != null ? ' · 相似度 ' + box.similarity.toFixed(2) : ''); el.append(label); $('boxes').append(el);
    });
    if (result && result.frame_size) {
      const origin = document.createElement('i'); origin.className = 'camera-origin'; origin.style.cssText = 'left:50%;top:50%'; $('boxes').append(origin);
      if (result.target) { const point=document.createElement('i'); point.className='camera-target-point'; point.style.left=(100*result.target.center_px[0]/result.frame_size[0])+'%'; point.style.top=(100*result.target.center_px[1]/result.frame_size[1])+'%'; $('boxes').append(point); }
    }
  }
  const signed = n => (n>0 ? '+' : '') + Number(n).toFixed(2);
  function showTarget(result) {
    const target = result && result.target;
    $('target-values').replaceChildren(); $('offset-list').replaceChildren();
    $('target-copy').disabled=!target;
    $('target-json').textContent=JSON.stringify(result && result.frame_size ? {target:target||null,frame_size:result.frame_size,angle_config:result.angle_config,target_policy:result.target_policy} : null,null,2);
    $('target-state').textContent=!result ? '上传照片后显示；已匹配人脸中选择相似度最高的一张作为目标。' : !result.frame_size ? '旧记录尚无角度数据，请点重新检测。' : target ? '当前目标：'+target.name+' · 相似度 '+target.similarity.toFixed(4)+' · 已选择最高相似度匹配' : result.target_reason;
    if (target) {
      for (const [label,value] of [['目标人脸',target.name+' · #'+target.index],['中心偏差（右 / 下）',target.offset_px.map(signed).join(' / ')+' px'],['水平修正（右为正）',signed(target.angles_deg.yaw)+'°'],['垂直修正（上为正）',signed(target.angles_deg.pitch)+'°']]) {
        const card=document.createElement('div'), title=document.createElement('small'), text=document.createElement('strong');title.textContent=label;text.textContent=value;card.append(title,text);$('target-values').append(card);
      }
    }
    for (const face of result && result.faces || []) {
      if (!face.offset_px) continue;
      const row=document.createElement('p');row.textContent=(target && face.index===target.index ? '★ ' : '')+'#'+face.index+' '+face.name+' · 中心 '+face.center_px.join(', ')+' px · 偏差 '+face.offset_px.map(signed).join(', ')+' px · 水平 '+signed(face.angles_deg.yaw)+'° / 垂直 '+signed(face.angles_deg.pitch)+'°';$('offset-list').append(row);
    }
  }
  function renderHistory() {
    const host = $('history'); host.replaceChildren();
    if (!items.length) { const p = document.createElement('p'); p.className = 'hint'; p.textContent = '暂无采集记录'; host.append(p); }
    items.forEach(item => {
      const b = document.createElement('button'); b.type = 'button'; b.className = 'camera-record'; b.setAttribute('aria-pressed', String(selected === item.id));
      if (item.image_url) { const img = document.createElement('img'); img.src = item.image_url; img.alt = ''; img.loading = 'lazy'; b.append(img); }
      const line = document.createElement('div'), title = document.createElement('strong'), meta = document.createElement('small');
      title.textContent = STATES[item.state] || item.state; meta.textContent = date(item.created) + ' · ' + (item.source === 'manual' ? '上传测试' : item.source === 'browser' ? '浏览器' : '机器人');
      line.append(title,meta); b.append(line); b.onclick = () => { selected = item.id; renderHistory(); showSelected(); }; host.append(b);
    });
  }
  async function refresh(force) {
    if (loading) return;
    loading = true;
    try {
      const value = await request(endpoint('status'));
      settings = value.settings;
      $('hardware').textContent = value.hardware_ready ? 'ESP 拍照接口在线 · 请求时初始化相机' : '机器人相机尚未接入';
      $('policy').textContent = value.reason;
      if (force || !dirty) {
        setForm(settings);
      }
      renderFaces(value.face_engine);
      $('pause').disabled = false; $('pause').textContent = settings.paused ? '恢复采集' : '暂停采集';
      $('config-state').textContent = dirty ? '有未保存的设置，采集仍按已保存配置执行。' : settings.enabled ? (settings.paused ? '采集已暂停' : '配置已启用。相机未运行时不会采集。') : '自动采集关闭；可点“机器人拍一张”或上传照片。';
      const signature = JSON.stringify(value.items), old = JSON.stringify(items);
      items = value.items;
      if (!selected || !items.some(i => i.id === selected)) selected = items[0] ? items[0].id : '';
      if (signature !== old || force) { renderHistory(); showSelected(); }
      $('snap').disabled = !stream || busy;
    } finally { loading = false; }
  }
  function renderFaces(state) {
    faceState = state;
    $('engine').textContent = state && state.ready ? '本地人脸引擎已就绪 · YuNet + SFace / CPU · 上传照片自动检测' : state && state.error || '本地人脸模型尚未就绪';
    if (!state || !state.ready) return;
    const select = $('person'), previous = select.value;
    const signature = JSON.stringify(state.people);
    if (select.dataset.signature !== signature) {
      select.replaceChildren(new Option('新增人员',''));
      state.people.forEach(p => select.add(new Option(p.name + ' · ' + p.photos + ' 张参考照',p.id)));
      select.value = state.people.some(p => p.id === previous) ? previous : '';
      select.dataset.signature = signature;
    }
    $('person-delete').disabled = !select.value || enrolling;
    $('people-status').textContent = '已登记 ' + state.people.length + ' 人。未达到匹配阈值的人脸显示为未知人员。';
    if (!faceDirty) { $('face-threshold').value = state.settings.threshold; $('face-margin').value = state.settings.margin; }
  }
  async function cameras() {
    if (!navigator.mediaDevices || !navigator.mediaDevices.enumerateDevices) throw Error('当前浏览器无法列出摄像头；请使用 HTTPS 或 localhost。');
    const devices = (await navigator.mediaDevices.enumerateDevices()).filter(d => d.kind === 'videoinput');
    const select = $('camera-select'), previous = select.value;
    select.replaceChildren(new Option(devices.length ? '系统默认摄像头' : '未发现电脑摄像头',''));
    devices.forEach((d,i) => select.add(new Option(d.label || '摄像头 ' + (i+1),d.deviceId)));
    if (devices.some(d => d.deviceId === previous)) select.value = previous;
    return devices.length;
  }
  async function enroll(current) {
    if (enrolling) return;
    const name = $('person-name').value.trim();
    if (!name) throw Error('请填写人员姓名');
    const data = new FormData(); data.append('name',name); if ($('person').value) data.append('person_id',$('person').value);
    if (current) {
      const item = items.find(i => i.id === selected); if (!item || !item.image_url) throw Error('请先上传一张照片');
      const response = await fetch(item.image_url); if (!response.ok) throw Error('读取登记图片失败');
      data.append('photos',await response.blob(),'reference.jpg');
    } else {
      const files = Array.from($('enroll-files').files); if (!files.length || files.length>10) throw Error('请选择 1~10 张单人清晰照片');
      for (const file of files) { if (file.size>4*1024*1024) throw Error('每张登记照片最多 4 MiB'); data.append('photos',file); }
    }
    enrolling=true; $('enroll').disabled=true; $('enroll-current').disabled=true;
    try {
      const state = await request('/api/camera/faces/enroll',{method:'POST',body:data}); renderFaces({...state,ready:true});
      $('enroll-files').value=''; message('人员已登记。点击“重新检测 / 识别”即可用更新的人脸库比对当前照片。');
    } finally { enrolling=false; $('enroll').disabled=false; showSelected(); }
  }
  function stop() {
    generation++;
    if (stream) stream.getTracks().forEach(track => track.stop());
    stream = null; $('video').srcObject = null; $('video').hidden = true; $('placeholder').hidden = false;
    $('stop').disabled = true; $('start').disabled = false; $('snap').disabled = true; $('live-state').textContent = '相机已关闭';
  }
  async function start() {
    if (!window.isSecureContext || !navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) throw Error('浏览器相机需要 HTTPS 或 localhost；当前可先上传照片测试。');
    const mine = ++generation; $('start').disabled = true;
    try {
      const device = $('camera-select').value;
      const media = await navigator.mediaDevices.getUserMedia({video:{width:{ideal:640},height:{ideal:480},...(device ? {deviceId:{exact:device}} : {})},audio:false});
      if (mine !== generation || D.currentTab() !== 'presence' || document.hidden) { media.getTracks().forEach(t => t.stop()); return; }
      stream = media; $('video').srcObject = stream; $('video').hidden = false; $('placeholder').hidden = true;
      await $('video').play(); $('stop').disabled = false; $('snap').disabled = false; $('live-state').textContent = '浏览器相机已打开';
      await cameras();
      message('相机已打开。要定时采集，请选择“浏览器摄像头”、启用定时拍照并保存。');
    } catch (error) { if (mine === generation) stop(); if (error.name==='NotFoundError') throw Error('没有检测到电脑摄像头。接入 USB 摄像头后点击“查找摄像头”；现在可以先上传照片。'); if (error.name==='NotAllowedError') throw Error('摄像头权限未允许，请在浏览器中授权。也可以先上传照片测试。'); throw error; }
    finally { if (!stream) $('start').disabled = false; }
  }
  async function upload(blob, lease) {
    if (blob.size > 4 * 1024 * 1024) throw Error('图片超过 4 MiB');
    const data = new FormData(); data.append('image',blob,'capture.jpg');
    const url = endpoint('upload') + (lease ? '&job_id=' + encodeURIComponent(lease.job_id) : '');
    const value = await request(url,{method:'POST',body:data,headers:lease ? {'X-Capture-Ticket':lease.ticket} : {}});
    selected = value.item.id; message('图片已上传，正在本地检测人脸…'); await refresh();
  }
  async function snap(automatic) {
    if (busy || !stream || !$('video').videoWidth) return;
    busy = true; $('snap').disabled = true;
    try {
      const value = await post(endpoint('capture'));
      if (!value.accepted) { $('policy').textContent = value.reason; if (!automatic) message(value.reason); return; }
      if (!stream || document.hidden || D.currentTab() !== 'presence') return;
      const video = $('video'), canvas = document.createElement('canvas');
      const ratio = Math.min(1,1280/video.videoWidth); canvas.width = Math.round(video.videoWidth*ratio); canvas.height = Math.round(video.videoHeight*ratio);
      canvas.getContext('2d').drawImage(video,0,0,canvas.width,canvas.height);
      const blob = await new Promise(resolve => canvas.toBlob(resolve,'image/jpeg',.85));
      if (!blob) throw Error('浏览器生成图片失败');
      await upload(blob,value);
    } catch (error) { message(error.message); }
    finally { busy = false; $('snap').disabled = !stream; }
  }
  function safe(fn) { return async function (event) { if (event) event.preventDefault(); try { await fn(event); } catch (error) { message(error.name === 'AbortError' ? '请求超时，请稍后重试' : error.message); } }; }
  function init() {
    activeDevice = $('device').value.trim();
    const visibilityKey='companion.camera.inputs.v1';
    let visibility={manual:true,device:true,pc:true};
    try {const saved=JSON.parse(localStorage.getItem(visibilityKey)||'{}');for(const key of Object.keys(visibility))if(typeof saved[key]==='boolean')visibility[key]=saved[key];}catch(_){}
    function updateVisibility(){
      const entries={manual:'camera-upload-entry',device:'camera-device-entry',pc:'camera-pc-panel'};
      for(const key of Object.keys(entries)){
        document.getElementById(entries[key]).hidden=!visibility[key];
        document.querySelector('[data-camera-jump="'+(key==='pc'?'camera-pc-entry':entries[key])+'"]').hidden=!visibility[key];
        document.querySelector('[data-camera-visible="'+key+'"]').checked=visibility[key];
      }
      if(!visibility.pc)stop();
    }
    document.querySelectorAll('[data-camera-visible]').forEach(box=>box.onchange=()=>{
      visibility[box.dataset.cameraVisible]=box.checked;updateVisibility();
      try{localStorage.setItem(visibilityKey,JSON.stringify(visibility));}catch(_){}
      if(!visibility.pc)message('电脑摄像头已隐藏并关闭；机器人相机和已保存的采集配置不受影响。');
    });
    updateVisibility();
    document.querySelectorAll('[data-camera-jump]').forEach(button => button.onclick=()=>document.getElementById(button.dataset.cameraJump).scrollIntoView({behavior:'smooth',block:'center'}));
    $('settings').addEventListener('input',() => { dirty = true; $('config-state').textContent = '有未保存的设置，采集仍按已保存配置执行。'; });
    $('settings').onsubmit = safe(async () => { const value = await post(endpoint('settings'),readSettings()); settings = value.settings; dirty = false; message('设置已保存'); await refresh(true); });
    $('load').onclick = safe(async () => { if (busy) throw Error('请等待当前图片上传结束'); stop(); activeDevice = $('device').value.trim(); dirty = false; await refresh(true); message('已读取设备配置'); });
    $('pause').onclick = safe(async () => { if (!settings) return; await post(endpoint('settings'),{...settings,paused:!settings.paused}); await refresh(); message(settings.paused ? '已暂停新采集；进行中的分析会正常结束。' : '已恢复采集'); });
    $('device-snap').onclick = safe(async () => { const result=await post(endpoint('request-device')); selected=result.job_id; message('拍照请求已排队，等待机器人领取、采集并上传。'); await refresh(); });
    $('refresh').onclick = safe(() => refresh()); $('start').onclick = safe(start); $('stop').onclick = stop; $('snap').onclick = () => snap(false);
    $('file').onchange = safe(async () => { const file = $('file').files[0]; if (!file) return; if (busy) throw Error('请等待当前上传结束'); busy = true; try { await upload(file); } finally { busy = false; $('file').value = ''; } });
    $('retry').onclick = safe(async () => { await post('/api/camera/jobs/' + selected + '/retry'); message('正在本地重新检测和比对…'); await refresh(); });
    $('camera-list').onclick = safe(async () => { const n=await cameras(); message(n ? '找到 '+n+' 个摄像头；选择后点击打开。' : '没有发现摄像头。接入 USB 摄像头后重试，现在可先上传照片。'); });
    $('enroll').onclick=safe(() => enroll(false)); $('enroll-current').onclick=safe(() => enroll(true));
    $('person').onchange=() => { const person=faceState && faceState.people.find(p=>p.id===$('person').value); $('person-name').value=person ? person.name : ''; $('person-delete').disabled=!person; };
    $('person-delete').onclick=safe(async () => { if (!$('person').value) return; if (!confirm('删除所选人员及其参考照片？')) return; const state=await request('/api/camera/faces/people/'+$('person').value,{method:'DELETE'});renderFaces({...state,ready:true});$('person-name').value='';message('人员及参考照片已删除；历史识别记录保留，重新检测可刷新结果。'); });
    ['face-threshold','face-margin'].forEach(id=>$(id).oninput=()=>{faceDirty=true;});
    $('face-save').onclick=safe(async()=>{const state=await post('/api/camera/faces/settings',{threshold:Number($('face-threshold').value),margin:Number($('face-margin').value)});faceDirty=false;renderFaces({...state,ready:true});message('识别阈值已保存');});
    $('delete').onclick = safe(async () => { await request('/api/camera/jobs/' + selected,{method:'DELETE'}); selected = ''; message('记录和图片已删除'); await refresh(); });
    $('export').onclick = () => { const blob = new Blob([JSON.stringify({device_id:activeDevice,timezone:'Asia/Shanghai',items},null,2)],{type:'application/json'}); const url = URL.createObjectURL(blob), a = document.createElement('a'); a.href=url; a.download='camera-records.json'; a.click(); setTimeout(() => URL.revokeObjectURL(url),1000); };
    $('target-copy').onclick=safe(async()=>{await navigator.clipboard.writeText($('target-json').textContent);message('目标偏差与角度 JSON 已复制');});
    window.addEventListener('tabchange',event => { if (event.detail.tab !== 'presence') stop(); });
    document.addEventListener('visibilitychange',() => { if (document.hidden) stop(); });
    window.addEventListener('pagehide',stop);
    setInterval(() => { if (stream && settings && settings.enabled && !settings.paused && settings.source === 'browser' && !document.hidden && D.currentTab() === 'presence') snap(true); },1000);
    setInterval(() => { if (!document.hidden && D.currentTab()==='presence') refresh().catch(e=>message(e.message)); },1500);
  }
  window.PAGES.presence = {title:'人员相机',init,refresh:() => refresh().catch(error => message(error.message))};
})();
