/**
 * @file page-idle-pet.js
 * 用途与使用：待机动作库与空闲时间设置；用户确认启用后同步板端，并显示应用回执。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
/* Administrator recipes and explicit idle activation; no browser timers move hardware. */
(function () {
  'use strict';
  var D=window.DASH_API, profile=null, loaded=false;
  function device() { var e=D.$('#pet-device');return (e && e.value.trim()) || 'esp-bridge'; }
  function note(text) { D.$('#pet-result').textContent=text; }
  function recipe() {
    return {id:D.$('#pet-action-id').value.trim(),name:D.$('#pet-action-name').value.trim(),
      meaning:D.$('#pet-action-meaning').value.trim(),custom:true,
      confirmed:D.$('#pet-action-confirm').checked,timeline:JSON.parse(D.$('#pet-recipe').value)};
  }
  function fillAction(id) {
    var action=(profile.actions || []).find(function(a){return a.id===id;});
    if(!action)return;
    D.$('#pet-action-id').value=action.id;D.$('#pet-action-name').value=action.name;
    D.$('#pet-action-meaning').value=action.meaning || '';
    D.$('#pet-action-confirm').checked=action.custom ? !!action.confirmed : !!action.ready;
    D.$('#pet-recipe').value=JSON.stringify(action.timeline || {steps:(action.moves || []).map(function(m){return {at_ms:0,move:m};})},null,2);
    D.$('#pet-preview').textContent='';
  }
  function options(selector, selected, empty) {
    var e=D.$(selector);e.textContent='';
    if(empty){var none=document.createElement('option');none.value='';none.textContent='不使用';e.appendChild(none);}
    (profile.actions || []).forEach(function(a){
      var o=document.createElement('option');o.value=a.id;o.textContent=a.name+(a.ready ? '' : '（待确认）');e.appendChild(o);
    });e.value=selected || '';
  }
  async function load() {
    var j=await D.getJSON('/api/admin/device-profile');if(!j.profile)throw Error(j.error || '加载动作库失败');
    profile=j.profile;var old=D.$('#pet-action-select').value;
    options('#pet-action-select',old || 'idle_look',false);fillAction(D.$('#pet-action-select').value);
    var r=await D.getJSON('/api/admin/idle-pet?device_id='+encodeURIComponent(device()));
    if(!r.ok)throw Error(r.error || '加载待机设置失败');
    var s=r.settings;D.$('#pet-enabled').checked=s.enabled;
    ['idle','sleep','gap'].forEach(function(k){D.$('#pet-'+k+'-seconds').value=s[k+'_seconds'];});
    options('#pet-awake-1',s.awake_actions[0],true);options('#pet-awake-2',s.awake_actions[1],true);
    options('#pet-sleep-action',s.sleep_action,true);syncNote(r);
  }
  function syncNote(r) {
    var s=r.sync || {};
    D.$('#pet-sync').textContent=s.error ? '配方当前无效，已下发关闭：'+s.error :
      (s.applied ? '板端已确认：' : '等待ESP/F407同步：')+(s.enabled ? '待机动作开启' : '待机动作关闭');
    var pose=r.pose || {};D.$('#pet-state').textContent=pose.fresh ?
      (pose.idle===4 ? '板端正在休息，声音可唤醒' : pose.idle ? '板端正在执行待机动作' : '板端空闲/交互中') : pose.message || '尚未收到板端状态';
  }
  async function saveAction() {
    try {
      var a=recipe();if(a.id==='home')throw Error('home为系统安全归中，请使用新的动作ID');
      var preview=await D.postJSON('/api/admin/action-preview',{device_id:device(),timeline:a.timeline});
      if(!preview.ok)throw Error(preview.error);
      var p=JSON.parse(JSON.stringify(profile)),index=p.actions.findIndex(function(x){return x.id===a.id;});
      if(index<0)p.actions.push(a);else p.actions[index]=a;
      var result=await D.postJSON('/api/admin/actions',{action:a});
      if(!result.ok)throw Error(result.error || '保存失败');
      note(a.confirmed ? '动作已保存并确认；AI和互动动作库可以使用。待机执行还需要下方选择并启用。' : '动作草稿已保存；未确认，不会被AI或待机执行。');
      await load();D.$('#pet-action-select').value=a.id;fillAction(a.id);
    }catch(e){note('保存失败：'+e.message);}
  }
  async function saveIdle() {
    var awake=[D.$('#pet-awake-1').value,D.$('#pet-awake-2').value].filter(Boolean);
    var settings={enabled:D.$('#pet-enabled').checked,idle_seconds:Number(D.$('#pet-idle-seconds').value),
      sleep_seconds:Number(D.$('#pet-sleep-seconds').value),gap_seconds:Number(D.$('#pet-gap-seconds').value),
      awake_actions:awake,sleep_action:D.$('#pet-sleep-action').value};
    try {
      var r=await D.postJSON('/api/admin/idle-pet',{device_id:device(),settings:settings});
      if(!r.ok)throw Error(r.error || '保存失败');syncNote(r);
      note('待机参数已保存。待板端显示已确认后生效；录音、上传、OTA及交互优先。');
    }catch(e){note('保存失败：'+e.message);}
  }
  async function init() {
    if(loaded)return;loaded=true;
    D.$('#pet-action-select').addEventListener('change',function(){fillAction(this.value);});
    D.$('#pet-new').addEventListener('click',function(){
      D.$('#pet-action-id').value='my_action';D.$('#pet-action-name').value='我的动作';D.$('#pet-action-meaning').value='';
      D.$('#pet-action-confirm').checked=false;
      D.$('#pet-recipe').value=JSON.stringify({duration_ms:4000,steps:[{at_ms:0,move:{type:'expression',face:'think'}},
        {at_ms:2000,move:{type:'expression',face:'smile'}}]},null,2);
    });
    D.$('#pet-reload').addEventListener('click',function(){load().catch(function(e){note(e.message);});});
    D.$('#pet-action-save').addEventListener('click',saveAction);
    D.$('#pet-save').addEventListener('click',saveIdle);
    D.$('#pet-action-preview').addEventListener('click',async function(){
      try {var r=await D.postJSON('/api/admin/action-preview',{device_id:device(),timeline:recipe().timeline});
        if(!r.ok)throw Error(r.error);D.$('#pet-preview').textContent=JSON.stringify(r.plan,null,2);note('预览完成，未下发；请检查时间、角度和速度。');
      }catch(e){note('预览失败：'+e.message);}
    });
    D.$('#pet-action-test').addEventListener('click',async function(){
      try {var r=await D.postJSON('/api/actions',{device_id:device(),action_id:D.$('#pet-action-select').value});
        if(!r.ok)throw Error(r.error);note('已下发已保存的动作，队列长度 '+r.queue_len+'；编辑框未保存的内容不会执行。');
      }catch(e){note('执行失败：'+e.message);}
    });
    await load();
  }
  async function refresh() {
    if(!loaded)return;
    var r=await D.getJSON('/api/admin/idle-pet?device_id='+encodeURIComponent(device()));
    if(r.ok)syncNote(r); // Never replace unsaved recipes or timing inputs.
  }
  window.IDLE_PET_UI={init:init,refresh:refresh};
})();
