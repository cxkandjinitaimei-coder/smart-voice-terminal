/**
 * @file theme.js
 * 用途与使用：主题切换及本地外观偏好；页面启动时应用，持久化只保存外观，不修改设备参数。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
/* ============================================================
   主题引导（theme bootstrap）
   ------------------------------------------------------------
    必须在 <head> 里用同步 <script> 加载（不能加 defer/async）。
   浏览器解析到同步脚本会停下渲染等它执行完，`<html data-theme>` 就在
   首屏绘制之前定好了 —— 这就是"不闪白"的全部原理。
   如果挪到 body 末尾或加 defer，就会先按亮色画一帧再跳成暗色。

   优先级：localStorage 里用户选过的 > 暖色默认主题。
   用户未选过时使用暖色首页；保留原有手动暗色切换能力。
   ============================================================ */
(function () {
  'use strict';

  var KEY = 'ui_theme_v1';   // 键名风格对齐项目里已有的 remote_quick_servo_led_v2

  function readStored() {
    try {
      var v = localStorage.getItem(KEY);
      return (v === 'dark' || v === 'light') ? v : null;
    } catch (e) { return null; }   // 隐私模式 / 禁用存储：读不到就当没存过
  }

  //  首屏前先落定，别等任何人
  document.documentElement.setAttribute('data-theme', readStored() || 'light');

  window.Theme = {
    KEY: KEY,

    get: function () {
      return document.documentElement.getAttribute('data-theme') === 'dark' ? 'dark' : 'light';
    },

    /* persist=false 用于"跟随系统"的临时切换；默认 true 记住用户的选择 */
    set: function (t, persist) {
      if (t !== 'dark' && t !== 'light') return;
      document.documentElement.setAttribute('data-theme', t);
      if (persist !== false) {
        try { localStorage.setItem(KEY, t); } catch (e) { /* 存不了也不影响本次生效 */ }
      }
      // 图表要重取色，各页监听这个事件即可
      try {
        window.dispatchEvent(new CustomEvent('themechange', { detail: { theme: t } }));
      } catch (e) {
        var ev = document.createEvent('CustomEvent');
        ev.initCustomEvent('themechange', false, false, { theme: t });
        window.dispatchEvent(ev);
      }
    },

    toggle: function () {
      window.Theme.set(window.Theme.get() === 'dark' ? 'light' : 'dark');
    },

    /* 按钮上的 ☀/☾ 与 title 文案 */
    syncButtons: function () {
      var dark = window.Theme.get() === 'dark';
      var btns = document.querySelectorAll('[data-theme-toggle]');
      for (var i = 0; i < btns.length; i++) {
        btns[i].textContent = dark ? '☀ 亮色' : '☾ 暗色';
        btns[i].setAttribute('title', dark ? '切换到亮色主题' : '切换到暗色主题');
        btns[i].setAttribute('aria-label', btns[i].getAttribute('title'));
      }
    }
  };

  //  按钮文案（此刻按钮可能还没解析出来，DOMContentLoaded 再补一次）
  window.addEventListener('DOMContentLoaded', window.Theme.syncButtons);
  window.addEventListener('themechange', window.Theme.syncButtons);
})();
