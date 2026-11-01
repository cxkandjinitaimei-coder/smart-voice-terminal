> 2026-09-21 19:40 Codex 最新状态：中文 UTF-8 A5 帧头冲突已修复；识别状态/后台发送/AI回复入队已补齐；ESP→F407 局域网 OTA 已编译与主机回归通过，实板待验。首次使用交付/20260921_CN_ASR_OTA/firmware 中的合并固件，详见该目录验收说明与 HANDOFF。用户已确认字库初始化完成，不重灌；看门狗后续接入。此状态取代下文旧的“OTA不通/不烧Bootloader/字库未验”判断。

# 智能家居项目笔记：云端服务器全流程 + OTA

> 记录日期：2026-08-29
> 最近更新: 2026-09-20 16:54（★ **02OTA** 订正 §五.1 Flash 分区、§五.5 帧协议与 App 基址）
> 最后操作者: 02OTA (Claude Code)
> 项目：ESP8266（网关桥）→ Flask 中转站 → DeepSeek AI → 决策回传 F407

---

## 一、系统架构总览

```
F407 ──UART──► ESP8266 ──HTTP POST──► Flask 中转站 ──► DeepSeek（AI）
   ◄────────      ◄──────HTTP响应──────      ◄──JSON决策──
```

- **ESP8266**：只做"桥"。收 F407 的 `@SENSOR:`/`@USER:` 数据 → POST 给中转站 → 解析 AI 决策 JSON → 用 `@AI:{...}` 转发回 F407。
- **中转站（flask_relay/app.py + ai_relay.py）**：收数据 → 写 `data_history.txt` → 读最近 N 条当上下文 → 调 AI → 存 `decision_log.txt` → 返回统一 JSON。
- **统一决策 JSON**：`{"action":"fan_on","value":1,"message":"...","raw":"原始输出"}`
- **UART 帧协议**：`@AI:`（服务器→F407）、`@SENSOR:`、`@USER:`（F407→服务器）

### 中转站关键代码位置
| 文件 | 作用 |
|---|---|
| `app.py` | Flask 路由：`/` 仪表盘、`/api/data` 上报入口、`/api/history`、`/api/health` |
| `ai_relay.py` | 各家模型 OpenAI 兼容调用、key 轮换、JSON 解析 |
| `config.json` | providers 配置（deepseek 已启用）、key、actions、rules |
| `data_history.txt` | 环境数据历史 |
| `decision_log.txt` | AI 决策历史 |
| `templates/index.html` + `static/app.js` | 网页仪表盘（测试面板、曲线图、表格、5s 自动刷新） |

---

## 二、踩过的坑（务必记住）

1. **Windows 系统代理坑**：`requests` 会读系统代理设置（如 `127.0.0.1:20000`），代理软件没开会连不上外网。
   - 修复：`_call_chat` 里用 `requests.Session()` + `session.trust_env = False` 直连外网。
   - 现象：AI 调用报 `ProxyError: HTTPSConnection(host='127.0.0.1', port=20000)`。

2. **多进程占端口坑**：werkzeug 在 Windows 上设了 `SO_REUSEADDR`，多个 `python app.py` 能同时"绑定"5000，但只有先绑的能收到连接——请求打到哪个进程完全随机。
   - 排查：`Get-NetTCPConnection -LocalPort 5000 -State Listen` 看谁占端口；`Get-CimInstance Win32_Process -Filter "Name='python.exe'"` 看有哪些 python。
   - 解决：**只留一个中转站进程**，其余全杀掉。

3. **ESP8266 收包坑**：读响应必须 `delay()` 让 WiFi 协议栈处理收包，紧循环会一直拿不到数据。
   - 模式：等第一个字节（超时）→ `while (client.available() || client.connected())` 收完剩余 → 拆 HTTP header/body。

4. **HTTP vs HTTPS**：ESP 的 `WiFiClient` 只支持明文 HTTP。云服务器/隧道要提供 **HTTP** 端点；用 HTTPS 就得换 `WiFiClientSecure` + 证书，麻烦得多。

5. **PowerShell 传中文参数乱码**：curl.exe 在 PowerShell 5.1 里引号/中文编码有坑，测试用 `python -c` 或写脚本文件跑，别手敲 curl。

