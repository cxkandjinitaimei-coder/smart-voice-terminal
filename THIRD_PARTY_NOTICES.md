# 第三方与资源许可

根MIT只覆盖项目自有代码，不能覆盖第三方库或来源未确认的字模/美术。以下原始说明保留在对应源码或资源目录。

| 组件 | 位置 | 来源/许可 |
| --- | --- | --- |
| STM32F4 SPL、设备模板 | `firmware/STM32_Controller/Lib`、`User/stm32f4xx*`、`User/system_stm32f4xx*` | STMicroelectronics；源文件内 MCD-ST Liberty SW License Agreement V2 等原始条款适用 |
| CMSIS/startup | `firmware/STM32_Controller/Core` | ARM/Keil；逐文件保留版权与许可，不重新标MIT |
| FatFs | `firmware/STM32_Controller/System/FATFS` | ChaN；原文件许可/免责声明保留 |
| Chart.js4.4.1 | `server/Robot_Server/static/vendor/chart.umd.min.js` | MIT；Chart.js Contributors |
| Three.js r160及OrbitControls | `static/vendor`，以及`motion-trainer.html`内嵌副本 | MIT；Three.js Authors |
| YuNet | `face_local/model_source.json`、`models/YuNet_LICENSE` | OpenCV Zoo，MIT；权重不随源码附带 |
| SFace | 同目录`SFace_LICENSE` | OpenCV Zoo，Apache-2.0；权重不随源码附带 |
| faster-whisper | pip依赖 | SYSTRAN，MIT；转换后的Whisper权重许可另行适用 |
| PyAV/OpenCV/NumPy等 | `requirements.txt`安装 | 各自上游许可；未把Python安装环境复制进项目 |
| ESP8266 Arduino Core、ArduinoJson | 构建依赖 | 各上游许可；未把开发环境或其库源码复制进项目 |
| 点阵字模 | `App/font_data.h`、`Hardware/15LCD/lcd_font.h` | 为兼容保留，包含原例程/字体生成数据；字体授权未独立核验，**不在项目MIT授权范围内** |
| SD表情/GUI美术、个人照片/录音 | 本源码不附带 | 使用者自行准备有权使用的素材 |

源码审阅可继续进行。对外发布前，发布者仍需确认点阵字体及原示例资源的再分发授权；如不能确认，可用已获授权字体重生成兼容位序的字模后再发布。下载脚本只处理有明确模型来源/哈希的权重，不证明训练数据可供任意用途。

Chart.js 与 Three.js 的完整MIT文本另见 `licenses/`。其他组件以其原始逐文件条款为准。
