# 源码导读与注释规范

入口顺序：先读根 README 与架构，再读 F407 main、ESP_Gateway.ino、服务 app。文件头解释用途和调用方式，内部注释解释状态/数据所有权及边界；不逐句翻译赋值。

## F407 代码

| 模块 | 职责与调用 |
| --- | --- |
| `firmware/STM32_Controller/App/ailink.c` | ESP 下行命令入口，协调舵机、表情、时间线、待机动作及升级回执。 |
| `firmware/STM32_Controller/App/camera_link.c` | 把一次拍照请求连接到 OV7725 FIFO 采集及 ESP 图片传输。 |
| `firmware/STM32_Controller/App/face_anim.c` | 从 SD 卡读取 RGB565 序列帧，在表情区域播放动画。 |
| `firmware/STM32_Controller/App/font.c` | UTF-8 解码、ASCII 与汉字点阵检索以及文本绘制。 |
| `firmware/STM32_Controller/App/font_store.c` | 在 W25Q SPI Flash 中定位、校验和查询扩展字库。 |
| `firmware/STM32_Controller/App/font_upload.c` | 接收上位机字库分块并写入 W25Q，提供进度和失败恢复接口。 |
| `firmware/STM32_Controller/App/media.c` | SD 相册、表情、文本与 GUI 的统一显示入口。 |
| `firmware/STM32_Controller/App/miclink.c` | 麦克风触发、自动断句和上传忙状态的应用层协调器。 |
| `firmware/STM32_Controller/App/robot_gui.c` | 表情页和参数页的布局、顶部状态栏、录音反馈及方向适配。 |
| `firmware/STM32_Controller/App/w25q_img.c` | 外置 SPI Flash 图片槽的索引、校验、上传和读取。 |
| `firmware/STM32_Controller/Bootloader/boot_main.c` | 独立 Bootloader：校验元数据、安装 B 到 A 并跳转应用。 |
| `firmware/STM32_Controller/Hardware/02KEY/key.c` | PA0/K-UP 与 PE3/KEY1 的采样、防抖和单次按下事件。 |
| `firmware/STM32_Controller/Hardware/06USART/uart.c` | USART2 调试口初始化、printf 重定向和前台命令输入。 |
| `firmware/STM32_Controller/Hardware/06USART/uart_rx.c` | 调试串口接收队列和逐行提取接口。 |
| `firmware/STM32_Controller/Hardware/11DHT11/dht11.c` | PE0 单总线温湿度读取及应答/校验错误分类。 |
| `firmware/STM32_Controller/Hardware/13LEDPWM/led_pwm.c` | TIM3 通道的 LED PWM 亮度控制。 |
| `firmware/STM32_Controller/Hardware/14ESP_UART/esp_uart.c` | USART6 字节环形队列及文本/二进制混合流解析。 |
| `firmware/STM32_Controller/Hardware/15LCD/lcd.c` | FSMC 并口 LCD 初始化、方向设置、窗口和像素写入。 |
| `firmware/STM32_Controller/Hardware/16SD/sd.c` | 原生 SDIO 一位总线的 SD 卡识别与扇区读写。 |
| `firmware/STM32_Controller/Hardware/17PCA9685/pca9685.c` | 通过 I2C 配置 PCA9685 的 PWM 频率和舵机脉宽。 |
| `firmware/STM32_Controller/Hardware/17PCA9685/pca9685_cmd.c` | 解析调试口 /pwm 命令并调用 PCA9685 驱动。 |
| `firmware/STM32_Controller/Hardware/18INMP441/inmp441.c` | INMP441 的 I2S2/DMA 接收、声道选择、采样统计和读半块接口。 |
| `firmware/STM32_Controller/Hardware/19MICREC/micrec.c` | 短录音的 RAM 缓存、调试命令和十六进制导出。 |
| `firmware/STM32_Controller/Hardware/20MICAUD/micaud.c` | 把麦克风 DMA 半块拼成 2048 字节块，经串口带 ACK 发送给 ESP。 |
| `firmware/STM32_Controller/Hardware/21IAP/iap_app.c` | 应用侧 OTA 请求确认、下载槽写入、CRC 校验和提交状态机。 |
| `firmware/STM32_Controller/Hardware/21IAP/iap_flash.c` | 统一片内 Flash 分区、擦写边界、CRC32 与镜像校验底层。 |
| `firmware/STM32_Controller/Hardware/21IAP/iap_meta.c` | S1 元数据日志的校验、序号选择和状态持久化。 |
| `firmware/STM32_Controller/Hardware/22W25Q/w25q.c` | W25Q SPI NOR 的识别、容量、页写和扇区擦除。 |
| `firmware/STM32_Controller/Hardware/22W25Q/w25q_selftest.c` | W25Q 的在线诊断和受控读写自测入口。 |
| `firmware/STM32_Controller/Hardware/23OV7725/camera_ov7725.c` | OV7725 SCCB 寄存器配置、帧同步、外置 FIFO 读取及色条诊断。 |
| `firmware/STM32_Controller/System/FATFS/diskio.c` | FatFs 扇区接口到本工程 SDIO 驱动的适配层。 |
| `firmware/STM32_Controller/System/time.c` | TIM6 毫秒全局时基及初始化/延时辅助接口。 |
| `firmware/STM32_Controller/User/main.c` | F407 应用启动、自检和无 RTOS 协作调度主循环。 |
| `firmware/STM32_Controller/User/stm32f4xx_it.c` | Cortex-M 异常处理和启动故障诊断。 |
| `firmware/STM32_Controller/User/system_stm32f4xx.c` | 复位后的系统时钟、VTOR 和最小启动诊断。 |

