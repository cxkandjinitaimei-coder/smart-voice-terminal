# ESP 网络控制：原理 + 实现

> 记录日期：2026-08-29
> 目标：手机/网页在任意地方控制家里的 ESP 设备（风扇、灯、空调…），支持多设备。

---

## 一、核心原理（必须理解）

**通信永远由 ESP 主动发起，服务器只能在响应里"捎带"命令。**

```
ESP（在任意能上网的网络，躲在 NAT 后，没有公网 IP）
   │  ① 主动外连：connect(云服务器公网IP, 5000)
   ▼
云服务器（固定公网 IP，一直监听 5000）──► 收到 → 处理 → 响应
   │  ② 响应里带上"待下发命令"（若有）
   ▼
ESP 解析响应 → 转发给 F407 执行
```

为什么必须这样：
- 出站连接（ESP→服务器）防火墙天然放行，所以 ESP 连任何能上网的 WiFi 都能到达服务器。
- 服务器**找不到** ESP（ESP 没公网 IP），所以命令只能**排队等 ESP 来取**。

一句话：**设备上报 → 服务器在响应里带命令**，30 秒周期内命令就能下发到设备。

---

## 二、总体设计

```
设备A fan#1   ESP ──POST {device_id:"fan_1", sensor, user}──►┐
设备B light#1 ESP ──POST {device_id:"light_1", ...}────────►┤  云服务器
                                                            ▼
   数据按 device_id 分设备存（data_history + decision_log）
   待下发命令队列：device_id → 命令（网页/手机发的先存这）
                                                            ▲
手机网页：设备列表 → 选"风扇" → 发"我热了" ─────────────────┘
```

三个角色各管一块：
| 角色 | 改动 |
|---|---|
| **ESP** | 上报带 `device_id`；解析响应里的 `command` 字段并转发 F407 |
| **中转站** | 按 device_id 存数据；维护"待下发命令"队列，响应时带出 |
| **网页** | 设备列表；每设备一张卡片（最新数据 + 发指令） |

---

## 三、通信协议（字段约定）

### ESP → 服务器（POST /api/data）
```json
{
  "device_id": "fan_1",
  "sensor": "温度:28.5,湿度:65",
  "user": "我热了"
}
```

### 服务器 → ESP（响应，在原有决策 JSON 基础上加 command）
```json
{
  "action": "fan_on",
  "value": 1,
  "message": "好的，这就为您打开风扇。",
  "raw": "{...}",
  "command": {
    "cmd": "开灯",
    "ts": "2026-08-29 15:00:00"
  }
}
```
`command` 存在 = 有下发给本设备的命令；取走后服务器清除，下次上报不再带。

---

## 四、代码实现

### 1. ESP 端（sketch）

```cpp
// ---- 每台设备改这一个宏即可 ----
#define DEVICE_ID "fan_1"          // 设备B: "light_1", 设备C: "ac_1"

// 上报 body 里加 device_id
void sendToServer() {
  DynamicJsonDocument bodyDoc(256);
  bodyDoc["device_id"] = DEVICE_ID;
  bodyDoc["sensor"] = sensorData;
  bodyDoc["user"]   = userMsg;
  String body;
  serializeJson(bodyDoc, body);
  // ...原有 POST 逻辑不变
}

// 解析响应时取出 command 并转发 F407
void parseAIResponse(const String& respBody) {
  DynamicJsonDocument respDoc(1024);
  DeserializationError err = deserializeJson(respDoc, respBody);
  if (err) { /* 原有错误处理 */ return; }

  // 原有：action / value / message / raw 打印 + @AI: 转发

  // 新增：检查有没有下发给自己的命令
  if (respDoc["command"] && !respDoc["command"]["cmd"].isNull()) {
    const char* cmd = respDoc["command"]["cmd"];
    String frame = String("@CMD:") + cmd + "\n";
    f407.print(frame);
    Serial.println("[UART] -> F407: " + frame);
  }
}
```

### 2. 中转站端（app.py）

核心：一个"待下发命令"字典 + 一个写入接口 + 响应时带出。

```python
# 全局：device_id -> 待下发命令
_pending_commands = {}          # {"fan_1": {"cmd": "开灯", "ts": "..."}}
_pending_lock = threading.Lock()

# ① 新增路由：网页/手机发指令（先存队列，等设备来取）
@app.route("/api/command", methods=["POST"])
def api_command():
    body = request.get_json(force=True, silent=True) or {}
    device_id = (body.get("device_id") or "").strip()
    cmd = (body.get("cmd") or "").strip()
    if not device_id or not cmd:
        return jsonify({"ok": False, "error": "device_id 和 cmd 不能为空"}), 400
    with _pending_lock:
        _pending_commands[device_id] = {"cmd": cmd, "ts": now_str()}
    return jsonify({"ok": True})

# ② 在 /api/data 响应时带出本设备待办命令
def pop_pending_command(device_id):
    with _pending_lock:
        return _pending_commands.pop(device_id, None)

# /api/data 里构造响应时：
#   decision 是原有 AI 决策 dict
cmd = pop_pending_command(device_id)          # 设备来取走命令
if cmd:
    decision["command"] = cmd                 # 有命令就带上
```

### 3. 网页端（app.js）

```javascript
// 设备卡片：点"发送"后调中转站存命令
fetch('/api/command', {
  method: 'POST',
  headers: {'Content-Type': 'application/json'},
  body: JSON.stringify({device_id: 'fan_1', cmd: '我热了'})
});

// 设备列表：从 /api/history 里按 device_id 分组渲染
// 每个卡片显示：该设备最新 sensor + 一条指令输入框 + 发送按钮
```

---

## 五、完整时序（一次远程控制）

```
① 手机/网页点"风扇 → 发'我热了'"
      │ POST /api/command {device_id:"fan_1", cmd:"我热了"}
      ▼
② 中转站：存 _pending_commands["fan_1"] = "我热了"
      ▲
③ fan_1 的 ESP 下一次上报（≤30 秒）
      │ POST /api/data {device_id:"fan_1", ...}
      ▼
④ 中转站：pop 出 "我热了"，随决策一起返回
      │ {"action":"fan_on", ..., "command":{"cmd":"我热了", ...}}
      ▼
⑤ ESP：解析到 command → 转发 @CMD:我热了 → F407 执行
      ▼
⑥ F407 响应 → ESP 下次上报时把结果带回 → 网页能看到设备新状态
```

---

## 六、注意事项

1. **device_id 全局唯一**：每台设备烧录时改 `DEVICE_ID`，别重复。可以用"设备类型_序号"命名（fan_1、light_1）。
2. **命令一次性**：`pop` 取走即删除，避免重复执行。若 F407 没收到要重试，需加 ACK 机制（F407 回复后服务器才清除）。
3. **超时清空**：命令存太久设备没来取（设备离线），可加过期时间自动丢弃，防止"开机执行旧命令"。
4. **AI 调用时机**：`/api/command` 可以先只存指令不调 AI（设备下次上报时把指令作为 user 交给 AI 决策），也可当场调 AI 存好决策。推荐后者，网页能立刻看到决策结果。
5. **安全**：暴露公网后 `/api/command` 要加 token 校验，防止陌生人乱控你的设备。

---

## 七、已记录的上云/OTA 笔记

- 云端服务器全流程 + OTA：见同目录 [OTA.md](OTA.md)
- 这份文档聚焦"网络控制"的实现细节，两者配套看。
