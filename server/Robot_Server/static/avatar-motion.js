/* 原F407表情的完整序列交给浏览器原生WebP解码，不使用静图位移模拟眨眼。
 * 动态/静态资源均随网页本地交付，失败回PNG；支持减少动态偏好与手动暂停。
 */
(() => {
  'use strict';
  const reduced=matchMedia('(prefers-reduced-motion: reduce)');
  let enabled=!reduced.matches;
  const pattern=/\/static\/avatars\/(normal|listen|happy|think)\.(png|webp)(?:\?.*)?$/;
  const failed=new Set();
  function sync(img){
    const match=img.getAttribute('src')?.match(pattern);
    if(!match)return;
    const key=match[1],shouldAnimate=enabled&&!failed.has(key);
    const next='/static/avatars/'+key+(shouldAnimate?'.webp':'.png');
    img.dataset.expression=key;img.dataset.animated=String(shouldAnimate);
    if(img.getAttribute('src')!==next)img.src=next;
  }
  function refresh(){document.querySelectorAll('img').forEach(sync);const button=document.getElementById('motion-toggle');if(button){button.textContent=enabled?'暂停表情':'播放表情';button.setAttribute('aria-pressed',String(!enabled));}}
  document.addEventListener('error',event=>{const img=event.target;if(!(img instanceof HTMLImageElement))return;const match=img.getAttribute('src')?.match(pattern);if(match&&match[2]==='webp'){failed.add(match[1]);sync(img);}},true);
  new MutationObserver(records=>{for(const item of records){if(item.type==='attributes')sync(item.target);else for(const node of item.addedNodes){if(node.nodeType!==1)continue;if(node.matches('img'))sync(node);node.querySelectorAll('img').forEach(sync);}}}).observe(document.body,{childList:true,subtree:true,attributes:true,attributeFilter:['src']});
  const setEnabled=value=>{enabled=Boolean(value);refresh();};
  const actions=document.querySelector('.topbar-right');
  if(actions){const button=document.createElement('button');button.type='button';button.id='motion-toggle';button.className='theme-btn';button.addEventListener('click',()=>setEnabled(!enabled));actions.prepend(button);}
  reduced.addEventListener('change',()=>setEnabled(!reduced.matches));
  window.AVATAR_MOTION={setEnabled,get enabled(){return enabled;}};
  refresh();
})();