内联实现如 `servo_motion.h`、`motion_timeline.h`、`idle_pet.h`由 ailink 持有状态并调用。Core/SPL/FatFs/标准设备头保留上游说明；两个 ST 启动模板只补本项目使用说明，未改上游授权。

## 上位机代码

| 文件 | 职责 |
| --- | --- |
| `server/Robot_Server/ai_relay.py` | 调用兼容 chat/completions 的 AI 服务，并把回复解析成文本、动作与时间线。 |
| `server/Robot_Server/analytics.py` | 解析温湿度等传感器记录，计算统计并评估阈值规则。 |
| `server/Robot_Server/app.py` | Flask 服务入口：鉴权、遥测、命令队列及语音/相机/OTA 路由的组合。 |
| `server/Robot_Server/asr.py` | 常驻 CPU faster-whisper 语音识别与可观测加载状态。 |
| `server/Robot_Server/asr_check.py` | 语音依赖、WAV 解码、模型准备和运行服务状态检查。 |
| `server/Robot_Server/audio_archive.py` | SQLite 录音归档及待发送文本 FIFO。 |
| `server/Robot_Server/audio_retention.py` | 录音保留数量的配置校验和持久化。 |
| `server/Robot_Server/audio_rx.py` | 接收 PCM 分块、落盘 WAV、检查完整性、排队识别并按手动/自动模式送 AI。 |
| `server/Robot_Server/board_pose.py` | 缓存 F407 经 ESP 回传的舵机姿态快照。 |
| `server/Robot_Server/camera_workspace.py` | 拍照任务队列、图像上传、人脸分析与历史图像访问。 |
| `server/Robot_Server/device_controls.py` | 麦克风 VAD 与舵机全局速度的参数保存、修订计算和应用确认。 |
| `server/Robot_Server/device_profile.py` | 三轴舵机限位、回正、速度、表情和管理员动作库的规范档案。 |
| `server/Robot_Server/download_asr_model.py` | 显式下载并验证 faster-whisper small 模型，可断点续传。 |
| `server/Robot_Server/f407_ota_relay.py` | F407 OTA 镜像边界/向量校验及 ESP 直连传输适配。 |
| `server/Robot_Server/face_local/__init__.py` | 本地人脸处理 Python 包入口。 |
| `server/Robot_Server/face_local/face_engine.py` | OpenCV YuNet 检测和 SFace 特征比对的本地 CPU 实现。 |
| `server/Robot_Server/iap_pull.py` | 设备主动拉取 F407 OTA 的任务状态机。 |
| `server/Robot_Server/idle_pet.py` | 管理员可编辑的待机动作、空闲间隔和同步载荷。 |
| `server/Robot_Server/lan_discovery.py` | UDP 局域网自动发现协议与可访问地址选择。 |
| `server/Robot_Server/local_faces.py` | 相机工作区到本地人脸引擎的适配层。 |
| `server/Robot_Server/motion_timeline.py` | 把管理员/AI 动作计划编译为板端有时间戳的二进制步骤。 |
| `server/Robot_Server/ota_console.py` | 固件库、ESP 固件版本发布、F407 升级任务与回执的网页接口。 |
| `server/Robot_Server/release_version.py` | 网页及识别模块的公开版本常量。 |
| `server/Robot_Server/runtime_config.py` | 私有服务配置与会话签名密钥。 |
| `server/Robot_Server/servo_contract.py` | 读取唯一舵机物理默认契约。 |
| `server/Robot_Server/start_server.py` | 可迁移的局域网服务启动入口。 |
| `server/Robot_Server/store.py` | SQLite 遥测、事件、命令与交互记录，以及后台日志导入/聚合。 |
| `server/Robot_Server/ui_skins.py` | 网页皮肤注册表与静态资源路径检查。 |

## 维护规则

- 文件头写清目的、使用入口/顺序、依赖、数据格式和限制。状态机在每个重要阶段就近说明。
- 说明缓冲由谁写、谁读、何时释放；区分接收、落盘、云端完整确认与AI处理完成。
- 板端 busy 是协作状态；服务端 Lock/RLock 才是线程互斥。不要混用描述。
- 修改协议两端和回归一起核对；调用方不能在返回前把共享解析缓冲留给异步代码。
- F407 中文源文件保留 UTF-8 BOM，让ARMCC5正确识别；原始ST设备头保留GB18030。不要一键把所有文件改成同一字节编码。
- ARMCC内联汇编 `boot_enter` 的指令换行具有语义，已用clang-format off/on保护，不合并成一行。
- 不在源文件里保留协作占区标记、聊天式修改记录、私有绝对路径；历史改动由版本控制记录。
- 不批量改写第三方授权、压缩库和生成数值数组。