---

## 三、云端服务器全流程（阿里云轻量）

### 0. 实际部署记录（2026-08-29 ✅ 已完成并跑通）

**服务器信息**
| 项 | 值 |
|---|---|
| 公网 IP | `<SERVER_IP>`（ESP / 手机都连这个）|
| 私网 IP | `172.19.32.138`（阿里云内部用，外部不用管）|
| 系统 | Ubuntu 24.04（Python 3.12）|
| 登录用户 | `admin`（网页终端默认，操作要 `sudo`）|
| 代码位置 | `/home/admin/flask_relay` |
| venv 位置 | `/home/admin/venv`（flask 3.1.3 + requests 2.34.2）|

**实际操作流程**
```bash
# ① 本机 PowerShell：传代码（root 密码登录）
scp -r d:\ArduinoIDE\ArdProject\flask_relay user@<SERVER_HOST>:/root/

# ② 服务器网页终端（admin 用户）：把代码挪到自己名下，以后改代码不用 sudo
sudo mv /root/flask_relay /home/admin/flask_relay
sudo chown -R admin:admin /home/admin/flask_relay

# ③ 建虚拟环境 + 装依赖（在 /home/admin 下）
python3 -m venv /home/admin/venv
/home/admin/venv/bin/pip install flask requests
# 注：Ubuntu 24.04 直接 pip 装会报 externally-managed-environment，
#     用 venv 就绕开了，不用 --break-system-packages

# ④ 启动（后台常驻，关终端不掉）
cd /home/admin/flask_relay
nohup /home/admin/venv/bin/python app.py > server.log 2>&1 &
```

**验证三步**
```bash
cat server.log                           # 有 * Running on http://0.0.0.0:5000
curl http://127.0.0.1:5000/api/health    # 返回 {"status":"ok",...}
ss -tlnp | grep 5000                     # 0.0.0.0:5000 在 LISTEN
```

**防火墙（最容易漏的一步 ⚠️）**
- 阿里云控制台 → 轻量服务器 → 实例 → **防火墙** → **添加规则**
- 端口 `5000` / 协议 `TCP` / 来源 `0.0.0.0/0`
- 默认只放行了 22/80/443，**5000 必须手动加**，否则外网打不开
- 注意找"添加规则"按钮，别选"防火墙模板"

**访问**：手机用流量打开 `http://<SERVER_IP>:5000` → 仪表盘
**ESP 端**：`SERVER_IPS[]` 改成 `{"<SERVER_IP>"}`

### 1. 为什么能上云
- ESP 本身有外网能力（要调 AI 就得连出去），唯一在局域网里的只有**中转站**。
- **上云 = 把中转站搬到有公网 IP 的服务器上**，让它能被外网访问。

### 2. 选服务器
| 项目 | 建议 |
|---|---|
| 规格 | 最便宜轻量即可（2 核 2G），中转站内存占用几十兆 |
| 系统镜像 | **Ubuntu 22.04 / 24.04 LTS** |
| 地区 | 大陆（访问 DeepSeek 快）；**用 IP 访问不用备案，避开域名备案的麻烦** |
| 安全组 | 控制台防火墙放行 **5000 端口（TCP）**，否则外面连不进 |

### 3. 国内 vs 国外服务器
| | 国内 | 海外/香港 |
|---|---|---|
| 域名备案 | 必须（绑域名时） | 不用 |
| IP 访问 | ✅ 免备案 | ✅ |
| 国内访问速度 | 快、稳 | 香港还行，欧美偏慢 |
| 访问 DeepSeek | 快 | 多一段国际链路 |
| 访问境外网站 | 受限 | 不受限 |

**结论**：本项目链路全在国内（ESP 国内 → DeepSeek 国内），选**国内服务器 + IP 访问**最划算。要"域名 + 不备案"才选香港。

