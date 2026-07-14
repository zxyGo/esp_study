"""局域网内的 LLM 与离线 TTS 网关。

ESP32 只需要访问本服务；云端 API Key 由网关从环境变量读取，不会写入固件。
上游采用 OpenAI Chat Completions 兼容协议，因此也可以连接本机 Ollama。
语音合成使用本机 sherpa-onnx，不会向云端发送回答文本。
"""

from __future__ import annotations

import json
import logging
import os
import threading
import urllib.error
import urllib.request
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any


LOG = logging.getLogger("llm-gateway")


class Config:
    def __init__(self) -> None:
        self.api_base = os.getenv("LLM_API_BASE", "http://host.docker.internal:11434/v1").rstrip("/")
        self.api_key = os.getenv("LLM_API_KEY", "")
        self.model = os.getenv("LLM_MODEL", "qwen2.5:3b")
        self.system_prompt = os.getenv(
            "LLM_SYSTEM_PROMPT",
            "你是运行在 ESP32 语音助手中的中文助理。回答自然、准确、简短，默认不超过80个汉字；只输出纯文本，不使用Markdown或表情。",
        )
        self.max_tokens = int(os.getenv("LLM_MAX_TOKENS", "160"))
        self.temperature = float(os.getenv("LLM_TEMPERATURE", "0.7"))
        self.thinking = os.getenv("LLM_THINKING", "").strip().lower()
        if self.thinking not in ("", "enabled", "disabled"):
            raise ValueError("LLM_THINKING 只能是 enabled、disabled 或留空")
        self.history_turns = max(0, int(os.getenv("LLM_HISTORY_TURNS", "4")))
        self.request_timeout = float(os.getenv("LLM_REQUEST_TIMEOUT", "60"))
        self.tts_model_dir = os.getenv("TTS_MODEL_DIR", "/models/vits-melo-tts-zh_en")
        self.tts_speed = float(os.getenv("TTS_SPEED", "1.0"))
        self.tts_num_threads = max(1, int(os.getenv("TTS_NUM_THREADS", "2")))
        self.tts_max_audio_bytes = int(os.getenv("TTS_MAX_AUDIO_BYTES", str(4 * 1024 * 1024)))
        self.tts_target_peak = float(os.getenv("TTS_TARGET_PEAK", "0.25"))
        if not 0.0 < self.tts_target_peak <= 1.0:
            raise ValueError("TTS_TARGET_PEAK 必须大于 0 且不超过 1")
        self.port = int(os.getenv("PORT", "10096"))


class LocalTts:
    """按需加载 sherpa-onnx，避免未使用 TTS 时占用模型内存。"""

    def __init__(self, config: Config) -> None:
        try:
            import sherpa_onnx
        except ImportError as exc:
            raise RuntimeError("缺少 sherpa-onnx，无法使用离线语音合成") from exc

        model_dir = config.tts_model_dir
        required = {
            "model": os.path.join(model_dir, "model.onnx"),
            "lexicon": os.path.join(model_dir, "lexicon.txt"),
            "tokens": os.path.join(model_dir, "tokens.txt"),
        }
        missing = [path for path in required.values() if not os.path.isfile(path)]
        if missing:
            raise RuntimeError(f"离线 TTS 模型文件不存在: {missing[0]}")

        rule_fsts = [
            os.path.join(model_dir, name)
            for name in ("phone.fst", "date.fst", "number.fst")
            if os.path.isfile(os.path.join(model_dir, name))
        ]
        tts_config = sherpa_onnx.OfflineTtsConfig(
            model=sherpa_onnx.OfflineTtsModelConfig(
                vits=sherpa_onnx.OfflineTtsVitsModelConfig(**required),
                provider="cpu",
                num_threads=config.tts_num_threads,
            ),
            rule_fsts=",".join(rule_fsts),
            max_num_sentences=1,
        )
        if not tts_config.validate():
            raise RuntimeError("离线 TTS 模型配置无效")
        self._sherpa_onnx = sherpa_onnx
        self._tts = sherpa_onnx.OfflineTts(tts_config)
        self._speed = config.tts_speed
        self._target_peak = config.tts_target_peak
        LOG.info("离线 TTS 模型已加载: %s", model_dir)

    def synthesize(self, text: str) -> tuple[bytes, int]:
        import numpy as np

        generation = self._sherpa_onnx.GenerationConfig()
        generation.speed = self._speed
        generation.silence_scale = 0.2
        audio = self._tts.generate(text, generation)
        if len(audio.samples) == 0 or audio.sample_rate <= 0:
            raise RuntimeError("离线 TTS 生成了空音频")

        # MeloTTS 的原始电平偏低。按每段语音的实际峰值自动增益，避免小功放听起来近乎静音；
        # target_peak 小于 1，保留余量并防止转换 PCM16 时削波。
        samples = np.asarray(audio.samples, dtype=np.float32)
        original_peak = float(np.max(np.abs(samples)))
        if original_peak > 0.0:
            gain = self._target_peak / original_peak
            samples = samples * gain
            LOG.info(
                "TTS 音量归一化: 原始峰值 %.1f%%，增益 %.1fx，目标峰值 %.1f%%",
                original_peak * 100.0,
                gain,
                self._target_peak * 100.0,
            )
        samples = np.clip(samples, -1.0, 1.0)
        pcm = (samples * 32767.0).astype("<i2").tobytes()
        return pcm, int(audio.sample_rate)


