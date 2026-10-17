/**
 * @file nexus.js
 * 用途与使用：可选皮肤的界面增强与 3D 姿态映射；角度契约来自共享参数，姿态数据由板端回传。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
/* Standalone appearance adapter. Never writes original ui_theme_v1 or calls APIs. */
(function(){
  'use strict';
  var root=document.documentElement,key='companion.nexus.mode.v1';
  root.dataset.skin='nexus-red';
  var mode='dark';try{mode=localStorage.getItem(key)==='light'?'light':'dark';}catch(e){}
  function set(value){
    mode=value==='light'?'light':'dark';root.dataset.theme=mode;
    document.querySelectorAll('[data-theme-toggle]').forEach(function(b){b.textContent=mode==='dark'?'☀ 浅色':'☾ 深色';b.title='切换此皮肤的明暗';});
    try{window.dispatchEvent(new CustomEvent('themechange',{detail:{theme:mode}}));}catch(e){}
  }
  set(mode);
  window.Theme={get:function(){return mode;},set:set,toggle:function(){set(mode==='dark'?'light':'dark');try{localStorage.setItem(key,mode);}catch(e){}},syncButtons:function(){set(mode);}};
  window.addEventListener('DOMContentLoaded',function(){
    set(mode);
    var bg=document.createElement('div');bg.className='nexus-background';bg.setAttribute('aria-hidden','true');document.body.prepend(bg);
  });
})();
