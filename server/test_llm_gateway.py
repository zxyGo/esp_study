import io
import json
import os
import unittest
from unittest.mock import patch

from llm_gateway import Config, Gateway


class FakeResponse:
    def __init__(self, body):
        self.body = io.BytesIO(json.dumps(body).encode())

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return None

    def read(self):
        return self.body.read()


class GatewayTest(unittest.TestCase):
    def setUp(self):
        env = {
            "LLM_API_BASE": "http://model.test/v1/",
            "LLM_API_KEY": "secret",
            "LLM_MODEL": "test-model",
            "LLM_HISTORY_TURNS": "1",
        }
        self.env = patch.dict(os.environ, env, clear=True)
        self.env.start()
        self.gateway = Gateway(Config())

    def tearDown(self):
        self.env.stop()

    @patch("urllib.request.urlopen")
    def test_chat_uses_compatible_endpoint_and_auth(self, urlopen):
        urlopen.return_value = FakeResponse(
            {"choices": [{"message": {"content": "  你好！  "}}]}
        )
        self.assertEqual(self.gateway.chat("你好", "device-1"), "你好！")

        request = urlopen.call_args.args[0]
        self.assertEqual(request.full_url, "http://model.test/v1/chat/completions")
        self.assertEqual(request.headers["Authorization"], "Bearer secret")
        payload = json.loads(request.data.decode())
        self.assertEqual(payload["model"], "test-model")
        self.assertEqual(payload["messages"][-1], {"role": "user", "content": "你好"})

    @patch("urllib.request.urlopen")
    def test_history_is_limited_by_turns(self, urlopen):
        urlopen.return_value = FakeResponse(
            {"choices": [{"message": {"content": "回答一"}}]}
        )
        self.gateway.chat("问题一", "device-1")
        urlopen.return_value = FakeResponse(
            {"choices": [{"message": {"content": "回答二"}}]}
        )
        self.gateway.chat("问题二", "device-1")

        payload = json.loads(urlopen.call_args.args[0].data.decode())
        self.assertEqual(
            payload["messages"][-3:],
            [
                {"role": "user", "content": "问题一"},
                {"role": "assistant", "content": "回答一"},
                {"role": "user", "content": "问题二"},
            ],
        )
        self.assertEqual(len(self.gateway._histories["device-1"]), 2)

    @patch("urllib.request.urlopen")
    def test_thinking_is_only_sent_when_configured(self, urlopen):
        urlopen.return_value = FakeResponse(
            {"choices": [{"message": {"content": "简短回答"}}]}
        )
        self.gateway.chat("问题", "device-1")
        payload = json.loads(urlopen.call_args.args[0].data.decode())
        self.assertNotIn("thinking", payload)

        with patch.dict(os.environ, {"LLM_THINKING": "disabled"}):
            gateway = Gateway(Config())
        urlopen.return_value = FakeResponse(
            {"choices": [{"message": {"content": "简短回答"}}]}
        )
        gateway.chat("问题", "device-2")
        payload = json.loads(urlopen.call_args.args[0].data.decode())
        self.assertEqual(payload["thinking"], {"type": "disabled"})


if __name__ == "__main__":
    unittest.main()
