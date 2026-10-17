# 构建与使用

本文保留常用命令速查。首次从零安装软件、开发板包和依赖，请按中文文档[环境搭建与依赖说明](环境搭建与依赖说明.md)开始，分项说明包含配置、安装、验证和常见错误。

所有脚本通过自身路径定位项目。可更改外层目录名，也可放入带空格的路径；不要只移动单个子目录后仍期待跨模块工具找到根目录。

## STM32F407

工具链：Keil MDK/uVision、ARMCC 5.06 update 7（build 960）、Keil STM32F4xx DFP 3.1.1。工程基于 SPL/CMSIS，源码依赖已包含，不需要原工程的目录联接。未适配 ARM Compiler 6。

```powershell
python tools/set_version.py
python tools/build_stm32.py --keil "<Keil安装目录>/UV4/UV4.exe" --fromelf "<Keil安装目录>/ARM/ARMCC/bin/fromelf.exe"
```

也可分别在 Keil 打开并 Rebuild：

| 工程 | 目标名 | 输出位置（相对 STM32_Controller） | 使用 |
| --- | --- | --- | --- |
| `User/STM32_Controller.uvprojx` | `STM32_Controller` | `User/Objects/STM32_Controller.*` | 无 Bootloader 的普通调试 |
| `User/STM32_Controller_OTA.uvprojx` | `STM32_Controller_OTA` | `User/ObjectsOTA/STM32_Controller_OTA.*` | 链接 Slot A，BIN 可交 OTA |
| `Bootloader/STM32_Bootloader.uvprojx` | `STM32_Bootloader` | `Bootloader/Objects/STM32_Bootloader.*` | 首次有线安装的引导程序 |

输出 HEX 由 Keil 生成，辅助脚本用 fromelf 导出 BIN；`.build` 保存日志。普通应用的基址为 `0x08000000`，不能上传到 Slot A，直接写入会覆盖已有 Bootloader。首次引导安装按 Flash 文档执行，不由构建脚本自动烧录。

## ESP8266

1. 安装 ESP8266 Arduino Core 3.1.2、ArduinoJson 7.4.3。
2. 将 `firmware/ESP_Gateway/gateway_config.example.h` 复制为 `gateway_config.h`，填写 Wi-Fi、API_TOKEN 与服务器地址。设备 ID 保持稳定，不随产品改名。
3. Arduino 打开 `ESP_Gateway/ESP_Gateway.ino`；开发板 NodeMCU 1.0，Flash 4MB，LittleFS 2MB。

```powershell
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:eesz=4M2M firmware/ESP_Gateway
```

草图目录和 `.ino` 必须同名。主文件按固定顺序包含实现头，共享对象有初始化顺序要求；不能任意把这些 `.h` 改为 `.cpp`。编译仍有 ArduinoJson 兼容 API 弃用告警，见验证记录。

## 上位机与模型

启动顺序及配置见根 README。模型默认目录：`server/Robot_Server/models/asr`；录音默认 `recordings`；人脸模型在 `face_local/models`。如需复用自己已有的 ASR 缓存，可设置 `ASR_MODELS_DIR`，不会扫描其他工程。

```powershell
python server/Robot_Server/asr_check.py --app-dir server/Robot_Server --dependencies
python server/Robot_Server/asr_check.py --app-dir server/Robot_Server --prepare
python tools/run_checks.py
```

`--dependencies` 只验证 WAV 解码依赖，不下载模型；`--prepare` 加载本地权重并测试 VAD。下载器支持 `--endpoint`，环境中的 `HF_ENDPOINT` 应在导入模型库前设置。模型验证输出 `MODEL_READY` 后仍需运行服务；下载完成不会自动部署或启动服务。

网页采用原有路由与 UI，不在本次重新设计。局域网发现用 UDP5001，默认 HTTP5000；防火墙应按实际使用放行。默认 Flask 启动器用于局域网联调，公网部署需自行配置进程管理、反向代理和 HTTPS；公开源码没有绑定个人云主机的自动部署脚本。