class Gateway:
    def __init__(self, config: Config, tts_engine: Any | None = None) -> None:
        self.config = config
        self._histories: dict[str, list[dict[str, str]]] = defaultdict(list)
        self._lock = threading.Lock()
        self._tts_engine = tts_engine
        self._tts_lock = threading.Lock()

    def chat(self, text: str, session_id: str) -> str:
        with self._lock:
            history = list(self._histories[session_id])

        messages = [{"role": "system", "content": self.config.system_prompt}]
        messages.extend(history)
        messages.append({"role": "user", "content": text})
        payload = {
            "model": self.config.model,
            "messages": messages,
            "temperature": self.config.temperature,
            "max_tokens": self.config.max_tokens,
            "stream": False,
        }
        # thinking 不是通用 OpenAI 参数，仅在显式配置时发送，避免影响 Ollama 等服务。
        if self.config.thinking:
            payload["thinking"] = {"type": self.config.thinking}
        request = urllib.request.Request(
            f"{self.config.api_base}/chat/completions",
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        if self.config.api_key:
            request.add_header("Authorization", f"Bearer {self.config.api_key}")

        try:
            with urllib.request.urlopen(request, timeout=self.config.request_timeout) as response:
                result = json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            detail = exc.read(512).decode("utf-8", errors="replace")
            raise RuntimeError(f"上游返回 HTTP {exc.code}: {detail}") from exc
        except (urllib.error.URLError, TimeoutError) as exc:
            raise RuntimeError(f"无法连接大模型服务: {exc.reason if hasattr(exc, 'reason') else exc}") from exc
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise RuntimeError("上游返回了无效 JSON") from exc

        try:
            answer = result["choices"][0]["message"]["content"].strip()
        except (KeyError, IndexError, TypeError, AttributeError) as exc:
            raise RuntimeError("上游响应中没有回答内容") from exc
        if not answer:
            raise RuntimeError("大模型返回了空回答")

        if self.config.history_turns:
            with self._lock:
                items = self._histories[session_id]
                items.extend(
                    [
                        {"role": "user", "content": text},
                        {"role": "assistant", "content": answer},
                    ]
                )
                del items[: max(0, len(items) - self.config.history_turns * 2)]
        return answer

    def speech(self, text: str) -> tuple[bytes, int]:
        """离线生成 PCM16 单声道音频及其采样率。"""
        with self._tts_lock:
            if self._tts_engine is None:
                self._tts_engine = LocalTts(self.config)
            audio, sample_rate = self._tts_engine.synthesize(text)
        if len(audio) > self.config.tts_max_audio_bytes:
            raise RuntimeError("语音合成结果过大")
        if not audio or len(audio) % 2 or sample_rate <= 0:
            raise RuntimeError("语音合成服务返回了无效的 PCM16 音频")
        return audio, sample_rate


class Handler(BaseHTTPRequestHandler):
    gateway: Gateway
    server_version = "FunASR-LLM-Gateway/1.0"

    def _send_json(self, status: int, body: dict[str, Any]) -> None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path == "/health":
            self._send_json(
                200,
                {
                    "status": "ok",
                    "model": self.gateway.config.model,
                    "tts_model": os.path.basename(self.gateway.config.tts_model_dir),
                    "tts_backend": "sherpa-onnx",
                },
            )
        else:
            self._send_json(404, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path not in ("/chat", "/tts"):
            self._send_json(404, {"error": "not found"})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 8192:
                raise ValueError("请求体大小无效")
            body = json.loads(self.rfile.read(length).decode("utf-8"))
            text = body.get("text", "")
            session_id = body.get("session_id", "esp32s3")
            if not isinstance(text, str) or not text.strip():
                raise ValueError("text 不能为空")
            if not isinstance(session_id, str) or not session_id.strip() or len(session_id) > 64:
                raise ValueError("session_id 无效")
            text = text.strip()[:1000]
            if self.path == "/tts":
                audio, sample_rate = self.gateway.speech(text)
                self.send_response(200)
                self.send_header("Content-Type", "audio/pcm")
                self.send_header("Content-Length", str(len(audio)))
                self.send_header("X-Audio-Sample-Rate", str(sample_rate))
                self.send_header("X-Audio-Channels", "1")
                self.send_header("X-Audio-Bits-Per-Sample", "16")
                self.end_headers()
                self.wfile.write(audio)
            else:
                answer = self.gateway.chat(text, session_id.strip())
                self._send_json(200, {"answer": answer})
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            self._send_json(400, {"error": str(exc)})
        except RuntimeError as exc:
            LOG.warning("LLM 请求失败: %s", exc)
            self._send_json(502, {"error": str(exc)})
        except Exception:
            LOG.exception("处理请求时发生未预期错误")
            self._send_json(500, {"error": "网关内部错误"})

    def log_message(self, fmt: str, *args: Any) -> None:
        LOG.info("%s - %s", self.address_string(), fmt % args)


def main() -> None:
    logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"), format="%(asctime)s %(levelname)s %(message)s")
    config = Config()
    Handler.gateway = Gateway(config)
    server = ThreadingHTTPServer(("0.0.0.0", config.port), Handler)
    LOG.info(
        "LLM/TTS 网关监听 0.0.0.0:%d，上游模型: %s，离线语音模型: %s",
        config.port,
        config.model,
        config.tts_model_dir,
    )
    server.serve_forever()


if __name__ == "__main__":
    main()
