#include "rmtc_bot/protocol.hpp"
#include "rmtc_bot/strategy.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

using Json = nlohmann::json;
using Cell = std::pair<int, int>;

Json frame(int x = 4, int y = 4, int width = 9, int height = 9, int radius = 4) {
  return {{"tick", 0}, {"session_id", "test-match"}, {"vision_radius", radius},
          {"self", {{"alive", true}, {"position", {x, y}}, {"protected", false}}},
          {"map", {{"width", width}, {"height", height}}},
          {"visible_entities", Json::array()}};
}

Json entity(const char *type, int dx, int dy) {
  std::string grid;
  for (int y = 0; y < 9; ++y) {
    if (y) grid += '\n';
    for (int x = 0; x < 9; ++x) {
      grid += (x == 4 && y == 4) ? '1' : (x == dx + 4 && y == dy + 4) ? '2' : '0';
    }
  }
  return {{"type", type}, {"position", grid}};
}

void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void expect(const Json &p, const char *action, const char *direction = nullptr) {
  Strategy strategy;
  const auto result = rmtc::action_for(p, strategy.decide(p));
  check(result.at("action") == action, "unexpected action");
  if (direction) check(result.at("direction") == direction, "unexpected direction");
  check(result.at("tick") == p.at("tick") && result.at("session_id") == p.at("session_id"),
        "action lost the perception envelope");
}

void scenarios() {
  auto p = frame();
  p["visible_entities"] = {entity("resource_normal", 1, 0), entity("resource_high", 0, -3)};
  expect(p, "move", "right");  // 最近资源优先，并非直接朝最高价值资源走。
  p["visible_entities"] = {entity("resource_high", 0, -1), entity("resource_normal", 1, 0)};
  expect(p, "move", "up");

  for (const auto *blocker : {"obstacle", "robot"}) {
    p["visible_entities"] = {entity("resource_normal", 2, 0), entity(blocker, 1, 0)};
    expect(p, "move", "up");  // 不能把各实体的相对图混成一张图。
  }
  p["visible_entities"] = {entity("resource_normal", 2, 0), entity("resource_high", 0, 2),
                           entity("obstacle", 1, 0), entity("obstacle", 2, -1),
                           entity("obstacle", 2, 1), entity("obstacle", 3, 0)};
  expect(p, "move", "down");  // 围住的资源不可达，选择另一个。
  p["visible_entities"] = {entity("resource_normal", 2, 0),
                           entity("obstacle", 1, 0), entity("obstacle", 0, -1)};
  expect(p, "move", "down");

  expect(frame(0, 0, 6, 6), "move", "right");
  expect(frame(5, 5, 6, 6), "move", "up");
  expect(frame(0, 0, 1, 1), "wait");
  expect(frame(4, 4, 9, 9, 0), "wait");
  p = frame();
  p["visible_entities"] = {entity("obstacle", 0, -1), entity("obstacle", 1, 0),
                           entity("obstacle", 0, 1), entity("obstacle", -1, 0)};
  expect(p, "wait");
  p["self"]["alive"] = false;
  expect(p, "wait");
  p = frame();
  p["visible_entities"] = {entity("resource_normal", 0, 0)};
  expect(p, "move", "up");  // 重叠图没有 2，不能误判为损坏或卡在原地。

  p = frame(4, 4, 9, 9, 1);
  p["visible_entities"] = {entity("resource_normal", 1, 0)};
  expect(p, "move", "right");
  p["visible_entities"] = {entity("resource_normal", 2, 0)};
  expect(p, "wait");
  p = frame(0, 0, 6, 6);
  p["visible_entities"] = {entity("resource_normal", -1, 0)};
  expect(p, "wait");
  p = frame();
  p["visible_entities"] = {entity("obstacle", 1, 0)};
  const auto valid_grid = p["visible_entities"][0]["position"].get<std::string>();
  for (int index : {9, 44, 0}) {
    auto invalid_grid = valid_grid;
    invalid_grid[index] = '2';  // 坏换行、坏中心、多个 2。
    p["visible_entities"][0]["position"] = invalid_grid;
    expect(p, "wait");
  }
  p["visible_entities"][0]["position"] = Json::array({1, 0});
  expect(p, "wait");
  p["visible_entities"][0]["position"] = "000";
  expect(p, "wait");
  p = frame();
  p["self"]["position"] = {4.5, 4};
  expect(p, "wait");
  expect(frame(-1, 0), "wait");
  expect(frame(0, 0, 0, 9), "wait");
  expect(frame(0, 0, 9, 9, 5), "wait");

  Strategy strategy;
  check(strategy.decide(frame()).at("direction") == "up", "initial exploration changed");
  strategy.decide(frame(4, 3));  // 实际到过上方，再回到原格。
  check(strategy.decide(frame()).at("direction") == "right", "visited cells were ignored");
  strategy.reset();
  check(strategy.decide(frame()).at("direction") == "up", "new session kept old visits");
  p = frame();
  p["visible_entities"] = {entity("resource_normal", 2, 0)};
  check(strategy.decide(p).at("direction") == "right", "resource target lost");
  p["tick"] = 1;  // 上一步移动失败，实际位置没变；新的感知增加了障碍。
  p["visible_entities"].push_back(entity("obstacle", 1, 0));
  check(strategy.decide(p).at("direction") == "up", "failed move advanced a cached path");
}

// 合成地图只由测试端生成公开感知，策略不会收到全图或隐藏实体。
int collect_in_synthetic_maze() {
  const std::set<Cell> walls = {{2, 0}, {2, 1}, {2, 2}, {2, 3}};
  std::set<Cell> resources = {{4, 0}, {5, 5}, {0, 5}};
  Cell current{0, 0};
  Strategy strategy;
  for (int tick = 0; tick < 80; ++tick) {
    auto p = frame(current.first, current.second, 6, 6);
    p["tick"] = tick;
    const auto add_visible = [&](const auto &cells, const char *type) {
      for (const auto &[x, y] : cells) {
        const int dx = x - current.first, dy = y - current.second;
        if (std::abs(dx) <= 4 && std::abs(dy) <= 4) {
          p["visible_entities"].push_back(entity(type, dx, dy));
        }
      }
    };
    add_visible(walls, "obstacle");
    add_visible(resources, "resource_normal");
    const auto action = rmtc::action_for(p, strategy.decide(p));
    check(action.at("action") == "move", "collector stopped in a traversable maze");
    const auto direction = action.at("direction").get<std::string>();
    Cell next = current;
    if (direction == "up") --next.second;
    else if (direction == "right") ++next.first;
    else if (direction == "down") ++next.second;
    else if (direction == "left") --next.first;
    else throw std::runtime_error("invalid move direction");
    check(next.first >= 0 && next.first < 6 && next.second >= 0 && next.second < 6 &&
          !walls.count(next), "collector hit a wall or map boundary");
    current = next;
    resources.erase(current);
    if (resources.empty()) return tick + 1;
  }
  throw std::runtime_error("collector did not collect the three resources within 80 ticks");
}

int main() {
  try {
    scenarios();
    const int ticks = collect_in_synthetic_maze();
    std::cout << "strategy scenarios passed; synthetic maze: 3 resources in " << ticks << " ticks\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
