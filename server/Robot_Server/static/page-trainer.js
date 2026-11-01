(function () {
  'use strict';
  const D=window.DASH_API, byId=id=>document.getElementById(id);
  let initialized=false;
  const device=()=>byId('short-device').value.trim()||'esp-bridge';
  async function api(path,body) {
    const response=await fetch('/api/companion/'+path+'?device_id='+encodeURIComponent(device()),
      body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({...body,device_id:device()})});
    const value=await response.json();
    if(!response.ok||value.error)throw Error(value.error||'请求失败');
    return value;
  }
  async function refresh() {
    const state=await api('status');
    byId('short-status').textContent=(state.device_online?'执行板在线 · ':'执行板离线 · ')+state.reason;
    byId('short-idle').checked=state.idle.enabled;
    byId('short-idle-state').textContent=state.idle.enabled?
      (state.idle_applied?'待机短动作已同步到 F407':'待机短动作已启用，等待 F407 配置回执'):'待机短动作已关闭';
    const value=await api('actions');
    const box=byId('short-buttons');box.replaceChildren();
    value.actions.forEach(action=>{
      const button=document.createElement('button');button.type='button';button.className='qbtn';
      button.textContent=action.name;button.disabled=!state.device_online||!action.ready;
      button.title=action.unavailable_reason||'点击发送这个短动作';
      button.onclick=async()=>{button.disabled=true;try{
        const result=await api('actions',{action_id:action.id});
        byId('short-result').textContent=result.message;
      }catch(error){byId('short-result').textContent=error.message;}finally{button.disabled=false;}};
      box.append(button);
    });
  }
  function safely(task){return async()=>{try{await task();}catch(error){byId('short-result').textContent=error.message;}};}
  window.PAGES.trainer={title:'短动作与姿态',init(){
    if(initialized)return;initialized=true;
    byId('short-refresh').onclick=safely(refresh);
    byId('short-idle').onchange=safely(async()=>{await api('idle',{enabled:byId('short-idle').checked});await refresh();});
    byId('short-device').onchange=safely(refresh);
    const frame=byId('motion-trainer-frame');
    frame.src='/static/motion-preview.html?v=W20261029-07';
    refresh().catch(error=>{byId('short-result').textContent=error.message;});
    setInterval(()=>{if(!document.hidden&&D.currentTab()==='trainer')refresh().catch(()=>{});},3000);
  },refresh:()=>initialized?refresh():Promise.resolve()};
})();
