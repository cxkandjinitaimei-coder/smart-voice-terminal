"""验证停用语音请求；Flask 测试客户端，无网络或用户数据写入。

运行：python -B test_text_only.py。即使旧配置仍写 tts_enabled=true，
当前服务也只返回 410；保留认证边界及文字/动作链路。
"""
from unittest.mock import patch
import app as A


def main():
    with patch.object(A, 'load_config', return_value={
            'token': 'test-only', 'tts_enabled': True}), \
         patch.object(A, 'start_auto_tick'), patch.object(A, 'start_store'), \
         patch.object(A, 'start_beacon'), patch.object(A.asr, 'warmup'), \
         patch('requests.sessions.Session.request') as outbound:
        client = A.app.test_client()
        assert client.post('/api/tts/speech', json={'text': '你好'}).status_code == 401
        for payload in ({'text': '你好'}, {}, {'text': '很长的句子' * 1000}):
            response = client.post('/api/tts/speech?token=test-only', json=payload)
            assert response.status_code == 410
            assert response.get_json()['ok'] is False
            assert response.mimetype == 'application/json'
        outbound.assert_not_called()
        assert not hasattr(A.ai_relay, 'get_tts_audio')
    print('PASS: speech disabled even with legacy config; authenticated 410, no synthesis/network')


if __name__ == '__main__':
    main()
