#include "rmtc_bot/protocol.hpp"
#include "rmtc_bot/strategy.hpp"
#include "rmtc_bot/session.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

// 基础策略回归。这里生成的完整地图只在测试端，交给选手的仍是公开局部感知。
using Json = nlohmann::json; // 构造测试消息。
using Cell = std::pair<int, int>; // 测试地图格子[x,y]。
void full_scenarios();
void gateway_scenarios(const std::string &base);

Json frame(int x = 4, int y = 4, int width = 9, int height = 9, int radius = 4) {
  // x/y本车位置，width/height地图尺寸，radius实际视野。省略参数仅用于固定测试夹具。
  return {{"tick", 0}, {"session_id", "test-match"}, {"vision_radius", radius},
          {"self", {{"alive", true}, {"position", {x, y}}, {"protected", false}}},
          {"map", {{"width", width}, {"height", height}}},
          {"visible_entities", Json::array()}};
}

Json entity(const char *type, int dx, int dy) {
  // type为实体类型，dx/dy为相对位移；生成固定世界轴下的9×9实体图。
  std::string grid; // 拼成JSON中的position字符串；x/y是图内列/行。
  for (int y = 0; y < 9; ++y) {
    if (y) grid += '\n';
    for (int x = 0; x < 9; ++x) {
      grid += (x == 4 && y == 4) ? '1' : (x == dx + 4 && y == dy + 4) ? '2' : '0';
    }
  }
  return {{"type", type}, {"position", grid}};
}

void check(bool condition, const char *message) {
  // condition是假时抛message，让整个回归以非零退出码失败。
  if (!condition) throw std::runtime_error(message);
}

void expect(const Json &p, const char *action, const char *direction = nullptr) {
  // p是感知；action是预期动作或none；direction为空表示不检查方向。
  rmtc::Session session(1); // 独立第一阶段场次，防止场景之间共享记忆。
  const auto chosen = session.act(p); // 可选的完整动作。
  if (std::string(action) == "none") {
    check(!chosen, "stage1 published a fallback action");
    return;
  }
  check(chosen.has_value(), "expected action was suppressed");
  const auto &result = *chosen; // 已确认存在的动作，随后检查协议身份。
  check(result.at("action") == action, "unexpected action");
  if (direction) check(result.at("direction") == direction, "unexpected direction");
  check(result.at("tick") == p.at("tick") && result.at("session_id") == p.at("session_id"),
        "action lost the perception envelope");
}

void scenarios() {
  auto p = frame(); // 逐步改变公开感知，覆盖资源、墙、边界、坏输入和失败移动。
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
  expect(frame(0, 0, 1, 1), "none");
  expect(frame(4, 4, 9, 9, 0), "move", "up");
  p = frame();
  p["visible_entities"] = {entity("obstacle", 0, -1), entity("obstacle", 1, 0),
                           entity("obstacle", 0, 1), entity("obstacle", -1, 0)};
  expect(p, "none");
  p["self"]["alive"] = false;
  expect(p, "none");
  p = frame();
  p["visible_entities"] = {entity("resource_normal", 0, 0)};
  expect(p, "none");  // 重叠图没有 2；不发布动作也会自动收集。

  p = frame(4, 4, 9, 9, 1);
  p["visible_entities"] = {entity("resource_normal", 1, 0)};
  expect(p, "move", "right");
  p["visible_entities"] = {entity("resource_normal", 2, 0)};
  expect(p, "none");
  p = frame(0, 0, 6, 6);
  p["visible_entities"] = {entity("resource_normal", -1, 0)};
  expect(p, "none");
  p = frame();
  p["visible_entities"] = {entity("obstacle", 1, 0)};
  const auto valid_grid = p["visible_entities"][0]["position"].get<std::string>(); // 未破坏的实体图样本。
  for (int index : {9, 44, 0}) {
    auto invalid_grid = valid_grid; // 每次只破坏index位置，验证解析器拒绝。
    invalid_grid[index] = '2';  // 坏换行、坏中心、多个 2。
    p["visible_entities"][0]["position"] = invalid_grid;
    expect(p, "none");
  }
  p["visible_entities"][0]["position"] = Json::array({1, 0});
  expect(p, "none");
  p["visible_entities"][0]["position"] = "000";
  expect(p, "none");
  p = frame();
  p["self"]["position"] = {4.5, 4};
  expect(p, "none");
  expect(frame(-1, 0), "none");
  expect(frame(0, 0, 0, 9), "none");
  expect(frame(0, 0, 9, 9, 5), "none");

  Strategy strategy(1); // 复用同一个策略，验证实际访问记忆和reset。
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
  const std::set<Cell> walls = {{2, 0}, {2, 1}, {2, 2}, {2, 3}}; // 仅测试端知道的墙。
  std::set<Cell> resources = {{4, 0}, {5, 5}, {0, 5}}; // 仅测试端知道的剩余资源。
  Cell current{0, 0}; // 测试端的真实位置，成功执行后才更新。
  Strategy strategy(1); // 被测第一阶段采集策略。
  for (int tick = 0; tick < 80; ++tick) {
    auto p = frame(current.first, current.second, 6, 6); // 这一Tick交给选手的感知。
    p["tick"] = tick;
    const auto add_visible = [&](const auto &cells, const char *type) {
      // cells是测试端墙/资源集合，type决定实体类型；只加入视野内的x/y。
      for (const auto &[x, y] : cells) {
        const int dx = x - current.first, dy = y - current.second; // 实体相对本车的位置。
        if (std::abs(dx) <= 4 && std::abs(dy) <= 4) {
          p["visible_entities"].push_back(entity(type, dx, dy));
        }
      }
    };
    add_visible(walls, "obstacle");
    add_visible(resources, "resource_normal");
    const auto action = rmtc::action_for(p, strategy.decide(p), 1); // 策略结果经过真实动作检查层。
    check(action.at("action") == "move", "collector stopped in a traversable maze");
    const auto direction = action.at("direction").get<std::string>(); // 计划走的世界方向。
    Cell next = current; // 测试端计算的落点，检查边界/墙后才真正采纳。
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
  // 运行核心场景；RMTC_TEST_GATEWAY只由本地测试夹具传入，不用于正式机器人配置。
  try {
    scenarios();
    full_scenarios();
    const int ticks = collect_in_synthetic_maze(); // 收完三个资源花了多少Tick。
    if (const char *gateway = std::getenv("RMTC_TEST_GATEWAY")) gateway_scenarios(gateway);
    std::cout << "strategy scenarios passed; synthetic maze: 3 resources in " << ticks << " ticks\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
