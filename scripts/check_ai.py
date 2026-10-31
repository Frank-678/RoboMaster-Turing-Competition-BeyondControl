#!/usr/bin/env python3
"""赛前一次最小模型请求。配置由C++启动器传入，不手输环境变量，也不输出密钥或响应正文。"""
import json
import os
import time
from urllib.parse import urlsplit

import requests
from urllib3.exceptions import ReadTimeoutError


def probe(url, key, model, thinking, json_output, max_tokens):
    # 参数全部来自同一match_config.hpp。requests继承启动器明确设置的出网代理，和原practice一致。
    payload = {"model": model, "stream": False, "max_tokens": max_tokens,
               "messages": [{"role": "system", "content":
                             "You are a machine-readable probe. Output exactly one JSON object: no prose, no markdown fences, no commentary."},
                            {"role": "user", "content": 'Return this JSON object: {"ok":true}'}]}
    if thinking:
        payload["thinking"] = {"type": thinking}
    if json_output:
        payload["response_format"] = {"type": "json_object"}
    started = time.monotonic()  # 只计真实耗时，不使用游戏Tick。
    result = {"kind": "request_error", "http_status": 0, "elapsed_ms": 0}
    try:
        # 与原版practice相同的5秒连接/25秒读取、禁跳转、身份头和压缩设置。
        # 读取超时是等待数据的空闲上限，不等于请求总耗时；不自动重试或改走另一代理。
        with requests.post(url, json=payload, headers={"Authorization": "Bearer " + key,
                           "Accept-Encoding": "identity"}, timeout=(5, 25), allow_redirects=False) as response:
            result["http_status"] = response.status_code
            if 300 <= response.status_code < 400:
                result["kind"] = "redirect"
            elif response.status_code != 200:
                result["kind"] = "http_error"
            else:
                body = json.loads(response.text)  # 只在本地判断，不把正文或服务端错误里的凭据打印出来。
                choice = body["choices"][0]
                content = choice["message"].get("content")
                if body.get("model") == "practice-mock":
                    result["kind"] = "mock_response"
                elif choice.get("finish_reason") == "length":
                    result["kind"] = "output_truncated"
                elif not isinstance(content, str) or not content.strip():
                    result["kind"] = "empty_content"
                else:
                    result["kind"] = "request_ok" if json.loads(content) == {"ok": True} else "unexpected_probe_output"
    except requests.exceptions.ProxyError:
        result["kind"] = "proxy_error"
    except requests.exceptions.SSLError:
        result["kind"] = "tls_error"
    except requests.exceptions.ConnectTimeout:
        result["kind"] = "connect_timeout"
    except requests.exceptions.ReadTimeout:
        result["kind"] = "read_timeout"
    except requests.exceptions.ConnectionError as error:
        # requests在响应头到达后，可能把读正文超时包装成ConnectionError(ReadTimeoutError)。
        result["kind"] = "read_timeout" if error.args and isinstance(error.args[0], ReadTimeoutError) else "connection_error"
        # 其余连接错误可能是DNS、拒绝连接、重置；不靠异常文本猜原因，也不输出含凭据的文本。
    except requests.exceptions.RequestException:
        result["kind"] = "request_error"
    except (ValueError, KeyError, IndexError, TypeError):
        result["kind"] = "invalid_completion"  # 包括非JSON响应、错误外壳或模型未按最小JSON回答。
    result["elapsed_ms"] = round((time.monotonic() - started) * 1000)
    return result


def main():
    # 这些变量是C++启动器的内部交接；用户只运行bash check_ai.sh，不直接填环境变量。
    config = os.environ
    required = ("PRACTICE_LLM_URL", "PRACTICE_LLM_KEY", "PRACTICE_MODEL",
                "RMTC_CHECK_THINKING", "RMTC_CHECK_JSON_OUTPUT", "RMTC_CHECK_MAX_TOKENS")
    if any(name not in config for name in required) or any(not config[name] for name in required[:3]):
        print("请通过 bash check_ai.sh 运行；配置只填match_config.hpp并重新构建。")
        return 2
    result = probe(config["PRACTICE_LLM_URL"], config["PRACTICE_LLM_KEY"], config["PRACTICE_MODEL"],
                   config["RMTC_CHECK_THINKING"], config["RMTC_CHECK_JSON_OUTPUT"] == "1",
                   int(config["RMTC_CHECK_MAX_TOKENS"]))
    result["upstream_proxy"] = "configured" if config.get("https_proxy") else "direct"
    result["upstream_host"] = urlsplit(config["PRACTICE_LLM_URL"]).hostname  # 只显示主机，不显示URL凭据或查询参数。
    result["model"] = config["PRACTICE_MODEL"]
    print(json.dumps(result, ensure_ascii=False))
    print("此请求直测练习上游，不经过比赛网关，不计本场LLM成功次数或正式资格。")
    return 0 if result["kind"] == "request_ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