### 4. 部署步骤
```bash
# 1. SSH 连上服务器（控制台终端或 Xshell/Windows Terminal）
# 2. 装依赖
sudo apt update
sudo apt install -y python3-pip
pip install flask requests
# 3. 传代码：把 flask_relay 整个文件夹传上去（scp 或 git）
# 4. 填密钥：服务器上 config.json 里放 DeepSeek key（固件里永远不带 key）
# 5. 跑起来（常驻，关 SSH 不掉）
nohup python app.py > server.log 2>&1 &
```
- 验证：手机浏览器打开 `http://服务器公网IP:5000`，能见仪表盘即成功。
- 常驻更正规的做法：写一个 systemd 服务（`[Unit]` + `[Service] ExecStart=/usr/bin/python3 /path/app.py`）。

### 5. ESP 端改动
把 `SERVER_IPS[]` 里的局域网 IP 换成服务器公网 IP：
```cpp
const char* SERVER_IPS[] = {"123.45.67.89"};   // 云服务器公网IP
#define SERVER_PORT 5000
```

### 6. 手机访问方式
- 纯 IP：`http://123.45.67.89:5000`，手机浏览器存书签 / 添加到主屏幕（像 App 一样）。
- 想要域名：国内服务器要 ICP 备案（免费但等一两周）；香港服务器 + 便宜域名（如 `xxx.xyz`）免备案，A 记录指过去即可。

---

## 四、远程控制原理（重点）

### 核心：通信永远由 ESP 主动发起
```
ESP 连任意能上网的网络（手机热点/家里路由/学校WiFi）
  → 主动外连 connect(云服务器公网IP, 5000)   ← 出站连接防火墙放行
  → 服务器响应原路返回
```
- **服务器找不到设备**：ESP 躲在 NAT 后没有公网 IP，所以**设备主动上报，服务器在响应里"捎带"命令**。
- 手机发指令 → 服务器缓存到"待下发队列" → 等 ESP 下次上报（≤30s）→ 响应里带上 → ESP 转发给 F407。

### 多设备：加 `device_id`
```
设备A fan#1   ESP ──POST {device_id:"fan_1",   ...}──►┐
设备B light#1 ESP ──POST {device_id:"light_1", ...}──►┤ 服务器按 device_id
设备C ac#1    ESP ──POST {device_id:"ac_1",    ...}──►┘ 存历史+待办命令
```
三处改动：
1. **ESP**：`#define DEVICE_ID "fan_1"`，body 里加 `bodyDoc["device_id"] = DEVICE_ID;`，每台设备烧不同 ID。
2. **中转站**：历史记录带 `device_id`；加"待下发命令"表（device_id → 命令），`/api/data` 响应带出该设备待办命令。
3. **网页**：顶部设备列表，每设备一张卡片（最新数据 + 发指令按钮）。

---

## 五、OTA（无线刷固件）

### 0. 一句话原理
OTA = 不插 USB，用 WiFi 把新固件写进 flash 的**备用区**，校验通过后重启进新固件。
写坏/中途断电 = 变砖，只能回插 USB 用串口重刷。

### 1. 关键前置：Flash 分区（不改分区就没有备用区）
- Arduino IDE → 工具 → Flash Size **按板型选**（★ 2026-09-20 16:08 订正 —— 原来写死的 `4MB (FS:2MB OTA:1019KB)` **只对 NodeMCU 成立**）：

  | 板子 | Flash Size 必须选 | 理由 |
  |---|---|---|
  | **ESP-01S**（扩展板上要换的裸模块） | **`1M (64K SPIFFS)`** | ESP-01S = **1MB**（25Q80）。**这是 1MB 上唯一带 OTA 的档**（对应 `eagle.flash.1m64.ld`，两个 ~470KB 的槽）。选成 `1M (no SPIFFS)` / `1M (496K SPIFFS)` 之类会**编译过、烧录过、跑起来也正常，但 OTA 一调 `Update.begin()` 就失败**，而且报错看起来像网络问题 |
  | **NodeMCU**（现在实际在跑的整板） | **`4MB (FS:2MB OTA:~1019KB)`** | 4MB 的板子，本文档原来那个值对 |

