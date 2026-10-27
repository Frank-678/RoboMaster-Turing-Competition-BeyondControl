#pragma once
#include <optional>
#include <set>
#include <string>
#include "rmtc_bot/ai_worker.hpp"
#include "rmtc_bot/strategy.hpp"

namespace rmtc {
// ROS 与离线回放共用：缓存、换场、异步结果接纳在同一处验证。
class Session {
public:
  Session(int stage, AiConfig config = {}); // stage决定动作限制；config交给本场AI工作线程。
  std::optional<nlohmann::json> act(const nlohmann::json &perception); // 一帧输入→可发布的完整动作，或不发布。
  const std::string &reason() const { return reason_; } // 最近决策原因。
  std::string ai_status() const { return ai_.status(); } // 后台请求/事件理解状态。
  bool ai_http_success() const { return ai_.observed_http_success(); } // 完整HTTP成功，非正式资格。
  nlohmann::json ai_diagnostics() const { return ai_.diagnostics(); } // 不含密钥的计数与错误码。
private:
  int stage_; // 本次进程固定的比赛阶段。
  Strategy strategy_; // 地图记忆和本地决策。
  AiWorker ai_; // 公告解释请求的后台线程。
  std::string session_, reason_; // 当前场次、上次决策原因。
  std::set<std::string> retired_; // 已结束的场次，迟到消息不能把程序切回旧场。
  Tick latest_tick_ = -1; // 本场已经处理过的最大Tick；-1表示尚未处理。
  nlohmann::json cached_; // 同Tick重发的缓存动作；null表示该Tick选择不发布。
};
}  // namespace rmtc
