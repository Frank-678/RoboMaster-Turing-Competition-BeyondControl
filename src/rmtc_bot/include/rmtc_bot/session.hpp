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
  Session(int stage, AiConfig config = {});
  std::optional<nlohmann::json> act(const nlohmann::json &perception);
  const std::string &reason() const { return reason_; }
  std::string ai_status() const { return ai_.status(); }
  bool ai_http_success() const { return ai_.observed_http_success(); }
  nlohmann::json ai_diagnostics() const { return ai_.diagnostics(); }
private:
  int stage_;
  Strategy strategy_;
  AiWorker ai_;
  std::string session_, reason_;
  std::set<std::string> retired_;
  Tick latest_tick_ = -1;
  nlohmann::json cached_;
};
}  // namespace rmtc
