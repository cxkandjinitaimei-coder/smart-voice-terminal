/* S3 连续预览入口，独立于电脑摄像头和历史记录。
 * 自动挂到人员相机页；开始/停止修改服务端采集策略，关闭页面不停止机器人。
 * 请求串行且有超时；旧设备/旧请求结果不覆盖新设备，Blob URL 使用后释放。
 * 最新画面与识别画面分开：框只画在实际分析过的那一帧上。
 */
(function () {
  'use strict';
  const $ = id => document.getElementById(id);
  let running=false, current='', latest=0, analyzed=0, epoch=0;
  async function fetchBounded(url, options) {
    const c=new AbortController(), t=setTimeout(()=>c.abort(),6000);
    try {const r=await fetch(url,{...options,signal:c.signal,cache:'no-store'}); if(!r.ok)throw Error('HTTP '+r.status); return await (options && options.image ? r.blob() : r.json());}
    finally {clearTimeout(t);}
  }
  const device=()=>($('camera-device').value.trim() || 'esp-bridge');
  const path=(name,d=device())=>'/api/camera/'+name+'?device_id='+encodeURIComponent(d);
  function text(s){$('s3-live-message').textContent=s;}
  async function control(start) {
    if(running)return;
    running=true;
    try {
      const d=device(), v=await fetchBounded(path('status',d));
      const settings={...v.settings, enabled:start, paused:false};
      if(start)Object.assign(settings,{capture_mode:'continuous',source:'device',camera_transport:'s3-wifi',continuous_fps:Number($('s3-live-fps').value)});
      await fetchBounded(path('settings',d),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(settings)});
      // 让原设置表重新读取，避免保存旧表单把连续模式覆盖回去。
      $('camera-load').click();
      text(start?'已开启连续上传，等待 ESP32-S3 连接。跟随使用下方单独开关。':'已停止连续采集；正在执行的旧识别结果不会再控制舵机。');
    }catch(e){text('操作失败：'+e.message);}finally{running=false;}
  }
  async function picture(name,d,mine) {
    const blob=await fetchBounded(path('live/image',d)+(name==='analyzed'?'&kind=analyzed':''),{image:true});
    if(mine!==epoch)return null;
    const im=new Image(), url=URL.createObjectURL(blob);
    try {im.src=url;await im.decode();return im;}finally{URL.revokeObjectURL(url);}
  }
  async function tick() {
    try {
      if(document.hidden || window.DASH_API.currentTab()!=='presence')return;
      const d=device();
      if(d!==current){current=d;latest=analyzed=0;epoch++;$('s3-live-view').hidden=true;$('s3-live-detection').hidden=true;}
      const mine=epoch, s=await fetchBounded(path('live/status',d));
      if(mine!==epoch || d!==device())return;
      const fresh=s.active && s.age_sec!==null && s.age_sec<5;
      $('s3-live-stats').textContent=(fresh?'连续画面在线':'等待连续画面 / 已停止')+' · 收到 '+s.received+' 帧 · 已识别 '+s.analyzed+' 帧 · 跳过旧帧 '+s.dropped+(s.age_sec!==undefined && s.age_sec!==null?' · 最新 '+s.age_sec+' 秒前':'');
      if(!fresh){$('s3-live-view').hidden=true;$('s3-live-detection').hidden=true;latest=analyzed=0;return;}
      if(s.sequence!==latest){
        const im=await picture('latest',d,mine);
        if(im && d===device()){const c=$('s3-live-view');c.width=im.width;c.height=im.height;c.getContext('2d').drawImage(im,0,0);c.hidden=false;latest=s.sequence;}
      }
      if(s.analysis_sequence && s.analysis_sequence!==analyzed){
        // 取帧和状态之间可能又完成识别：端点绑定帧序号，避免框与图片错配。
        const blob=await fetchBounded(path('live/image',d)+'&kind=analyzed&sequence='+s.analysis_sequence,{image:true});
        const im=await createImageBitmap(blob);
        try {
          if(mine!==epoch || d!==device())return;
          const c=$('s3-live-detection');c.width=im.width;c.height=im.height;const ctx=c.getContext('2d');ctx.drawImage(im,0,0);
          for(const b of s.analysis && s.analysis.boxes || []){
            ctx.strokeStyle=b.recognized?'#6bffb7':'#ffcb6b';ctx.lineWidth=2;ctx.strokeRect(b.x*c.width,b.y*c.height,b.w*c.width,b.h*c.height);
            ctx.fillStyle=ctx.strokeStyle;ctx.font='14px sans-serif';ctx.fillText(b.name||'未知人员',b.x*c.width,Math.max(16,b.y*c.height-4));
          }
          c.hidden=false;analyzed=s.analysis_sequence;
          $('s3-live-result').textContent=s.error || (s.analysis ? s.analysis.summary+' · '+s.elapsed_ms+' ms · 分析帧 #'+s.analysis_sequence : '等待识别');
        }finally{im.close();}
      }
    }catch(e){if(e.message!=='HTTP 409')text('预览等待重连：'+e.message);}
    finally{setTimeout(tick,250);}
  }
  function init() {
    const host=$('presence-camera-slot');if(!host)return;
    const panel=document.createElement('section');panel.className='card';panel.id='s3-live-panel';
    panel.innerHTML='<h2>S3 连续画面与识别</h2><div class="camera-toolbar"><label>目标帧率 <select id="s3-live-fps"><option value="1">1</option><option value="2" selected>2</option><option value="3">3</option><option value="5">5</option></select></label><button type="button" class="btn" id="s3-live-start">开始连续上传识别</button><button type="button" class="qbtn" id="s3-live-stop">停止连续采集</button></div><p id="s3-live-message" role="status">S3 与电脑同网；图像持续上传，在电脑运行 YuNet + SFace。跟随需另行启用。</p><p id="s3-live-stats">等待连续画面</p><div style="display:flex;gap:16px;flex-wrap:wrap"><div style="flex:1;min-width:240px"><p>最新画面</p><canvas id="s3-live-view" style="width:100%;background:#080e13" hidden></canvas></div><div style="flex:1;min-width:240px"><p>最近识别画面（框与图对应）</p><canvas id="s3-live-detection" style="width:100%;background:#080e13" hidden></canvas></div></div><p id="s3-live-result">尚无识别结果</p><p class="hint">默认目标2帧/秒，上限5；实际取决于RGB565编码、网络与电脑。处理慢则跳过旧帧。连续帧不保存历史；“机器人拍一张”仍可保存。</p>';
    host.insertBefore(panel,$('camera-engine').nextSibling);
    $('s3-live-start').onclick=()=>control(true);$('s3-live-stop').onclick=()=>control(false);tick();
  }
  if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',init);else init();
})();
