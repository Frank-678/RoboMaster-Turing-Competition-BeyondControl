#!/usr/bin/env python3
"""检查诊断能区分HTTP、代理、TLS和读取失败；只访问本机HTTP夹具，不使用真实密钥。"""
import importlib.util
import json
import os
from pathlib import Path
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

import requests
from urllib3.exceptions import ReadTimeoutError

source = Path(__file__).resolve().parents[1] / "scripts/check_ai.py"  # 实际诊断脚本，不复制它的实现。
spec = importlib.util.spec_from_file_location("check_ai", source)
check_ai = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check_ai)
KEY = "fixture-not-a-real-key"  # 只给本机夹具；断言所有诊断均不包含它。


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert self.headers["Authorization"] == "Bearer " + KEY
        assert self.headers["Accept-Encoding"] == "identity"
        assert body["model"] == "fixture-model" and body["stream"] is False
        assert body["thinking"] == {"type": "disabled"}
        assert body["response_format"] == {"type": "json_object"} and body["max_tokens"] == 2200
        data = {"model": "fixture-model", "choices": [{"finish_reason": "stop", "message": {"content": '{"ok":true}'}}]}
        code = {"/http401": 401, "/http502": 502, "/redirect": 302}.get(self.path, 200)
        if code != 200:
            data = {"error": {"message": KEY}}  # 上游甚至回显凭据，诊断也不得输出正文。
        elif self.path == "/empty":
            data["choices"][0]["message"] = {"content": None, "reasoning_content": KEY}
        elif self.path == "/truncated":
            data["choices"][0]["finish_reason"] = "length"
        raw = ("<html>" + KEY).encode() if self.path == "/html" else json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        if code == 302:
            self.send_header("Location", "http://upstream.invalid/must-not-follow")
        self.end_headers()
        self.wfile.flush()
        if self.path == "/slow-body":
            time.sleep(0.2)  # 头已到、正文迟到，requests会把超时包装为ConnectionError。
        try:
            self.wfile.write(raw)
        except (BrokenPipeError, ConnectionResetError):
            pass  # 超时夹具的客户端已主动断开。


def main():
    proxy_names = ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY")
    # 本测试的上游就是127.0.0.1，不继承宿主出网代理。
    with patch.dict(os.environ, {**dict.fromkeys(proxy_names, ""), "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}):
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            cases = {"/ok": ("request_ok", 200), "/http401": ("http_error", 401), "/http502": ("http_error", 502),
                     "/redirect": ("redirect", 302), "/html": ("invalid_completion", 200),
                     "/empty": ("empty_content", 200), "/truncated": ("output_truncated", 200)}
            for path, expected in cases.items():
                result = check_ai.probe(base + path, KEY, "fixture-model", "disabled", True, 2200)
                assert (result["kind"], result["http_status"]) == expected, result
                assert KEY not in json.dumps(result)
            original_post = requests.post  # 仅将本机夹具的读取等待缩短到50ms，不改生产脚本。
            def short_post(*args, **kwargs):
                kwargs["timeout"] = (0.2, 0.05)
                return original_post(*args, **kwargs)
            with patch.object(check_ai.requests, "post", side_effect=short_post):
                result = check_ai.probe(base + "/slow-body", KEY, "fixture-model", "disabled", True, 2200)
                assert result["kind"] == "read_timeout", result
        finally:
            server.shutdown()
            server.server_close()
            worker.join()
    # TLS/代理异常用明确异常类型注入，避免为了测试创建证书或访问真实外网。
    for exception, expected in [(requests.exceptions.ProxyError(KEY), "proxy_error"),
        (requests.exceptions.SSLError(KEY), "tls_error"), (requests.exceptions.ConnectTimeout(KEY), "connect_timeout"),
        (requests.exceptions.ReadTimeout(KEY), "read_timeout"), (requests.exceptions.ConnectionError(KEY), "connection_error"),
        (requests.exceptions.ConnectionError(ReadTimeoutError(None, None, KEY)), "read_timeout")]:
        with patch.object(check_ai.requests, "post", side_effect=exception):
            result = check_ai.probe("http://unused.invalid", KEY, "fixture-model", "disabled", True, 2200)
            assert result["kind"] == expected and KEY not in json.dumps(result), result
    print("AI check: 8 local HTTP cases and 6 injected network failures passed; no live model called")


if __name__ == "__main__":
    main()