- ⚠️ **ESP-01（512KB / 25Q40）做不了 OTA** —— 一个槽都放不下。它和 ESP-01S **外观几乎一样**，**只能靠 Flash Size 设置、或上电打 `ESP.getFlashChipRealSize()` 分辨**。
- ★ **决定 OTA 成败的数是 `ESP.getFreeSketchSpace()`**，不是 `getFlashChipSize()` —— 前者才是 `Update.begin()` / `ESP8266httpUpdate` 真正比对的。
  ESP8266 **没有独立 OTA 分区**：新固件写在当前 sketch 后面的空档里、再翻盖过去 ⇒ **sketch 区必须是固件体积的两倍**。（2MB 的板上这一句实测是 `getFreeSketchSpace()` = 491520，而 `getFlashChipRealSize()` = 2097152。）
- **备用区大小 = 固件 `.bin` 体积上限**，固件别超过它。
- 板子上电先跑**引导区（bootloader）**，由它决定"跑当前固件 / 跑备用区里的新固件"。OTA 就是往备用区写入 → 重启 → bootloader 激活新固件。

### 2. 两条升级路径（重要区分 ⚠️）
| | ArduinoOTA | HTTP OTA（云端）|
|---|---|---|
| 通信对象 | 电脑（Arduino IDE） | 云服务器**公网 IP** |
| 覆盖范围 | ❌ **仅局域网**（电脑和设备必须同一 WiFi，靠 mDNS 找设备） | ✅ **不限局域网，只要 ESP 能上网**（手机热点/家里/学校 WiFi 都行） |
| 用途 | 开发期图省事 | 生产/远程管理 |

> **结论：真正的远程 OTA 是不限局域网的 HTTP OTA。** ArduinoOTA 只是开发期便利，一离开同一 WiFi 就用不了。用户关心的"只要连上网都能升级"，靠的就是 HTTP OTA 这条链路。

### 3. HTTP OTA 工作流（已写进 ESP 代码 `checkForUpdate()`）
```
┌──────────── ESP8266（连任意能上网的网络，手机热点也行）────────────┐
│                                                                     │
│  loop() 里每 60 秒调 checkForUpdate()                                │
│    │                                                                │
│    ├─① GET http://<SERVER_IP>:5000/api/ota/info?token=xxx           │
│    │      （和数据上报是同一条链路：ESP 主动外连云端公网 IP）         │
│    │                                                                │
│    ├─② 服务器返回 {"version":"1.0.0",                               │
│    │       "url":"http://<SERVER_IP>:5000/api/fw/sketch.ino.bin?token=xxx"}│
│    │                                                                │
│    ├─③ 对比：云端 version > 本机 FW_VERSION 才更新                   │
│    │                                                                │
│    └─④ ESPhttpUpdate.update(url)                                    │
│          ├─ 下载 .bin ──► 校验 ──► 写入备用区 ──► 自动重启进新固件    │
│          └─ 失败只打错误码，当前固件照常跑，不会半途变砖             │
└──────────────────────────────────────────────────────────────────────┘
```

### 4. 服务器端（已写进 app.py）
- `config.json`：`firmware_version`（云端版本号）+ `firmware_file`（.bin 文件名）
- `GET /api/ota/info`：返回最新版本号 + 带 token 的下载地址
- `GET /api/fw/<文件名>`：下载固件（query 带 token 放行）
- `.bin` 放服务器 `/home/admin/flask_relay/firmware/` 目录

### 4.5 发布流程（PC 端更新 → 上传 → 目标机自动升级）
```
┌──── PC（开发机）──────────────────────────┐
│  ① 改代码，Arduino IDE 编译出 sketch.ino.bin  │
└──────────────┬───────────────────────────────┘
               │ ② scp 上传到服务器 firmware/ 目录
               ▼
┌──── 云端服务器 <SERVER_IP> ────────────────┐
│  ③ 改 config.json：firmware_version → 1.1.0   │
└──────────────┬───────────────────────────────┘
               │ ④ ESP 每 60 秒主动轮询 /api/ota/info
               ▼
┌──── 目标机 ESP8266 ────────────────────────┐
│  ⑤ 发现 云端1.1.0 > 本机1.0.0 → 下载 → 更新   │
└──────────────────────────────────────────────┘
```
- **不是服务器"推"，是设备"拉"**：ESP 在 NAT 后没有公网 IP，服务器连不上它，
  只能让设备定时来问。上传好后所有设备最多 60 秒内自动更新，无需逐台操作。
