# 当前硬件与接线

本版本主链路是 **STM32F407VET6 + NodeMCU/ESP8266 网关 + 独立 ESP32-S3 无线相机**。S3 相机直接通过 Wi-Fi 上传 JPEG；F407 不承担当前相机图像采集，也不需要连接 OV7725 数据总线。

以下按当前驱动的信号名列出；排针实体编号与供电脚须按开发板及模块原理图确认。所有逻辑按 3.3V 电平，共地；电源接口不等于 MCU 普通 GPIO。

| 外设 | 信号 → F407 |
| --- | --- |
| ESP8266 USART6 | PC6 TX → ESP GPIO3 RX；PC7 RX ← ESP GPIO1 TX；921600/8N1 |
| 调试 USART2 | PA2 TX、PA3 RX，115200/8N1 |
| INMP441/I2S2 | PB10 CK、PB9 WS、PC3 SD；当前选左声道，L/R 接 GND |
| DHT11 | PE0 DATA；供电与单总线上拉按实际模块确认 |
| PCA9685 | PB6 SCL、PB7 SDA，I2C 地址 0x40；舵机独立电源并共地 |
| SDIO | PC12 CK、PD2 CMD、PC8 D0，一位模式 |
| W25Q/SPI1 | PB0 CS、PB3 SCK、PB4 MISO、PB5 MOSI |
| LED PWM | PA6 TIM3_CH1、PA7 TIM3_CH2 |
| 实体按键 | PA0/K-UP 高有效；PE3/KEY1 低有效，接 GND |

第二只麦克风及扬声器尚未在本版应用中形成完整的双声道采集/播放链。它们不能仅凭预留接口就写成已启用功能。

## 历史 OV7725 FIFO 接口（已退役）

以下映射只用于复现实验代码和排查旧飞线，不是当前版本的接线要求。当前相机使用独立 ESP32-S3 模块，不能把这张表接到主链路后期待网页收到图像。历史驱动入口为 `Hardware/23OV7725/camera_ov7725_pins.h`。

| 模块信号 | MCU |
| --- | --- |
| D0 / D1 / D2 / D3 | PC0 / PC1 / PC2 / PC4 |
| D4 / D5 / D6 / D7 | PE1 / PE5 / PE6 / PD3 |
| SCL/SIOC | PB8 |
| SDA/SIOD | PB11 |
| VSYNC/VSY | PA8 |
| WEN/WREN | PD11 |
| RCLK/RCK | PD6 |
| WRST | PD12 |
| RRST | PA1 |
| OE/CS | PA5 |

历史驱动假设模块具备配套 FIFO/时钟电路，没有额外分配 XCLK/PCLK/HREF 引脚。它仅保留作早期实验参考；裸传感器或没有时钟的板不能按本表直接替换。VSY 与 WEN 接反、数据位错线、线长/接触和供电问题均可能造成错色横条。当前 S3 相机应按其专用模块引脚和烧录说明接线，软件时序修改不能代替接线验证。

## LCD 16 位 FSMC 总线

CS=PD7，RS=PD13，WR=PD5，RD=PD4，背光=PB1（高有效），RST 接板级复位。

| 数据位 | MCU | 数据位 | MCU |
| --- | --- | --- | --- |
| D0 | PD14 | D8 | PE11 |
| D1 | PD15 | D9 | PE12 |
| D2 | PD0 | D10 | PE13 |
| D3 | PD1 | D11 | PE14 |
| D4 | PE7 | D12 | PE15 |
| D5 | PE8 | D13 | PD8 |
| D6 | PE9 | D14 | PD9 |
| D7 | PE10 | D15 | PD10 |

此表描述驱动已使用的信号，不能据此将开发板未列出的板载引脚全部判为空闲。NRF/触摸等接口在本应用未启用，也不代表其物理走线不存在。完整空闲引脚审核仍需当前开发板和扩展板原理图。

三舵机默认契约：S1 左右 15~165°/回正90°；S2 上下0~90°/回正30°；S3正面旋转0~180°/回正90°。第三轴90°横屏。网页共享值在 `static/servo-contract.json`，软件统一绝对角。
