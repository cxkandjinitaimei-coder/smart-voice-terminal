/**
 * @file page-trainer.js
 * 用途与使用：动作编辑器 iframe 的载入及页面生命周期；编辑结果须确认后保存/执行。
 * 调用约束：页面切换时遵循既有轮询/事件生命周期；不可把浏览器缓存当设备当前状态。
 * 维护：外层目录可改名；模块与接口名保持稳定，配置经服务端校验。
 */
(function () {
  'use strict';
  window.PAGES.trainer = {title: '3D 与动作训练', init: function () {
    const frame = document.getElementById('motion-trainer-frame');
    frame.addEventListener('load', function () {
      const skin = window.COMPANION_ACTIVE_SKIN;
      if (!skin || !skin.model_styles) return;
      const doc = frame.contentDocument;
      if (!doc) return;
      const style = doc.createElement('link'); style.rel = 'stylesheet'; style.href = skin.model_styles; doc.head.append(style);
      if (frame.contentWindow.ROBOT_PREVIEW && skin.model_background) {
        const model = frame.contentWindow.ROBOT_PREVIEW;
        model.renderer.setClearColor(skin.model_background);
        if (model.scene.background) model.scene.background.set(skin.model_background);
      }
    });
    if (!frame.src) frame.src = '/static/motion-trainer.html?v=1.0.0';
  }, refresh: function () { return Promise.resolve(); }};
})();
