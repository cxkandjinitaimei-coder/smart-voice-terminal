# Smart Voice Terminal

智能语音终端是一个持续迭代的嵌入式交互机器人项目。公开版本 `1.0.0` 提供 F407 主控固件、ESP8266 网络网关、Flask 网页服务、ESP32-S3 相机接口、工具和文档。

## 系统组成

- **STM32F407**：裸机协作调度，负责 LCD 表情/文字、I2S 麦克风、DHT11、PCA9685 舵机、SD 素材、串口协议和 A/B OTA。
- **ESP8266 网关**：NodeMCU 通过 Wi-Fi 与网页服务通信，通过 USART6 与 F407 交换传感器、文字、动作和录音帧。
- **Flask 服务**：提供网页控制台、设备队列、动作时间轴、AI 决策、ASR、相机工作台和 OTA 中继。
- **ESP32-S3 相机**：独立联网的 JPEG/人脸定位接口，可按请求或触发条件采集照片。

项目采用裸机主循环、DMA、中断和有限队列完成任务同步，不依赖 RTOS。动作、协议和舵机校准参数以可审查的配置和文档为准。

## 目录

```text
firmware/STM32_Controller/  F407 应用、Bootloader、硬件驱动
firmware/ESP_Gateway/       ESP8266 网关工程
firmware/ESP32S3_Camera/    ESP32-S3 相机工程
server/Robot_Server/         Flask 服务、网页和离线测试
tools/                      SD 素材、音频、识别和检查工具
docs/                       架构、协议、接线、构建和验证文档
tests/                      公开的回归测试入口
```

## 快速开始

1. 阅读 [环境搭建与依赖说明](docs/环境搭建与依赖说明.md)。
2. 复制 `server/Robot_Server/config.example.json` 为 `config.json`，生成随机登录口令并填写自己的 AI Provider。
3. 复制 `firmware/ESP_Gateway/gateway_config.example.h` 为 `gateway_config.h`，填写 Wi-Fi、服务地址和 API token。该文件只保存在本机，不提交。
4. 在服务目录安装 `requirements.txt`，运行 `python app.py`，浏览器访问 `http://127.0.0.1:5000/`。
5. 按 [构建与使用](docs/build-and-run.md) 编译 F407/ESP；真实固件输出、模型和运行数据不在仓库中。

未配置 AI 时，网页和硬件链路仍可使用模拟数据、动作测试和协议回归。语音、人脸和 TTS 需要按照文档单独安装可选依赖及模型。

## 文档入口

- [架构](docs/architecture.md)
- [硬件接线](docs/hardware.md)
- [协议与同步](docs/protocols.md)
- [构建与使用](docs/build-and-run.md)
- [Flash 与 OTA](docs/flash-and-ota.md)
- [公开配置说明](docs/公开配置说明.md)
- [SD 素材](docs/sd-assets.md)
- [验证流程](docs/validation.md)
- [已知限制](docs/known-limitations.md)

## 隐私与版本边界

仓库只包含可复现的源码、工具、测试、文档和许可证。`.gitignore` 会排除 Wi-Fi/API/服务器凭据、数据库、录音、照片和人脸库、模型权重、日志、HEX/BIN/ELF 等本地产物。发布版本为源码快照，不能直接替代你的设备固件；刷写前请按硬件型号和交付包说明验收。

## 许可证

自有代码按 [LICENSE](LICENSE) 发布；第三方库、字体和模型的许可范围见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
