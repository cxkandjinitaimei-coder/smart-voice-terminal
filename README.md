# 嵌入式交互机器人 · 1.0.0

STM32F407 承担传感器采集、三轴舵机、屏幕表情和 OTA 执行；ESP8266 作为网络网关，连接 Flask 上位机。上位机接收音频和图片，在本机运行语音/人脸识别，并调用配置的 AI API 产生回复及动作。

这是从持续迭代工程整理出的独立源码版本。产品名和 AI 名称尚未定稿，工程名按职责命名；外层目录可改名。软件采用 F407 裸机协作调度，**没有 RTOS**。

## 目录与入口

| 目录 | 职责 | 入口 |
| --- | --- | --- |
| `firmware/STM32_Controller` | F407 应用、硬件驱动和 Bootloader | `User/STM32_Controller_OTA.uvprojx` |
| `firmware/ESP_Gateway` | ESP8266 串口/网络网关 | `ESP_Gateway.ino` |
| `server/Robot_Server` | 网页、遥测、AI、语音、人脸与 OTA 服务 | `start_server.py` |
| `tools` | 编译、版本同步、模型下载、SD 素材转换、回归 | 各脚本文件头和 `--help` |
| `tests/server` | 使用真实服务模块的离线回归 | `tools/run_checks.py` |
| `docs` | 架构、硬件、协议、构建、迁移和限制 | 下列文档 |

## 功能

- I2S/DMA 麦克风采集；可调 VAD 起录、静音断句；手动定长录音；ESP 缓冲及网络上传。
- WAV 归档、限量保留、历史试听、识别队列；识别文字可手动确认或自动交 AI。
- 三轴绝对角控制、独立速度、机械限位、动作库、带时间戳计划和待机动作。
- SD 卡表情序列帧、局部切片刷新、录音状态条、横竖屏和参数页。
- DHT11 温湿度上报与曲线；板端姿态回传驱动网页 3D 显示。
- OV7725 FIFO 单帧拍照、上传历史、YuNet 检测、SFace 指定人员匹配及角度偏差计算。
- F407 A/B 下载升级与实体按键确认；ESP 固件版本发布。最终成功需板端重启校验回传。

## 从零搭建环境

首次使用先看 [环境搭建与依赖说明](docs/环境搭建与依赖说明.md)，按顺序准备软件、依赖、配置和硬件。分项说明均使用中文命名：

- [网页服务环境搭建](docs/网页服务环境搭建.md)：Windows/Linux、Python依赖、AI配置与局域网访问。
- [ESP8266环境搭建](docs/ESP8266环境搭建.md)：Arduino IDE/CLI、开发板包、库、网关配置与上传。
- [STM32环境搭建](docs/STM32环境搭建.md)：Keil/ARMCC、芯片包、应用/Bootloader与首次安装。
- [识别模型与工具环境搭建](docs/识别模型与工具环境搭建.md)：语音/人脸模型、SD素材和可选工具依赖。

## 先运行上位机

建议 Python 3.10+，在项目根打开终端：

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -r server/Robot_Server/requirements.txt
.venv\Scripts\python server/Robot_Server/start_server.py --init-config
```

编辑新生成的 `server/Robot_Server/config.json`：口令自动随机生成，与 ESP 的 `API_TOKEN` 保持一致；填写需要使用的 AI API 地址、模型和 Key，并启用对应 Provider。未配置 AI 时，硬件测试和网页仍可使用，AI 调用会明确失败。

```powershell
.venv\Scripts\python server/Robot_Server/download_asr_model.py
.venv\Scripts\python tools/download_face_models.py
.venv\Scripts\python server/Robot_Server/start_server.py
```

本机访问 <http://127.0.0.1:5000/>；同 Wi-Fi 设备使用本机局域网 IP。Linux 使用 `.venv/bin/python`。识别模型是可选功能依赖，不随源码附带；启动服务不会自动下载模型。

## 构建固件

- F407：Keil MDK、ARMCC 5.06 update 7、STM32F4xx DFP。正常 OTA 使用 `STM32_Controller_OTA`，另有独立 `STM32_Bootloader`。普通 `STM32_Controller` 链接在 Flash 起始地址，只供无 Bootloader 的单独调试。
- ESP8266：Arduino ESP8266 Core 3.1.2、ArduinoJson 7.4.3；NodeMCU、4MB Flash/2MB LittleFS。复制 `gateway_config.example.h` 为 `gateway_config.h` 并填写本地配置。
- 固件版本源为根 `VERSION`，编译前运行 `python tools/set_version.py`。

具体命令、首次写入与 OTA 的区别见 [构建与使用](docs/build-and-run.md) 和 [Flash 与 OTA](docs/flash-and-ota.md)。**公开版 1.0.0 低于旧开发 ESP 1.1.13，不能直接当作自动降级固件使用。**

## 阅读代码

自有代码采用中文详细文件头，说明用途、调用顺序、数据格式和边界；关键状态机在分支旁解释帧头、长度、负载、校验、ACK、超时及释放。第三方库、许可和生成字模保持原始内容。模块划分见 [架构](docs/architecture.md) 和 [源码导读](docs/source-guide.md)。

| 文档 | 内容 |
| --- | --- |
| [硬件接线](docs/hardware.md) | 当前 F407/ESP8266/OV7725 实际信号分配 |
| [协议与同步](docs/protocols.md) | 串口信封、音频、OTA、命令与回执 |
| [SD 素材](docs/sd-assets.md) | BMP/R16 生成、目录及缺素材行为 |
| [版本与迁移](docs/versioning.md) | 1.0.0 版本源、名称配置、旧版本迁移 |
| [验证记录](docs/validation.md) | 本次真实构建/测试范围与结果 |
| [已知限制](docs/known-limitations.md) | 阻塞、网络、飞线、识别和回滚限制 |
| [第三方声明](THIRD_PARTY_NOTICES.md) | MCU 库、网页库、模型与字体许可范围 |

仓库保留源码、测试、工具、使用文档和许可文件。Wi-Fi/API私有配置、数据库、录音、人员照片/特征、模型权重、编译输出及IDE本机配置不纳入版本控制；规则见`.gitignore`。首次使用按上方环境搭建说明准备本地依赖和配置。
