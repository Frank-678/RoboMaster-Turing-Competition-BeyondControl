#include "rmtc_bot/strategy.hpp"

void Strategy::reset() {
  // TODO: reset your strategy state when a new session starts.
  // 所有（TO THINK: 是否除了预测性的后面的决定？）私有变量置位null/initialization_value
}

nlohmann::json Strategy::decide(const nlohmann::json &perception) {
  // TODO: write your strategy here, using only public perception.
  // Examples of return values:
  // return {{"action", "move"}, {"direction", "right"}};
  // return {{"action", "attack"}, {"direction", "up"}};
  // return {{"action", "defend"}};
  (void)perception;
  // 接收 1. 从subsriber来的信息比如9*9等等 2. 公告来的自然语言经过llm处理
  // 计算方法，返回一个{,}。 （TO THINK: 预测性地输出后面的决定，但后来会有变化，如何？）
  return {{"action", "wait"}};
}