- 上传方式：`scp sketch.ino.bin admin@<SERVER_IP>:/home/admin/flask_relay/firmware/`
  然后改 config.json 版本号 → 重启服务器即可。

### 5. F407 升级：ESP 当"搬运工"（IAP）
F407 没 WiFi，ESP 代它从云端拿固件、再 UART 转发给 F407 的 bootloader：
```
云端服务器 ──HTTP──► ESP8266 ──UART帧──► F407（跑 IAP bootloader）
f407.bin            逐块下载             按帧转发写 flash
```
- **ESP 不能整包缓冲 f407.bin**（RAM 只有几十 KB），必须"下载一块 → 转发一块 → 下载下一块"流式搬运。
- UART 帧协议（发给 F407）：★ **2026-09-20 16:08 订正 —— 行里原来的 `SOF 0xAA / CMD / LEN / PAYLOAD / CRC16 / EOF 0x55` 是早期草案**，实际以 **`合作表.md §4.3` 的封装为准**（那是唯一会真正被实现的）：
  ```
  0xA5 0x5A | type(1) | seq(2,LE) | len(2,LE) | payload | xor(1)
  ```
  与现有 `@...\r\n` 文本行**共存**。
  ⇒ **★ 这点很重要**：本文档原方案、`计划蓝图.md §4.5`（Hex 文本帧）、`合作表.md §4.3`（二进制帧）**三份文档写的是三个不同的协议**，现已收敛到 `合作表 §4.3` 这一个。
  ⇒ **为什么二进制这次可以**：`计划蓝图 §4.5` 反对二进制的理由是「数据里出现 `\r`/`\n`/`#` 会被行解析提前截断」—— 那针对的是**行协议里的裸二进制**。`0xA5 0x5A + len(2,LE)` 是**长度定界**的，这几个字节不再有特殊含义，所以安全；而且带宽回到 **1×**（44KB，而不是 hex 编码的 88KB）。
  命令分：开始更新（带总大小 + App 基址）/ 数据块 / 结束校验 / 跳转App。
  - ✅ **App 基址已定案 = `0x08008000`（★ 用户 2026-09-20 拍板，2026-09-20 16:54 就地订正本文档）**。本文档原写 `0x08010000` —— 那是老方案：只给 Slot A 一个 64KB 扇区 ⇒ 余量只有 **1.45×**，而且 **Sector 1~3 共 48KB 白扔**（bootloader 只需要 Sector 0）。
    新值：App 基址 `0x08008000`、Slot A = Sector 2+3+4 = **96KB** ⇒ 余量 **2.2×**、不浪费扇区。
    ⇒ 顺带钉一条 `计划蓝图 §4.4` 漏写的规则：**固件体积上限 = `min(Slot A, Slot B)` = 96KB**（Slot B 128KB 比 Slot A 大 ⇒ 能下进 B 却拷不进 A，表现为「升级成功但变砖」）。
    ⇒ 详细推演见 `规划部分/IAP布局.md §3`。
  - ★ 定了之后要**同时改四处**，漏一处就「跳过去但跑不起来」：本文件 · `docs/项目/项目书.md:129` · Keil `Options for Target → Target → IROM1 Start` · **`system_stm32f4xx.c` 的 `VECT_TAB_OFFSET`**（最容易被忘，见下）。
  - ★★ **`VECT_TAB_OFFSET` 是最阴的一个，而且本板现在就是踩着的**：bootloader 跳转前设的 `SCB->VTOR = SLOT_A_BASE`，会被 **App 自己的 `SystemInit()` 再覆盖一次**。本板 `ST32/AIAssistant/User/system_stm32f4xx.c:309` 现在正是 `#define VECT_TAB_OFFSET  0x00`，而 `:454` 无条件执行 `SCB->VTOR = FLASH_BASE | VECT_TAB_OFFSET;` ⇒ **VTOR 被打回 `0x08000000`**。
    ⇒ 所以「Keil 里改 `IROM1 Start`」**是不够的**，必须同时把 `:309` 改成 `VECT_TAB_OFFSET = SLOT_A_BASE - FLASH_BASE`（App 基址 `0x08008000` ⇒ 填 `0x8000`；`0x08010000` ⇒ 填 `0x10000`）。**漏了它，现象就是「跑着跑着随机死机」，第一眼看上去像 App 的 bug。**
