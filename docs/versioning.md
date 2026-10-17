# 版本与命名

公开源码版本为 **1.0.0**，根 `VERSION` 是版本入口。

```powershell
python tools/set_version.py
python tools/set_version.py --check
```

同步目标：F407 `User/app_version.h::APP_FW_VERSION`、ESP `firmware_version.h::ESP_FW_VERSION/ESP_BUILD_ID`、网页 `release_version.py::APP_VERSION` 及动作编辑器缓存版本。网页模板从上下文取版本，不手工维护多处日期字符串。舵机契约修订、协议号和布局ID各自有兼容含义，不机械改成产品版本。

## 可改名范围

- 项目最外层目录可改，脚本以 `__file__` 定位，不查找 AGENTS 标记或固定盘符。
- Keil 工程按硬件职责命名；驱动如 `pca9685`、`micaud` 保留稳定接口名。
- Arduino 草图目录与 `.ino` 必须同名；修改这两个名称时一起改。
- F407 显示名在 `User/product_config.h::ROBOT_DISPLAY_NAME`，建议两个汉字或四个ASCII字符；网页/AI显示名在私有 `config.json` 的 `display_name`。这些只影响展示/默认角色，不进入设备ID、协议标识或路径。
- 自定义 `persona` 优先；不会按公开版本覆盖管理员写入的人设。

## 旧开发 ESP 的迁移

原开发 ESP 数字版本为1.1.13，高于公开1.0.0。现有自动更新只接受更高版本，因此：

1. 不能把1.0.0当正常自动降级包，设备会拒绝。
2. 若有线写入1.0.0而服务器还发布1.1.13，设备可能自动更新回旧开发版。
3. 真正迁移时应一起规划服务器发布记录、配置和首次写入方式；本次只整理源码，没有绕过比较规则，也没有修改线上固件。

F407仍使用已经扩容的224/256KiB布局，不能与早期96KiB布局混用。版本文字相同也不能替代布局校验。
