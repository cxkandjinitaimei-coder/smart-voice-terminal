/* Presentation only. Consume existing chat/audio observations; never issue a command. */
(() => {
  'use strict';
  let chatState='idle', chatUntil=0, audioUntil=0, audioPhase='', expiry, audioInitialized=false, lastAudioResult='';
  const names={normal:'陪你待一会儿',listen:'正在听你说',think:'正在思考',happy:'收到新的回复'};
  function render(){
    clearTimeout(expiry);
    const now=Date.now();
    const pending=chatState==='pending';
    const active=audioUntil>now;
    const key=pending?'think':active?(audioPhase==='recording'?'listen':'think'):chatState==='reply'&&chatUntil>now?'happy':'normal';
    document.querySelectorAll('[data-companion-avatar]').forEach(img=>{
      if(img.dataset.expression!==key) img.src='/static/avatars/'+key+'.png';
      img.alt='小D · '+names[key];
    });
    const label=document.getElementById('avatar-state');if(label)label.textContent=names[key];
    const next=Math.min(...[chatUntil,audioUntil].filter(t=>t>now));
    if(Number.isFinite(next))expiry=setTimeout(render,next-now+20);
  }
  window.ROBOT_AVATAR={
    chat(state){chatState=state;chatUntil=state==='reply'?Date.now()+4500:0;render();},
    audio(status){
      if(!status)return;
      // Server supplies age rather than interpreting local wall time in the browser.
      const reports=Object.values(status.transport||{})
        .filter(r=>typeof r.age_seconds==='number'&&r.age_seconds>=0&&r.age_seconds<20)
        .sort((a,b)=>a.age_seconds-b.age_seconds);
      const latest=reports[0];
      audioPhase=latest?.phase||'';
      audioUntil=latest&&['recording','uploading'].includes(audioPhase)?Date.now()+20000-latest.age_seconds*1000:0;
      if(audioPhase!=='recording'&&(status.sending||status.processing_queue>0||['processing','sending'].includes(status.last?.inject)||(status.live||[]).some(s=>s.age<20))){
        audioPhase='processing';audioUntil=Date.now()+20000;
      }
      const result=status.last;
      const resultKey=result&&(result.id||result.sess)+':'+result.inject;
      if(audioInitialized&&resultKey!==lastAudioResult&&['auto','manual'].includes(result?.inject)&&chatState!=='pending'){
        chatState='reply';chatUntil=Date.now()+4500;
      }
      audioInitialized=true;lastAudioResult=resultKey;
      render();
    }
  };
  // New chat bubbles reuse the hardware head, with no generated conversation.
  function decorate(){document.querySelectorAll('.bubble-row.ai').forEach(row=>{
    if(row.querySelector('.bubble-avatar'))return;
    const img=document.createElement('img');img.className='bubble-avatar';img.src='/static/avatars/normal.png';img.alt='小D';row.prepend(img);
  });}
  new MutationObserver(decorate).observe(document.getElementById('chat-box'),{childList:true,subtree:true});
  decorate();render();
})();
