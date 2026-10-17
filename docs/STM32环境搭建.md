# STM32环境搭建

适用工程：`firmware/STM32_Controller`，芯片为**STM32F407VET6，片内Flash512KiB**。当前是裸机程序，没有FreeRTOS或HAL/CubeMX生成依赖。

## 一、软件与库

| 组件 | 版本/用途 | 是否随源码提供 |
| --- | --- | --- |
| Keil MDK/uVision | Windows下打开和构建`.uvprojx` | 不附带，官网下载/按合法授权安装 |
| ARMCC | **5.06 update 7，build 960** | 不附带，需在Keil中可选择ARM Compiler 5 |
| STM32F4xx DFP | **Keil.STM32F4xx_DFP.3.1.1** | Pack Installer安装 |
| CMSIS、ST SPL | 启动、寄存器与标准外设库 | 源码的Core/Lib等目录已提供 |
| FatFs及项目驱动 | SD文件系统、LCD、麦克风、摄像头、IAP等 | 已提供，不需另找F103案例混入 |
| Python | 可选构建/版本/HEX合并工具 | 与网页使用同一虚拟环境即可，构建工具自身仅需标准库 |
| SWD下载工具 | J-Link或ST-Link及厂商驱动/软件 | 有线首次安装时需要；仅编译不需要接调试器 |
| USB-TTL与串口终端 | 3.3V逻辑，F407诊断日志 | 联调时可选；不等于已经具备系统ROM串口烧录接线 |

Keil入口：<https://www.keil.com/download/product/>。当前应用镜像超过传统32KiB评估版限制，应使用能支持本工程大小的有效工具授权；不要通过更换代码布局来绕过工具许可限制。

## 二、安装并核对工具链

1. 安装MDK/uVision，保留原安装路径即可，不要求使用某个盘符。
2. 确认安装ARMCC5.06u7，在uVision的工程目标设置/编译器管理中选择该编译器。仅有ARM Compiler 6时先补齐5.x环境；不要直接切换后把兼容性报错当成源码损坏。
3. 在Pack Installer搜索并安装`Keil.STM32F4xx_DFP` **3.1.1**。若出现缺Pack弹窗，先处理Pack，不重建工程替换芯片。
4. 核对目标设备为`STM32F407VETx`。外设库、启动文件、include和scatter均在源码树内，不需要链接回原开发目录。
5. 选择实际J-Link/ST-Link时，只调整本地调试/下载器设置；代码构建不依赖调试器已连接。工程保存的旧下载器选择不保证适用于你的探头。

当前代码依赖ARMCC5语法及启动方式，未验证ARM Compiler 6。工具若提示找不到`armcc.exe`，应修复Keil的编译器路径，而非把某个安装目录写进业务代码。

## 三、三个工程怎么选

| 工程文件（相对STM32_Controller） | 应用地址/职责 | 使用场景 |
| --- | --- | --- |
| `User/STM32_Controller.uvprojx` | `0x08000000`普通应用 | 无Bootloader的单独调试，会与Boot区域重叠 |
| `User/STM32_Controller_OTA.uvprojx` | `0x08008000`，Slot A应用 | 正常机器人固件与后续OTA |
| `Bootloader/STM32_Bootloader.uvprojx` | `0x08000000`，S0引导 | 首次有线安装/维护Bootloader |

需要保留OTA能力时使用**Bootloader + OTA应用**组合，不把普通应用烧到Flash起始处覆盖Bootloader。完整地址表见[Flash与OTA说明](flash-and-ota.md)。

## 四、在Keil中构建

先在项目根目录同步版本（下例使用已创建的Windows虚拟环境）：

```powershell
.\.venv\Scripts\python.exe tools/set_version.py
.\.venv\Scripts\python.exe tools/set_version.py --check
```

随后打开所需`.uvprojx`，选中对应Target并执行Rebuild，检查编译结果与生成文件。正常已验证环境的三个工程均为0错误、0警告；若报错，先保留完整日志核对工具链，而非只看最后一行。

| 产物 | 位置（相对STM32_Controller） |
| --- | --- |
| 普通应用 | `User/Objects/STM32_Controller.hex`及`.axf` |
| OTA应用 | `User/ObjectsOTA/STM32_Controller_OTA.hex`及`.axf` |
| Bootloader | `Bootloader/Objects/STM32_Bootloader.hex`及`.axf` |

