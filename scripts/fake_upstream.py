#!/usr/bin/env python3
"""测试用的假上游：模拟 OpenAI 兼容的 SSE 接口，用于验证 C++ 侧网关逻辑。

环境变量：
  FAKE_FAIL_FIRST=N   前 N 个请求直接返回错误（默认 0），用于验证重试
  FAKE_FAIL_STATUS=S  错误状态码（默认 429）
"""
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


FAIL_FIRST = int(os.getenv("FAKE_FAIL_FIRST", "0"))
FAIL_STATUS = int(os.getenv("FAKE_FAIL_STATUS", "429"))
_requests = 0
_requests_lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[upstream] " + fmt % args + "\n")

    def do_POST(self):
        global _requests
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")

        with _requests_lock:
            index = _requests
            _requests += 1

        if index < FAIL_FIRST:
            payload = json.dumps(
                {"error": {"message": "simulated failure", "type": "rate_limit"}}
            ).encode()
            self.send_response(FAIL_STATUS)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            self.wfile.flush()
            return

        if self.headers.get("Authorization") != "Bearer good-key":
            payload = json.dumps({"error": {"message": "Invalid API key", "type": "auth"}}).encode()
            self.send_response(401)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        pieces = ["上游", "流式", "回复", "：", "OK"]
        for piece in pieces:
            chunk = {
                "id": "up-1",
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": body.get("model", "fake"),
                "choices": [{"index": 0, "delta": {"content": piece}, "finish_reason": None}],
            }
            self.write_chunk(b"data: " + json.dumps(chunk, ensure_ascii=False).encode() + b"\n\n")
            time.sleep(0.05)

        usage = {
            "id": "up-1",
            "object": "chat.completion.chunk",
            "choices": [],
            "usage": {"prompt_tokens": 11, "completion_tokens": 5, "total_tokens": 16},
        }
        self.write_chunk(b"data: " + json.dumps(usage).encode() + b"\n\n")
        self.write_chunk(b"data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def write_chunk(self, data):
        self.wfile.write(("%X\r\n" % len(data)).encode() + data + b"\r\n")
        self.wfile.flush()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9099
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
