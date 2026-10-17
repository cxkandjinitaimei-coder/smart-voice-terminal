/**
 * @file skin-loader.js
 * 用途与使用：按服务端皮肤注册表载入 CSS/脚本；切换时清理旧资源，不直接访问 MCU。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
/* Appearance adapter. Add a skin.json + assets; device modules do not change. */
(function () {
  'use strict';
  const skins = window.COMPANION_SKINS || [];
  let chosen = 'original';
  try { chosen = localStorage.getItem('companion.skin.v1') || chosen; } catch (_) {}
  const skin = skins.find(s => s.id === chosen) || skins[0];
  if (!skin) return;
  window.COMPANION_ACTIVE_SKIN = skin;
  document.documentElement.dataset.skin = skin.id;
  if (skin.id !== 'original') {
    let mode = skin.default_mode;
    try { const saved = localStorage.getItem(skin.storage_key); if (['dark','light'].includes(saved)) mode = saved; } catch (_) {}
    document.documentElement.dataset.theme = mode;
    skin.styles.forEach(href => { const link = document.createElement('link'); link.rel = 'stylesheet'; link.href = href; document.head.append(link); });
    const script = document.createElement('script'); script.src = skin.bootstrap; script.async = false; document.head.append(script);
  }
  window.addEventListener('DOMContentLoaded', () => {
    const select = document.getElementById('appearance-skin');
    if (!select) return;
    skins.forEach(s => select.add(new Option(s.name, s.id)));
    select.value = skin.id;
    document.getElementById('appearance-apply').onclick = () => {
      try { localStorage.setItem('companion.skin.v1', select.value); location.reload(); }
      catch (_) { document.getElementById('appearance-result').textContent = '浏览器禁止保存偏好，暂时无法切换皮肤。'; }
    };
  });
})();