Keil生成HEX；需要OTA的BIN时可用下一节构建脚本导出。UTF-8中文C/H要保留现有BOM，原供应商文件保持原编码；不要为了注释格式统一而批量转为ANSI或剥BOM。`boot_enter`内联汇编的格式化开关必须保留。

## 五、用脚本顺序构建并导出BIN

把下面示例的Keil路径替换为本机实际安装位置：

```powershell
.\.venv\Scripts\python.exe tools/build_stm32.py --keil "C:/Keil_v5/UV4/UV4.exe" --fromelf "C:/Keil_v5/ARM/ARMCC/bin/fromelf.exe"
```

该路径只是示例，不要求安装在C盘。也可设置`KEIL_UV4`和`KEIL_FROMELF`环境变量后运行脚本。它依次重编三个工程，把日志放根`.build`，并从AXF导出同目录BIN；**只构建，不下载或擦除设备**。

OTA使用`User/ObjectsOTA/STM32_Controller_OTA.bin`。BIN是原始字节，不携带绝对地址；有线写入BIN时必须明确装载地址。HEX自身包含地址，不能因为后缀相同就把普通应用和OTA应用混用。

## 六、首次引导安装与调试

先确认确实需要初始化/迁移，并备份原板内容。新板或明确迁移时，将Bootloader与OTA应用按各自地址写入。若需要单一HEX，构建完成后可在项目根执行：

```powershell
.\.venv\Scripts\python.exe tools/merge_bootstrap.py firmware/STM32_Controller/Bootloader/Objects/STM32_Bootloader.hex firmware/STM32_Controller/User/ObjectsOTA/STM32_Controller_OTA.hex .build/BOOTSTRAP.hex
```

合并工具检查输入地址及HEX校验和，不包含META。旧META的CRC可能与新应用不符，首次迁移需按[Flash文档](flash-and-ota.md)处理；合并成功不等于可以忽略旧元数据直接覆盖运行板。

SWD方式按探头接SWDIO、SWCLK、GND及所需参考电压/复位信号，按开发板和探头说明操作。ST-Link可用STM32CubeProgrammer，J-Link使用其支持的下载工具；软件和驱动从相应厂商取得。使用系统ROM串口烧录时，另需确认BOOT配置和芯片支持的ROM下载串口；**PA2/PA3有诊断日志不代表它就是ROM下载口**。

烧录后退出下载启动模式并复位，观察Bootloader跳转和应用主循环。已有正确引导布局后，日常只通过OTA更新应用BIN，首次有线安装与OTA并不冲突。

## 七、运行所需硬件/素材

系统时基使用TIM6，每1ms采样按键。F407与ESP板间为USART6 PC6/PC7、921600；F407日志为USART2 PA2/PA3、115200。接线、供电和PCA9685独立舵机电源见[硬件说明](hardware.md)。

屏幕动画文件放SD卡，扩展字库使用W25Q，不由Keil构建自动写入。编译通过但没有SD素材时，动画可能回退或报缺文件；请按[模型与工具说明](识别模型与工具环境搭建.md)准备。摄像头假设是带FIFO/时钟的OV7725模块，F103案例的引脚不能直接照搬。

## 八、常见问题

| 现象 | 检查 |
| --- | --- |
| 找不到Pack/芯片 | STM32F4xx_DFP3.1.1是否安装并被当前Keil识别 |
| 编译器不支持语法/汇编 | 当前Target是否ARMCC5.06u7，不是自动选中的AC6 |
| 代码大小超过许可限制 | 当前Keil授权是否支持实际镜像大小，不是应用槽不足 |
| 一片中文附近的字符串/引号错误 | 源文件编码与BOM是否被编辑器改坏，查看第一个报错而非后续连锁错误 |
| 能编译但下载失败 | 实际探头、驱动、供电、SWD接线和Flash算法；构建成功不证明下载链路可用 |
| Boot后报告A槽CRC不符 | 应用是否链接在Slot A、META是否遗留旧CRC，按迁移文档核对 |
| 有线写入后OTA没了 | 是否误刷普通应用覆盖Bootloader，不能靠重新上传普通应用恢复布局 |
| 后续加缓冲或RTOS链接溢出 | 当前RAM余量紧张，重新分析SRAM/CCM、栈及DMA可达区域 |

返回[环境搭建总入口](环境搭建与依赖说明.md)。