- 因为 ESP 连的是云端公网 IP，所以 **F407 的固件升级同样不限局域网**——只要 ESP 能上网，在哪都能升级 F407。

### 6. 当前进度（2026-08-29）
- ✅ ESP `checkForUpdate()` 已写好（sketch_aug28a.ino）
- ✅ 服务器 OTA 两接口已写好（app.py）
- ⬜ **未部署**：编译 .bin → 传云端 → 改版本号 → 实测（用户选择先理解原理，暂不改代码）

### 7. 建议路线
开发期用 ArduinoOTA 图省事 → 设备多了/要远程了再上 HTTP OTA（固件放阿里云 + 云端版本号比对）。

### 8. 云端下发固件（★ 2026-09-24 落地）：**还是"设备拉"，但拉的是 4096 B 一块**

用户 2026-09-24 定案「**全部上云，包括 OTA**」。上云后 §5.5 那条
「网页 → Flask → **ESP 自己的 `:8080/f407/`** → 板上按 K-UP」**物理上做不到** ——
Flask 在公网、ESP 在 NAT 后，**服务器连不进** `http://<esp_ip>:8080/`。
⇒ 改成和 §5.3「ESP 自己拉」**同一个形状**：云端只负责**存着 + 发块**，由 **ESP 按块来拉**。
★ 注意这**不是**新发明的协议：`合作表.md §4.6` 早就登记了「② 段收敛成一套：ESP 当 HTTP 客户端 + 分块」。

```
网页 / CLI ──POST /api/iapl/job ──────► 云端任务表（进程内 dict）
ESP ──每秒 POST /api/data ────────────► 响应里捎带 iap 公告     ← 不新增轮询
ESP ──@IAP:REQUEST,total,crc ─────────► F407                   ← 屏上出 K-UP 确认页，一个字都还没擦
【用户按板上 K-UP】                                             ← 唯一的物理批准
ESP ──GET /api/iap/chunk?token=..&device_id=..&seq=N ─► 云端    ← 4096 B/块
ESP ──0x10 数据帧（128 B/帧）─────────► F407                   ← 复用已验证的那套帧
ESP ──END → COMMIT → @IAP:JUMP ───────► F407                   ← 必须一口气发完，中间不许插长阻塞
ESP ──POST /api/iap/report{slot_crc32} ─► 云端                ← 判 PASS / FAIL
```

**与局域网那条（§5.5）的对照**

| | §5.5 局域网中转（保留，交付包在用） | §8 云端下发（本次新增） |
|---|---|---|
| 谁发起 | **服务器**打 `http://<esp_ip>:8080/f407/` | **ESP** 打 `https://<云端>/api/iap/chunk` |
| 前提 | Flask 与 ESP **同一个局域网** | ESP **能上网**即可（穿 NAT） |
| 传输 | 服务器整包 multipart 推给 ESP | ESP 按 **4096 B/块** 拉 |
| 界面 | 网页走 `/api/otactl/*`（`C-16`） | 网页走 `/api/iapl/*`（`C-17`） |
| 固件目录 | `firmware_f407/`（同一个，**只读**） | 同上（同源，不另建目录） |

**★ 几条容易踩的（每条都有代价，别"顺手"改回去）**

- **块 = 4096 B，不是 128 B**：werkzeug **每个响应都 `Connection: close`、没有保活** ⇒ 按 128 B/帧去拉，
  96 KB = **768 条 TCP 连接**（40~150 秒）；4096 B/块 ⇒ **24 条**，那 768 次帧往返全留在**本地 921600 UART** 上。
  设备侧宏 `IAP_PULL_BLOCK` 必须与云端 `BLOCK_BYTES` 一致 —— **云端把这个值随公告发下去、设备当场对账**，不一致直接拒收。
- **末块不补齐**（与早先那版"补零到 4096"相反）：不补才换来 `zlib.crc32(b"".join(chunks)) == job.crc32`
  这条**可断言的不变量**。设备本来就按 `total` 截断。
- **进度服务端自己数**：设备**不每块上报**（每块报一次会把 24 条 TCP 变 48 条）。
  设备只在**换阶段**和**最终结果**时各报一次。
