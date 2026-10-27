#pragma once
#include <map>
#include <utility>
#include <nlohmann/json.hpp>

class Strategy {
public:
  // Called once on the first frame of each new session.
  void reset();

  // Called once per new tick. Return action and, if needed, direction.
  // The communication layer supplies tick and session_id.
  nlohmann::json decide(const nlohmann::json &perception);

private:
  // 只记录真正到达过的格子，不把计划中的移动当作已执行。
  std::map<std::pair<int, int>, unsigned> visits_;
};
