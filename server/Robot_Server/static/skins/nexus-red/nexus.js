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
