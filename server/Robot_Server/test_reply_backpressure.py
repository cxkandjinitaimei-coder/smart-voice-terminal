"""隔离真实Flask语音入口与并发入队；不调用AI，不写用户日志或数据库。"""
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import patch
import app as A

DEV = "reply-backpressure-test"

def main():
    with patch.object(A, "load_config", return_value={"token": "test-only"}), \
         patch.object(A, "append_line"), \
         patch.object(A, "start_auto_tick"), patch.object(A, "start_store"), \
         patch.object(A, "start_beacon"), patch.object(A.asr, "warmup"), \
         patch.object(A.ai_relay, "get_ai_decision") as ai, \
         patch.object(A.store, "record_interaction"), patch.object(A.store, "record_sensor"), \
         patch.object(A.store, "record_command"):
        # 锁内上限：60个生产者同时入队，仍只接收3条，不丢掉已排队动作。
        with ThreadPoolExecutor(max_workers=20) as pool:
            results=list(pool.map(lambda i: A.enqueue_reply(DEV,{"raw":"@TEXT:reply%d"%i}),range(60)))
        assert sum(x is not None for x in results)==A.DELIVER_MAX_QUEUE
        assert A._queue_len(DEV)==A.DELIVER_MAX_QUEUE
        before=list(A._pending_commands[DEV])
        client=A.app.test_client()
        response=client.post("/api/data",json={"token":"test-only","device_id":DEV,
            "user":"误触发识别文字","enqueue_decision":True,"need_ai":True})
        assert response.status_code==429,response.get_json()
        assert response.get_json()["deliver_state"]=="refused_queue_full"
        assert response.get_json()["queued_to_device"] is False
        ai.assert_not_called()
        assert list(A._pending_commands[DEV])==before
        # 设备消费后可继续投递，保留原有FIFO次序，不以新回复覆盖旧动作。
        assert A.pop_command(DEV)==before[0]
        assert A.enqueue_reply(DEV,{"raw":"@TEXT:retry"})==A.DELIVER_MAX_QUEUE
        assert A.pop_command(DEV)==before[1]
        assert A.pop_command(DEV)==before[2]
        assert A.pop_command(DEV)["raw"]=="@TEXT:retry"
        assert A.pop_command(DEV) is None
        assert A._delivery_view()[-1]["state"]=="refused_queue_full"
        # 模拟AI处理结束时队列被其他请求填满；第二道检查不能误报queued。
        def raced(*args, **kwargs):
            for i in range(A.DELIVER_MAX_QUEUE):
                A.enqueue_command(DEV,{"raw":"@TEXT:racing%d"%i})
            return "@TEXT:must not overflow"
        with patch.object(A,"decision_to_raw",side_effect=raced):
            response=client.post("/api/data",json={"token":"test-only","device_id":DEV,
                "user":"模拟语音","enqueue_decision":True,"mock":True})
            assert response.status_code==429,response.get_json()
            assert A._queue_len(DEV)==A.DELIVER_MAX_QUEUE
            assert A._delivery_view()[-1]["state"]=="refused_queue_full"
            while A.pop_command(DEV):pass
        # 新遥测为接收确认，不冒充屏幕已经显示；浏览器不能伪造板端遥测。
        with client.session_transaction() as session:session["authed"]=True
        report={"phase":"received","sequence":5,"attempts":2}
        response=client.post("/api/data",json={"token":"test-only","device_id":DEV,
            "text_transport":report})
        assert response.status_code==200,response.get_json()
        view=client.get("/api/deliveries?device_id="+DEV).get_json()
        assert view["text_transport"]["phase"]=="received"
        assert view["text_transport"]["attempts"]==2
        client.post("/api/data",json={"device_id":DEV,"no_pop":True,
            "text_transport":{"phase":"fake"}})
        assert A._motion_status[DEV]["text_transport"]["phase"]=="received"
    print("PASS: 60 concurrent replies limited, audio route 429 before AI, FIFO/race guard/text telemetry")

if __name__=="__main__":main()
