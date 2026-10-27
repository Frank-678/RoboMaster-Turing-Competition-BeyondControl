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

counts = Counter()
lock = threading.Lock()

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        if self.headers.get("Authorization") != "Bearer fixture-token":
            raise AssertionError("missing test bearer token")
        if self.headers.get("Transfer-Encoding"):
            raise AssertionError("unexpected chunked request")
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert request["stream"] is False and request["max_tokens"] > 0
        assert request["model"] == "fixture-model"
        context = json.loads(request["messages"][1]["content"])
        with lock:
            counts[self.path] += 1
            attempt = counts[self.path]
        status = 200
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
            events = []
            if context["announcements"]:
                events = [{
                    "kind": "fortress", "start": 0, "end": 1000,
                    "area": [6, 6, 7, 7], "capture_ticks": 2,
                    "amount": 50, "defended_damage": 10, "source": "fixture",
                }]
            answer = {"session_id": context["session_id"], "events": events, "unknown": []}
            body = {"choices": [{"message": {"content": json.dumps(answer)}}]}
        raw = json.dumps(body).encode() if body is not None else b"temporary upstream failure"
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json" if body is not None else "text/plain")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        except (BrokenPipeError, ConnectionResetError):
            pass

def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = os.environ.copy()
    env["RMTC_TEST_GATEWAY"] = f"http://127.0.0.1:{server.server_port}"
    try:
        subprocess.run([str(Path(sys.argv[1]).resolve())], env=env, check=True, timeout=30)
        with lock:
            assert counts["/ok"] == 1, ("announcement deduplication failed", counts)
            assert counts["/quota"] == 1, ("quota retried", counts)
            assert counts["/stage1-must-not-call"] == 0, counts
            assert counts["/rate"] == 2, counts
            assert counts["/auth"] == 1 and counts["/garbage"] == 1, counts
            assert counts["/bad-response"] == 1, counts
        print("mock gateway request-count and authorization assertions passed")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)

if __name__ == "__main__":
    main()
