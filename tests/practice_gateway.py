#!/usr/bin/env python3
"""只加载原版practice的HTTP处理器；ROS导入用占位，不启动裁判，也不调用真实模型。"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import ModuleType, SimpleNamespace


def original_handler(source):
    # source是原版practice.py路径。只读文件并禁止生成pyc；不替换其HTTP实现。
    raw = source.read_bytes()  # 测试前核对Git对象，保证不是我们重写的转发器。
    digest = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
    if digest != "abd400db06ade29c548e4a7b34dfc0bc1acc013f":
        raise AssertionError("practice.py changed: recheck the original release before testing")
    sys.dont_write_bytecode = True
    # 原文件顶层会导入ROS；这项测试只运行HTTP，不构造Node或执行任何ROS方法。
    imports = {"rclpy": {}, "rclpy.executors": {"MultiThreadedExecutor": object},
               "rclpy.node": {"Node": object}, "rclpy.qos": {"QoSProfile": object, "DurabilityPolicy": object},
               "std_msgs": {}, "std_msgs.msg": {"String": object}, "internal_ros": {"internal_context": object}}
    for name, members in imports.items():  # name模块名，members只供原模块导入的符号。
        module = ModuleType(name)
        module.__dict__.update(members)
        sys.modules[name] = module
    spec = importlib.util.spec_from_file_location("original_practice_http", source)
    module = importlib.util.module_from_spec(spec)  # 原模块；只导入，不运行其main。
    spec.loader.exec_module(module)
    return module.Handler, module.requests


class ModelProxy(BaseHTTPRequestHandler):
    # 固定模型HTTP夹具，同时扮演显式出网代理；域名.invalid不能被直接连接。
    models = []  # 按顺序记录实际收到的模型名，验证请求优先级。

    def log_message(self, *_):
        pass  # 不输出请求头、正文或测试密钥。

    def do_POST(self):
        assert self.path == "http://fixture-model.invalid/v1/chat/completions", self.path
        assert self.headers.get("Authorization") == "Bearer fixture-upstream-key"  # practice应替换席位令牌。
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))  # 实际转发请求，非手工组装的副本。
        self.models.append(body["model"])
        events = []  # 手工构造的模型输出；不代表语义理解。
        if body["model"] == "fixture-model":
            assert body["thinking"] == {"type": "disabled"}
            assert body["response_format"] == {"type": "json_object"}
            context = json.loads(body["messages"][1]["content"])  # C++当前场次和原公告。
            assert context["session_id"] == "test-match" and "fixture" in context["announcements"]
            events = [{"kind": "fortress", "start": 0, "end": 1000, "area": [6, 6, 7, 7],
                       "capture_ticks": 2, "amount": 50, "defended_damage": 10, "source": "fixture"}]
        else:
            assert body["model"] == "fallback-model"  # 只在请求缺model时使用PRACTICE_MODEL。
        answer = {"session_id": "test-match", "events": events, "unknown": []}
        raw = json.dumps({"choices": [{"finish_reason": "stop", "message": {
            "content": "```json\n" + json.dumps(answer) + "\n```"}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


def main():
    # argv[1]是正式核心测试二进制，argv[2]是只读原件；所有端口、日志均用临时资源。
    handler, requests = original_handler(Path(sys.argv[2]).resolve())
    upstream = ThreadingHTTPServer(("127.0.0.1", 0), ModelProxy)
    gateway = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    with tempfile.TemporaryDirectory() as data:
        gateway.app = SimpleNamespace(args=SimpleNamespace(ai_mode="real"), lock=threading.RLock(),
            session="test-match", stopped=False, ai_slots={s: threading.BoundedSemaphore(1) for s in "ABCD"},
            ai={}, run=Path(data))  # 只提供原HTTP处理器所需状态；不模拟裁判结算。
        for key in ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
            os.environ[key] = f"http://127.0.0.1:{upstream.server_port}"  # 明确由本机夹具接收出网转发。
        os.environ["NO_PROXY"] = os.environ["no_proxy"] = "localhost,127.0.0.1,::1"
        os.environ.update(PRACTICE_LLM_URL="http://fixture-model.invalid/v1/chat/completions",
                          PRACTICE_LLM_KEY="fixture-upstream-key", PRACTICE_MODEL="fallback-model")
        threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in (upstream, gateway)]
        for thread in threads:
            thread.start()
        try:
            base = f"http://127.0.0.1:{gateway.server_port}/v1/chat/completions"  # C++只能向这个网关请求。
            env = dict(os.environ, RMTC_TEST_PRACTICE_GATEWAY=base)
            env.pop("RMTC_TEST_GATEWAY", None)  # 这次运行只测原版代理，不重复跑其他固定网关场景。
            subprocess.run([str(Path(sys.argv[1]).resolve())], env=env, check=True, timeout=15)
            # 模拟陪练的本机请求：沿用带代理环境，必须因NO_PROXY直连本机gateway。
            response = requests.post(base, json={"messages": [{"role": "user", "content": "fixture"}], "stream": False},
                headers={"Authorization": "Bearer practice-A"}, timeout=3)
            assert response.status_code == 200
            assert ModelProxy.models == ["fixture-model", "fallback-model"], ModelProxy.models
            records = [json.loads(line) for line in (Path(data) / "ai.jsonl").read_text().splitlines()]
            assert len(records) == 2 and records[-1]["seat"] == "A" and records[-1]["success_count"] == 2
            print("original practice HTTP relay passed: model priority, token replacement, proxy routing, A audit; canned upstream only")
        finally:
            for server in (gateway, upstream):
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join(timeout=2)


if __name__ == "__main__":
    main()
