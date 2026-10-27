#include "rmtc_bot/strategy.hpp"

#include <array>
#include <cstdlib>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <string>

namespace {
using Json = nlohmann::json;
using Cell = std::pair<int, int>;
constexpr std::array<Cell, 4> steps = {{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
constexpr std::array<const char *, 4> directions = {{"up", "right", "down", "left"}};

// 每个实体有独立的 9×9 图：中心 1 是自己，2 是该实体。
// 中心重叠时没有 2；不能把单张图里的 0 当作空地。
std::optional<Cell> relative_position(const Json &position) {
  if (!position.is_string()) return std::nullopt;
  const auto &grid = position.get_ref<const std::string &>();
  if (grid.size() != 89 || grid[44] != '1') return std::nullopt;
  Cell offset{0, 0};
  bool found = false;
  for (int y = 0; y < 9; ++y) {
    if (y < 8 && grid[y * 10 + 9] != '\n') return std::nullopt;
    for (int x = 0; x < 9; ++x) {
      const char ch = grid[y * 10 + x];
      if (x == 4 && y == 4) continue;
      if (ch == '2' && !found) {
        offset = {x - 4, y - 4};
        found = true;
      } else if (ch != '0') {
        return std::nullopt;
      }
    }
  }
  return offset;
}
}  // namespace

void Strategy::reset() {
  // 新场次清空记忆；预测路线也不能跨场沿用。
  visits_.clear();
}

nlohmann::json Strategy::decide(const nlohmann::json &perception) {
  if (!perception.at("self").at("alive").get<bool>()) return {{"action", "wait"}};
  const auto &position = perception.at("self").at("position");
  const auto &map = perception.at("map");
  const auto &entities = perception.at("visible_entities");
  const auto &radius_json = perception.at("vision_radius");
  if (!position.is_array() || position.size() != 2 ||
      !position[0].is_number_integer() || !position[1].is_number_integer() ||
      !map.at("width").is_number_integer() || !map.at("height").is_number_integer() ||
      !radius_json.is_number_integer() || !entities.is_array()) {
    return {{"action", "wait"}};
  }
  const Cell start{position[0].get<int>(), position[1].get<int>()};
  const int width = map.at("width").get<int>();
  const int height = map.at("height").get<int>();
  const int radius = radius_json.get<int>();
  if (width <= 0 || height <= 0 || radius < 0 || radius > 4 ||
      start.first < 0 || start.first >= width || start.second < 0 || start.second >= height) {
    return {{"action", "wait"}};
  }

  std::set<Cell> blocked, resources;
  for (const auto &entity : entities) {
    const auto type = entity.at("type").get<std::string>();
    if (type != "obstacle" && type != "robot" &&
        type != "resource_normal" && type != "resource_high") continue;
    const auto offset = relative_position(entity.at("position"));
    if (!offset) return {{"action", "wait"}};
    // 相对位移先检查边界，再相加；畸形输入不会产生越界坐标。
    const auto [dx, dy] = *offset;
    if (std::abs(dx) > radius || std::abs(dy) > radius ||
        dx < -start.first || dx >= width - start.first ||
        dy < -start.second || dy >= height - start.second) {
      return {{"action", "wait"}};
    }
    const Cell cell{start.first + dx, start.second + dy};
    if (type == "obstacle" || type == "robot") blocked.insert(cell);
    else resources.insert(cell);
  }

  ++visits_[start];
  // 只在本帧视野内搜索；未知区域不按空地规划，不读取练习配置。
  std::queue<Cell> frontier;
  std::map<Cell, int> first_step;
  frontier.push(start);
  first_step[start] = -1;
  int explore_step = -1;
  unsigned least_visits = std::numeric_limits<unsigned>::max();
  while (!frontier.empty()) {
    const Cell current = frontier.front();
    frontier.pop();
    const int step = first_step.at(current);
    if (step != -1) {
      if (resources.count(current)) {
        return {{"action", "move"}, {"direction", directions[step]}};
      }
      const auto visit = visits_.find(current);
      const unsigned count = visit == visits_.end() ? 0 : visit->second;
      // BFS 保证访问次数相同时选较近的格子，再按上、右、下、左破平局。
      if (count < least_visits) {
        least_visits = count;
        explore_step = step;
      }
    }
    for (int i = 0; i < 4; ++i) {
      const Cell next{current.first + steps[i].first, current.second + steps[i].second};
      if (next.first < 0 || next.first >= width || next.second < 0 || next.second >= height ||
          std::abs(next.first - start.first) > radius ||
          std::abs(next.second - start.second) > radius ||
          blocked.count(next) || first_step.count(next)) continue;
      first_step[next] = step == -1 ? i : step;
      frontier.push(next);
    }
  }
  // 每 Tick 只执行一步，下一帧依据实际坐标重新规划，移动失败也不会跳步。
  // TODO: 接入官方 LLM 校验后的公告效果，再加入事件、任务与战斗策略。
  if (explore_step != -1) {
    return {{"action", "move"}, {"direction", directions[explore_step]}};
  }
  return {{"action", "wait"}};
}
