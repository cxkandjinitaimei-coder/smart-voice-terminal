/* Display existing observations. This module never issues hardware commands. */
(() => {
  'use strict';
  let expiry;
  window.COMPANION_AUDIO_VIEW={render(status){
    clearTimeout(expiry);
    const reports=Object.values(status.transport||{}).filter(r=>typeof r.age_seconds==='number'&&r.age_seconds<20).sort((a,b)=>a.age_seconds-b.age_seconds);
    const report=reports[0],receiving=(status.live||[]).some(s=>s.age<20),recording=report?.phase==='recording';
    const uploading=report?.phase==='uploading'||receiving;
    const processing=status.processing_queue>0||['processing','sending'].includes(status.last?.inject)||!!status.sending;
    const text=recording?'正在录音':uploading?'正在上传':processing?'正在识别 / 回复':report?.phase==='failed'?'录音中断':status.last?'等待下一句':'等待设备录音';
    for(const id of ['home-audio-state','audio-live-badge']){const n=document.getElementById(id);if(n)n.textContent=text;}
    document.querySelectorAll('.sound-wave').forEach(n=>{n.classList.toggle('is-recording',recording);n.title='录音状态示意';});
    if(report&&['recording','uploading'].includes(report.phase))expiry=setTimeout(()=>this.render({...status,transport:{},live:[]}),Math.max(200,20000-report.age_seconds*1000));
  }};
})();
