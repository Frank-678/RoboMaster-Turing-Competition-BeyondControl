#include "rmtc_bot/strategy.hpp"

void Strategy::reset() {
  // TODO: reset your strategy state when a new session starts.
}

nlohmann::json Strategy::decide(const nlohmann::json &perception) {
  // TODO: write your strategy here, using only public perception.
  // Examples of return values:
  // return {{"action", "move"}, {"direction", "right"}};
  // return {{"action", "attack"}, {"direction", "up"}};
  // return {{"action", "defend"}};
  (void)perception;
  return {{"action", "wait"}};
}
