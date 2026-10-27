#include "rmtc_bot/match_config.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

// 只给原版practice设置启动环境再执行它；不改practice中的任何文件。
int main() {
  namespace cfg = rmtc::match_config; // 与机器人共用的唯一配置。
  if (cfg::stage != 1 && cfg::stage != 2) {
    std::cerr << "stage must be 1 or 2 in match_config.hpp\n"; return 2;
  }
  const bool real = cfg::stage == 2; // 第二阶段必须真实模型；第一阶段没有AI任务。
  if (cfg::practice_port < 1 || cfg::practice_port > 65535) {
    std::cerr << "practice_port must be 1-65535\n"; return 2;
  }
  if (real && (!*cfg::practice_llm_url || !*cfg::practice_llm_key || !*cfg::ai_model)) {
    std::cerr << "Fill practice_llm_url, practice_llm_key and ai_model in match_config.hpp, then rebuild\n";
    return 2;
  }
  const std::string upstream = cfg::practice_llm_url; // 练习代理向外转发的目标，不是机器人接收地址。
  if (real && (upstream.size() < 17 || upstream.compare(upstream.size() - 17, 17, "/chat/completions") != 0)) {
    std::cerr << "practice_llm_url must be the full /chat/completions endpoint\n"; return 2;
  }
  // 这些变量由原版练习代理读取。用户只改头文件，不需要手工export。
  const auto set = [](const char *key, const char *value) { return ::setenv(key, value, 1) == 0; }; // key变量名/value明确值；覆盖继承环境。
  if (!set("ROS_DOMAIN_ID", cfg::ros_domain_id) || !set("ROS_LOCALHOST_ONLY", cfg::ros_localhost_only) ||
      !set("RMW_IMPLEMENTATION", cfg::rmw_implementation) ||
      !set("PRACTICE_LLM_URL", cfg::practice_llm_url) || !set("PRACTICE_LLM_KEY", cfg::practice_llm_key) ||
      !set("PRACTICE_MODEL", cfg::ai_model)) {
    std::cerr << "Cannot apply practice settings\n"; return 1;
  }
  for (const char *key : {"http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"})
    if (!set(key, cfg::practice_llm_proxy)) return 1;
  if (!set("NO_PROXY", "") || !set("no_proxy", "")) return 1;
  const auto stage = std::to_string(cfg::stage); // 原版命令行需要字符串形式的阶段。
  const auto port = std::to_string(cfg::practice_port); // 代理监听端口的字符串形式。
  // 第一阶段不需要LLM；第二阶段明确选real，避免误开mock后仍以为在调用模型。
  ::execl("/bin/bash", "bash", "practice/start_referee.sh", "--stage", stage.c_str(), "--port", port.c_str(),
          "--ai-mode", real ? "real" : "mock", static_cast<char *>(nullptr));
  std::cerr << "Cannot start original practice/start_referee.sh\n";
  return 1;
}
