#pragma once
#include <nlohmann/json.hpp>

class Strategy {
public:
  // Called once on the first frame of each new session.
  void reset();

  // Called once per new tick. Return action and, if needed, direction.
  // The communication layer supplies tick and session_id.
  nlohmann::json decide(const nlohmann::json &perception);

private:
  // Add your strategy state here.
};
