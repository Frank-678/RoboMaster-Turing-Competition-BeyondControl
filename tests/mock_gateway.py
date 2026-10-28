#!/usr/bin/env python3
"""Local protocol fixture only: canned model replies, never a real LLM/official qualification."""
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

counts = Counter()  # 每个请求路径的次数，用于检查去重、退避和阶段隔离。
lock = threading.Lock()  # HTTP处理线程与主测试线程共用的计数锁。
cumulative_lengths = []  # 累积公告测试实际发到HTTP正文的行数。

class Handler(BaseHTTPRequestHandler):
    # 本机假网关：按URL路径选择固定成功/错误回复，不访问真实模型。
    def log_message(self, *_):
        # 关闭HTTP访问日志，避免夹具输出淹没回归结果。
        pass

    def do_POST(self):
        # self是本次HTTP处理器；先检查客户端实际发出的头、长度和model。
        if self.headers.get("Authorization") != "Bearer fixture-token":
            raise AssertionError("missing test bearer token")
        if self.headers.get("Transfer-Encoding"):
            raise AssertionError("unexpected chunked request")
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))  # 客户端完整请求体。
        assert request["stream"] is False and request["max_tokens"] > 0
        assert request["model"] == ("deepseek-flash" if self.path == "/formal-options" else "fixture-model")
        if self.path == "/formal-options":
            assert request["max_tokens"] == 512 and "extra_body" not in request
            assert "JSON" in request["messages"][0]["content"]
        if self.path == "/standard-only":
            assert "thinking" not in request and "response_format" not in request
        else:
            assert request["thinking"] == {"type": "disabled"}
            assert request["response_format"] == {"type": "json_object"}
        context = json.loads(request["messages"][1]["content"])  # 用户消息里的场次、公告和已知事件。
        if self.path == "/cumulative":
            lines = context["announcements"].splitlines()  # 夹具每条公告占一行，检测实际请求是否重复。
            assert len(lines) == len(set(lines)), "cumulative announcement text duplicated on the wire"
            cumulative_lengths.append(len(lines))
        with lock:
            counts[self.path] += 1
            attempt = counts[self.path]  # 该路径第几次请求，决定限流/部分结果的下一次回复。
        status = 200  # HTTP结果；body在各分支构造，None表示故意返回非JSON错误。
        if self.path == "/quota":
            status, body = 429, {"error": {"code": "quota_exceeded"}}
        elif self.path == "/rate" and attempt == 1:
            status, body = 429, {"error": {"code": "rate_limit"}}
        elif self.path == "/auth":
            status, body = 401, {"error": {"code": "invalid_token"}}
        elif self.path == "/garbage":
            status, body = 503, None
        elif self.path == "/bad-response":
            body = {"choices": []}
        else:
            if self.path == "/slow":
                time.sleep(0.35)
            elif self.path == "/delayed":
                time.sleep(0.15)
            events = []  # 固定模型事件列表，与真实语义推理无关。
            if context["announcements"]:
                events = [{
                    "kind": "fortress", "start": 0, "end": 1000,
                    "area": [6, 6, 7, 7], "capture_ticks": 2,
                    "amount": 50, "defended_damage": 10, "source": "fixture",
                }]
            unknown = ["fixture uncertainty"] if self.path == "/partial" and attempt == 1 else []  # 第一次只解释一部分。
            if self.path == "/partial" and attempt > 1:
                assert context["known_events"], "validated partial events were discarded"
                events = []  # 固定模型事件列表，与真实语义推理无关。
            answer = {"session_id": context["session_id"], "events": events, "unknown": unknown}  # 内层模型JSON。
            body = {"choices": [{"message": {"content": json.dumps(answer)}}]}
            if self.path == "/fenced":
                body["choices"][0]["message"]["content"] = "```json\n" + json.dumps(answer) + "\n```"
            elif self.path == "/reasoning-only":
                body["choices"][0]["message"] = {"content": None, "reasoning_content": "fixture reasoning, never an answer"}
            elif self.path == "/truncated":
                body["choices"][0]["finish_reason"] = "length"
            if self.path == "/practice-mock":
                body["model"] = "practice-mock"
        raw = json.dumps(body).encode() if body is not None else b"temporary upstream failure"  # 真正写入HTTP的字节。
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json" if body is not None else "text/plain")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        except (BrokenPipeError, ConnectionResetError):
            pass

def main():
    # 启动夹具→运行sys.argv[1]指定的测试可执行文件→核对调用次数→清理线程/端口。
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)  # 操作系统分配临时端口，避免占用正式8080。
    thread = threading.Thread(target=server.serve_forever, daemon=True)  # 独立HTTP服务线程。
    thread.start()
    env = os.environ.copy()  # 仅传给被测进程的环境副本，不改用户终端。
    # 故意给坏的终端代理；客户端必须按自身配置直连夹具。
    for key in ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
        env[key] = "http://127.0.0.1:1"
    env["NO_PROXY"] = env["no_proxy"] = ""
    env["RMTC_TEST_GATEWAY"] = f"http://127.0.0.1:{server.server_port}"
    try:
        subprocess.run([str(Path(sys.argv[1]).resolve())], env=env, check=True, timeout=30)
        with lock:
            assert counts["/ok"] == 1, ("announcement deduplication failed", counts)
            assert counts["/cumulative"] == 2 and cumulative_lengths == [1, 100], cumulative_lengths
            assert counts["/partial"] == 2, ("partial interpretation was not retried exactly once", counts)
            assert counts["/quota"] == 1, ("quota retried", counts)
            assert counts["/stage1-must-not-call"] == 0, counts
            assert counts["/rate"] == 2, counts
            assert counts["/auth"] == 1 and counts["/garbage"] == 1, counts
            assert counts["/bad-response"] == 1, counts
            assert counts["/practice-mock"] == 1, counts
            assert all(counts[p] == 1 for p in ("/fenced", "/reasoning-only", "/truncated", "/standard-only", "/formal-options")), counts
            assert counts["http://upstream.invalid/proxy-target"] == 1, counts
        print("mock gateway request-count and authorization assertions passed")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)

if __name__ == "__main__":
    main()