- **两个 CRC 别搞混**：`@IAP:REQUEST,total,crc` 里那个是**原文** CRC；**PASS 判据** `slot_crc32` 是
  **按 `0xFF` 补到 96 KB** 的 padding CRC（`MAX_APP = 98304`）。两个都在用，各判一件事。
- **`/api/iap/`（设备端点，token 放行）与 `/api/iapl/`（控制台端点，必须登录）只差一个字母，是刻意的** ——
  与 `/api/ota/` vs `/api/otactl/` 同一个理由。**绝不许把 `/api/iapl/` 放进 token 白名单**，
  那等于把"谁能给板子刷固件"交给任何拿到 token 的 URL。
- **互斥**：IAP 与**流式录音**不许并发（既有约束，§7.4）；云端下发停在"等按 K-UP"那一态时，
  ESP 也**不去刷自己**（否则会把 F407 晾在确认页上）。
- **F407 侧一个字都不用改**：`@IAP:REQUEST/INFO/JUMP` 的语义与 §5.5 那条**逐字相同**，
  02 的 `IAP_OnFrame` + `0x10/0x13/0x15` 已够用。⚠️ 但 **`@IAP:COMMIT` 之后只剩 15 秒**
  （`IAP_JUMP_TIMEOUT_MS`）⇒ END/COMMIT/JUMP 必须一口气发完。

**★ 两个 `SERVER_IPS` 相关的坑（上云后才会显形）**

- `SERVER_IPS[0]` 现在是**公网 `<SERVER_IP>`** ⇒ **「静默回退到公网」从"故障"变成"设计"**。
  ⚠️ **别再把"板子连上了公网"当 bug 去查**。判据：`DBG`（GPIO2）那句 `[DISC] 作废已发现的 <IP>` ——
  **它出现**才说明"本机问不到了"。✅ 局域网联调的诚实性**没丢**：自动发现到的地址**永远排最前**，
  所以**本机 `app.py` 一跑，ESP 照旧用本机**。
- `checkForUpdate()` 原来**只退到 `SERVER_IPS[0]`、不走候选表**（它自己的注释写着这个 bug "已修"，**其实没修**
  —— 只是把写死的 IP 换成了另一个写死的 IP）。`[0]` 一改成公网，这个洞会变成"**永远只去公网查**"、
  局域网联调时更隐蔽 ⇒ **已一并改成走统一候选函数**。

**自测与验收现状**

| 项 | 状态 |
|---|---|
| `flask_relay/iap_pull_selftest.py` | ✅ **38 项全绿**（纯 PC、不绑端口、有副作用的全部静音） |
| ESP `arduino-cli compile` | ✅ exit 0 —— **IRAM 60987 → 60987（一个字节没涨）**，RAM +1816，Flash +7280 |
| 抽取的等价证明 | ✅ `sketch_aug28a/tools/verify_mcu_extract.py` 全绿 |
| 模块卡 | `项目地图.md` **B-11**（ESP 侧）/ **C-17**（服务端侧）；`合作表.md §4.5 / §4.6 / §4.8` |
| **真机验收** | 🔴 **还没做** —— 要人在板前按 K-UP（建任务 → ≤2 秒被发现 → 出确认页 → 24 条 chunk → `slot_crc32` 一致） |

---

## 六、待办清单（后续实现）

- [x] 中转站加 token 鉴权（暴露公网后必须，防盗用 AI 额度）——已完成 2026-08-29
- [x] ESP 加 ArduinoOTA（开发期）——已完成
- [x] HTTP OTA 代码（ESP `checkForUpdate` + 服务器 `/api/ota/info` `/api/fw/`）——已写完
- [ ] HTTP OTA 部署实测：编译 .bin → 传云端 firmware/ → 改版本号 → 验证自动更新
- [ ] 多设备：ESP 加 `device_id`，中转站加命令队列，网页加设备列表
- [ ] 远程命令通道（手机指令 → 待办队列 → ESP 上报时带回）
- [ ] F407 IAP：ESP 流式下载转发 f407.bin + F407 侧 bootloader 烧录对接
- [ ] 数据存储升级：`data_history.txt` → SQLite/MySQL + 历史曲线统计
