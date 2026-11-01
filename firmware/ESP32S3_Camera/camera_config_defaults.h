/* 本文件负责私用固件首次升级时应用已预填的联网参数。
 * 用法：setup在Preferences打开后调用cameraApplyBundledConfig，最后才写配置批次号。
 * 批次不变时不覆盖用户在配网页保存的设置。公开模板的批次为空，不迁移。
 * 任意写入失败不登记完成，本次使用编译默认值，下次启动重试，不输出密码。
 */
#pragma once
#include <cstring>

enum CameraDefaultsResult { CAMERA_DEFAULTS_NONE, CAMERA_DEFAULTS_APPLIED, CAMERA_DEFAULTS_RETRY };

template<class Store>
CameraDefaultsResult cameraApplyBundledConfig(Store &store, const char *revision,
    const char *ssid, const char *password, const char *server, const char *token, const char *device) {
  // 公开空模板或未填完整的私用模板不能清掉原有有效配置。
  if (!revision[0] || !ssid[0] || !server[0] || !token[0] || !device[0]) return CAMERA_DEFAULTS_NONE;
  if (std::strcmp(store.getString("cfg_rev", "").c_str(), revision) == 0) return CAMERA_DEFAULTS_NONE;
  // 每项分别落盘；失败时不提前写完成标记，避免半份配置永久生效。
  bool ok = store.putString("ssid", ssid) == std::strlen(ssid);
  ok = (store.putString("password", password) == std::strlen(password)) && ok;
  ok = (store.putString("server", server) == std::strlen(server)) && ok;
  ok = (store.putString("token", token) == std::strlen(token)) && ok;
  ok = (store.putString("device", device) == std::strlen(device)) && ok;
  if (!ok) return CAMERA_DEFAULTS_RETRY;
  // 最后提交批次。以后重启沿用用户保存值，直到主动安装新的配置批次。
  return store.putString("cfg_rev", revision) == std::strlen(revision)
      ? CAMERA_DEFAULTS_APPLIED : CAMERA_DEFAULTS_RETRY;
}
