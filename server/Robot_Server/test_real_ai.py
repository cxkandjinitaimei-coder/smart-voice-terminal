# -*- coding: utf-8 -*-
"""真实AI调用测试：走 HTTP 打到本机运行中的中转站，验证 DeepSeek 调用成功。"""
import json
import time
import requests

url = "http://127.0.0.1:5000/api/data"
payload = {
    "token": "YOUR_API_TOKEN",
    "sensor": "温度:28.5,湿度:65",
    "user": "我热了",
}

print("== 测试开始，调用真实AI（非mock）==", time.strftime("%H:%M:%S"))
t0 = time.time()
try:
    r = requests.post(url, json=payload, timeout=90)
    print("HTTP状态码:", r.status_code)
    print("耗时 %.1f 秒" % (time.time() - t0))
    print("响应内容:")
    try:
        data = r.json()
        print(json.dumps(data, ensure_ascii=False, indent=2))
    except Exception:
        print(r.text[:2000])
except Exception as e:
    print("请求失败:", repr(e))

print("\n== 测试结束 ==")
