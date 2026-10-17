# 片内 Flash 与 OTA

目标为 STM32F407VET6，片内 Flash 512KiB，布局 ID `0x00020001`。唯一驱动定义在 `Hardware/21IAP/iap_flash.h`。

| 区域 | 扇区 | 起始 | 结束（含） | 大小 | 内容 |
| --- | --- | --- | --- | --- | --- |
| Boot | S0 | `0x08000000` | `0x08003FFF` | 16KiB | 引导代码，末 32B 为布局描述符 |
| META | S1 | `0x08004000` | `0x08007FFF` | 16KiB | 序号、状态、长度及 CRC 等日志记录 |
| Slot A | S2–S5 | `0x08008000` | `0x0803FFFF` | 224KiB | 运行应用 |
| Slot B | S6–S7 | `0x08040000` | `0x0807FFFF` | 256KiB | 完整升级镜像暂存，不原地执行 |

有效应用最大值是 `min(A,B)=224KiB=229376B`，不是 256KiB。布局没有独立空闲扇区，不能随意借给参数保存。SD/W25Q 不属于这张表。

## 下载与启动

1. 应用收到升级请求后等待 PA0/K-UP 或 PE3/KEY1 实体确认，尚未确认不擦写。
2. BEGIN 校验长度/布局后延后擦 B；DATA 按序写入并回 ACK。
3. 完整镜像 CRC32 通过，才把元数据写为 READY。
4. 复位进入 Bootloader；校验 B，复制到 A，再校验 A并记确认状态。
5. 检查向量、设置栈和中断向量后跳到 A；应用/ESP 回传版本和校验状态供网页收尾。

META 为空时允许检查向量后启动 A；确认状态会按记录校验 A；校验失败停留引导诊断。CRC32 不是签名，本实现不提供安全启动认证。

## 首次有线安装

先确认芯片及布局。编译独立 Bootloader HEX 和 **OTA 应用 HEX**。有线工具可分别按 HEX 的绝对地址写入；需要单文件时：

```powershell
python tools/merge_bootstrap.py firmware/STM32_Controller/Bootloader/Objects/STM32_Bootloader.hex firmware/STM32_Controller/User/ObjectsOTA/STM32_Controller_OTA.hex .build/BOOTSTRAP.hex
```

合并文件不包含 META。首次迁移若板上已有旧元数据，需通过有线烧录工具明确擦除 S1，或按首次整片初始化流程处理；仅覆盖 A、保留旧 CRC 元数据可能让 Bootloader 拒绝启动。**烧录工具的整片擦除会删除全部片内内容**，本源码工具不会代执行。

以后常规升级只上传 OTA 应用 BIN，不再有线覆盖 Bootloader。本次交付是源码整理，不是新的硬件升级交付，未给设备烧录。

## 掉电和回滚的实际边界

下载失败/未提交时 A 不动。安装 B→A 期间，A 会被擦写；本布局没有另一份可直接运行的旧 A。不能宣称完整双运行槽、任何时刻掉电都能无损回滚。网页的传输、安装和重启校验阶段也不能合并成一个成功标志。
