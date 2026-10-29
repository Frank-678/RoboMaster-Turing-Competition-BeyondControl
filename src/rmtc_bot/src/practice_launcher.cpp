#include "rmtc_bot/match_config.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

// 只给原版practice设置启动环境再执行它；不改practice中的任何文件。
int main(int argc, char **argv) {
  namespace cfg = rmtc::match_config; // 与机器人共用的唯一配置。
  const bool check_ai = argc == 2 && std::string(argv[1]) == "--check-ai"; // 只由check_ai.sh选择；不是比赛运行开关。
  const bool batch = argc >= 2 && std::string(argv[1]) == "--batch"; // 批量测试也从这里取得同一份已编译网络配置。
  if (argc != 1 && !check_ai && !batch) {
    std::cerr << "Use bash practice.sh or bash check_ai.sh; settings stay in match_config.hpp\n"; return 2;
  }
  if (cfg::stage != 1 && cfg::stage != 2) {
    std::cerr << "stage must be 1 or 2 in match_config.hpp\n"; return 2;
  }
  const bool real = check_ai || batch || cfg::stage == 2; // 批量对抗与出网检查均用真实上游。
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
  // 测试也不刷新原版practice/__pycache__中的字节码文件。
  if (!set("PYTHONDONTWRITEBYTECODE", "1")) return 1;
  if (!set("ROS_DOMAIN_ID", cfg::ros_domain_id) || !set("ROS_LOCALHOST_ONLY", cfg::ros_localhost_only) ||
      !set("RMW_IMPLEMENTATION", cfg::rmw_implementation) ||
      !set("PRACTICE_LLM_URL", cfg::practice_llm_url) || !set("PRACTICE_LLM_KEY", cfg::practice_llm_key) ||
      !set("PRACTICE_MODEL", cfg::ai_model)) {
    std::cerr << "Cannot apply practice settings\n"; return 1;
  }
  for (const char *key : {"http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"})
    if (!set(key, cfg::practice_llm_proxy)) return 1;
  // 只让代理→上游出网走网络代理。陪练→本机8080必须直连，否则也会被送到出网代理。
  if (!set("NO_PROXY", "localhost,127.0.0.1,::1") || !set("no_proxy", "localhost,127.0.0.1,::1")) return 1;
  if (batch) {
    if (!set("RMTC_PRACTICE_PORT", std::to_string(cfg::practice_port).c_str())) return 1;
    // 只转交测试场景/输出参数；模型、密钥、域、代理仍只有match_config.hpp这一处配置。
    std::vector<char *> command{const_cast<char *>("python3"), const_cast<char *>("tools/batch_practice.py")};
    for (int i = 2; i < argc; ++i) command.push_back(argv[i]);
    command.push_back(nullptr);
    ::execvp(command.front(), command.data());
    std::cerr << "Cannot run tools/batch_practice.py\n"; return 1;
  }
  if (check_ai) {
    // 用同一份已编译模型/密钥/代理，只请求一次最小JSON；不启动裁判、不读练习地图。
    if (!set("RMTC_CHECK_THINKING", cfg::ai_thinking) ||
        !set("RMTC_CHECK_JSON_OUTPUT", cfg::ai_json_output ? "1" : "0") ||
        !set("RMTC_CHECK_MAX_TOKENS", std::to_string(cfg::ai_max_tokens).c_str())) return 1;
    ::execlp("python3", "python3", "scripts/check_ai.py", static_cast<char *>(nullptr));
    std::cerr << "Cannot run scripts/check_ai.py; install the Python requests dependency\n"; return 1;
  }
  std::cout << "Practice: stage=" << cfg::stage << " port=" << cfg::practice_port << " mode=" << (real ? "real" : "mock")
            << " model=" << cfg::ai_model << " upstream_proxy=" << (*cfg::practice_llm_proxy ? "configured" : "direct")
            << " local_gateway=direct built=" << __DATE__ << ' ' << __TIME__ << '\n' << std::flush;
  const auto stage = std::to_string(cfg::stage); // 原版命令行需要字符串形式的阶段。
  const auto port = std::to_string(cfg::practice_port); // 代理监听端口的字符串形式。
  // 第一阶段不需要LLM；第二阶段明确选real，避免误开mock后仍以为在调用模型。
  ::execl("/bin/bash", "bash", "practice/start_referee.sh", "--stage", stage.c_str(), "--port", port.c_str(),
          "--ai-mode", real ? "real" : "mock", static_cast<char *>(nullptr));
  std::cerr << "Cannot start original practice/start_referee.sh\n";
  return 1;
}
