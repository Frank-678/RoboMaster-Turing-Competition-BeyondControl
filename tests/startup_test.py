#!/usr/bin/env python3
"""用临时工作空间检查旧配置拦截；用exec占位检查启动器环境，不需要ROS或真实密钥。"""
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]  # 当前选手工程，只读取被测脚本和源码。


def run_guard(workspace, script):
    # script为run.sh/practice.sh；只在临时副本中移除系统ROS环境加载，不改原文件。
    text = (ROOT / script).read_text().replace("source /opt/ros/humble/setup.bash", ": # fixture has no ROS")
    (workspace / script).write_text(text)
    return subprocess.run(["bash", script], cwd=workspace, capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory() as folder:
        work = Path(folder)  # 所有假安装、摘要和编译产物的临时根目录。
        header = work / "src/rmtc_bot/include/rmtc_bot/match_config.hpp"
        header.parent.mkdir(parents=True)
        header.write_text("fixture configuration\n")
        (work / "build/rmtc_bot").mkdir(parents=True)
        (work / "install/rmtc_bot/lib/rmtc_bot").mkdir(parents=True)
        (work / "install/setup.bash").write_text("")
        # 这是临时夹具里的空标记文件；从不写实际仓库的practice/。
        (work / "practice").mkdir()
        (work / "practice/start_referee.sh").write_text("")
        for name in ("bot", "practice_launcher"):
            binary = work / "install/rmtc_bot/lib/rmtc_bot" / name
            binary.write_text("#!/bin/sh\necho fixture-binary\n")
            binary.chmod(0o755)
        marker = work / "build/rmtc_bot/match_config.sha256"  # 同build.sh格式的配置摘要。
        digest = hashlib.sha256(header.read_bytes()).hexdigest()
        for script in ("run.sh", "practice.sh", "check_ai.sh"):
            marker.unlink(missing_ok=True)
            assert run_guard(work, script).returncode == 2  # 无构建记录时不能偷偷运行已有二进制。
            marker.write_text(digest + "  src/rmtc_bot/include/rmtc_bot/match_config.hpp\n")
            if script == "check_ai.sh":
                check_marker = work / "build/rmtc_bot/ai_check.sha256"
                assert run_guard(work, script).returncode == 2  # 旧构建没有新入口记录，不能传未知参数误启动裁判。
                binary = work / "install/rmtc_bot/lib/rmtc_bot/practice_launcher"
                check_marker.write_text(hashlib.sha256(binary.read_bytes()).hexdigest() +
                                        "  install/rmtc_bot/lib/rmtc_bot/practice_launcher\n")
                original_binary = binary.read_text()
                binary.write_text("#!/bin/sh\necho stale-launcher\n")
                assert run_guard(work, script).returncode == 2
                binary.write_text(original_binary)
            result = run_guard(work, script)
            assert result.returncode == 0 and "fixture-binary" in result.stdout
            if script == "run.sh":
                assert any("fixture-binary" in p.read_text() for p in (work / "log").glob("bot_*.log"))
                bot = work / "install/rmtc_bot/lib/rmtc_bot/bot"
                original = bot.read_text()
                bot.write_text(original + "exit 7\n")
                assert run_guard(work, script).returncode == 7  # tee不能把真实机器人失败伪装成成功。
                bot.write_text(original)
            header.write_text("changed after compilation\n")
            assert run_guard(work, script).returncode == 2  # 改配置后仍旧二进制：必须拒绝。
            header.write_text("fixture configuration\n")
        # 编译真实启动器，仅将配置和exec目标替换为夹具，检查实际设置的环境/参数。
        config = (ROOT / "src/rmtc_bot/include/rmtc_bot/match_config.hpp").read_text()
        for key, value in {"ai_model": "fixture-model", "practice_llm_url": "http://upstream.invalid/chat/completions",
                           "practice_llm_key": "fixture-key", "practice_llm_proxy": "http://proxy.invalid:7890"}.items():
            prefix = f'inline constexpr const char *{key} = '
            lines = config.splitlines()
            config = "\n".join(prefix + '"' + value + '";' if line.startswith(prefix) else line for line in lines) + "\n"
        # 将输出上限固定为测试专用值；不依赖正式赛配置当前是512还是其他值。
        # 故意使用与正式配置不同的值，验证启动器确实传递编译期配置。
        import re
        config, replacements = re.subn(
            r"(?m)^(inline constexpr int ai_max_tokens = )\d+;",
            r"\g<1>731;", config)
        assert replacements == 1, "ai_max_tokens declaration changed; update fixture"
        header.write_text(config)
        stub = work / "exec_stub.cpp"
        stub.write_text(r'''
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
// exec占位只检查环境和参数，绝不启动替代裁判或连接模型。
extern "C" int execl(const char *path, const char *arg, ...) noexcept {
  const auto matches = [](const char *key, const char *expected) {
    const char *value = std::getenv(key); return value && std::strcmp(value, expected) == 0;
  };
  if (std::strcmp(path, "/bin/bash") || std::strcmp(arg, "bash") ||
      !matches("NO_PROXY", "localhost,127.0.0.1,::1") || !matches("no_proxy", "localhost,127.0.0.1,::1") ||
      !matches("http_proxy", "http://proxy.invalid:7890") || !matches("PRACTICE_MODEL", "fixture-model") ||
      !matches("PRACTICE_LLM_KEY", "fixture-key") || !matches("PYTHONDONTWRITEBYTECODE", "1")) std::exit(3);
  std::vector<std::string> args; // 捕获真实启动器给原脚本的参数。
  va_list values; va_start(values, arg);
  while (const char *value = va_arg(values, const char *)) args.emplace_back(value);
  va_end(values);
  if (args != std::vector<std::string>{"practice/start_referee.sh", "--stage", "2", "--port", "8080", "--ai-mode", "real"}) std::exit(4);
  std::exit(0);
}
// 出网检查复用同一份已编译配置；密钥仍只经进程环境交接，不放命令行。
extern "C" int execlp(const char *path, const char *arg, ...) noexcept {
  const auto matches = [](const char *key, const char *expected) {
    const char *value = std::getenv(key); return value && std::strcmp(value, expected) == 0;
  };
  if (std::strcmp(path, "python3") || std::strcmp(arg, "python3") ||
      !matches("PRACTICE_MODEL", "fixture-model") || !matches("PRACTICE_LLM_KEY", "fixture-key") ||
      !matches("https_proxy", "http://proxy.invalid:7890") ||
      !matches("RMTC_CHECK_THINKING", "disabled") || !matches("RMTC_CHECK_JSON_OUTPUT", "1") ||
      !matches("RMTC_CHECK_MAX_TOKENS", "731")) std::exit(5);
  va_list values; va_start(values, arg);
  const char *script = va_arg(values, const char *);
  if (!script || std::strcmp(script, "scripts/check_ai.py") || va_arg(values, const char *)) std::exit(6);
  va_end(values);
  std::puts("fixture-upstream-check");
  std::exit(0);
}
// 批量驱动也由同一启动器交接配置；参数只含测试范围，不含密钥。
extern "C" int execvp(const char *path, char *const args[]) noexcept {
  if (std::strcmp(path, "python3") || std::strcmp(args[1], "tools/batch_practice.py") ||
      std::strcmp(args[2], "--count") || std::strcmp(args[3], "4") || args[4] ||
      std::strcmp(std::getenv("PRACTICE_LLM_KEY"), "fixture-key") ||
      std::strcmp(std::getenv("PRACTICE_MODEL"), "fixture-model") ||
      std::strcmp(std::getenv("RMTC_PRACTICE_PORT"), "8080")) std::exit(7);
  std::puts("fixture-batch-config");
  std::exit(0);
}
''')
        launcher = work / "launcher"  # 只运行夹具编译产物；系统环境不改动。
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I", str(header.parents[1]),
                        str(ROOT / "src/rmtc_bot/src/practice_launcher.cpp"), str(stub), "-o", str(launcher)], check=True)
        env = dict(os.environ, NO_PROXY="bad-inherited-value", no_proxy="bad-inherited-value")
        subprocess.run([str(launcher)], env=env, check=True)
        result = subprocess.run([str(launcher), "--check-ai"], env=env, capture_output=True, text=True, check=True)
        assert "fixture-upstream-check" in result.stdout  # 不能误启动裁判，也不能漏掉检查分支。
        result = subprocess.run([str(launcher), "--batch", "--count", "4"], env=env, capture_output=True, text=True, check=True)
        assert "fixture-batch-config" in result.stdout
        print("startup guards and original-practice launcher environment passed; no ROS process launched")


if __name__ == "__main__":
    main()
