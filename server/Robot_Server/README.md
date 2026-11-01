# Robot Server

Flask 服务为 Smart Voice Terminal 提供网页控制台、设备状态、动作队列、AI 决策、语音识别、相机工作台和 OTA 接口。入口是 `app.py`。

## 本地运行

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt
Copy-Item config.example.json config.json
# 编辑 config.json：设置随机 token 和自己的 AI Provider
.venv\Scripts\python app.py
```

浏览器访问 `http://127.0.0.1:5000/`。局域网访问时使用运行服务电脑的私网地址。`config.json`、数据库、录音、照片和模型目录均为本地运行数据，禁止提交。

## 主要接口

- `POST /api/data`：ESP 上报传感器或用户文本并取得决策。
- `GET/POST /api/device/pose`：读取和更新设备姿态。
- `/api/admin/*`：动作时间轴、舵机参数、待机策略和相机管理。
- `/api/audio/*`：录音流和识别队列。
- `/api/ota/*`、`/api/f407/*`：设备升级和状态查询。

离线测试脚本以 `test_*.py` 命名，可在配置完成后按 `python test_motion_timeline.py` 等方式运行。需要模型的测试会在缺少可选依赖时明确跳过。
