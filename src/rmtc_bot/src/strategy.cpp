#include "rmtc_bot/strategy.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
using Cell = rmtc::Cell;
using Tick = rmtc::Tick;
using Kind = rmtc::EventKind;
constexpr std::array<Cell, 4> steps = {{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
constexpr std::array<const char *, 4> directions = {{"up", "right", "down", "left"}};
int integer(const Json &j) {
  if (!j.is_number_integer() || (j.is_number_unsigned() &&
      j.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())))
    throw std::runtime_error("invalid integer field");
  const auto value = j.get<std::int64_t>();
  if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
    throw std::runtime_error("integer out of range");
  return static_cast<int>(value);
}
Cell absolute_position(const Json &j) {
  if (!j.is_array() || j.size() != 2) throw std::runtime_error("invalid absolute position");
  return {integer(j[0]), integer(j[1])};
}
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
      if (ch == '2' && !found) { offset = {x - 4, y - 4}; found = true; }
      else if (ch != '0') return std::nullopt;
    }
  }
  return offset;
}
int manhattan(Cell a, Cell b) {
  return std::abs(a.first - b.first) + std::abs(a.second - b.second);
}
int ray(Cell from, Cell target) {
  const int dx = target.first - from.first, dy = target.second - from.second;
  if (dx == 0 && dy >= -2 && dy <= -1) return 0;
  if (dy == 0 && dx >= 1 && dx <= 2) return 1;
  if (dx == 0 && dy >= 1 && dy <= 2) return 2;
  if (dy == 0 && dx >= -2 && dx <= -1) return 3;
  return -1;
}
struct Enemy { Cell position; double hp; bool defending, protected_spawn; };
struct Entity { std::string type; Cell position; double value; bool high; };
}  // namespace

Strategy::Strategy(int stage) : stage_(stage) {
  if (stage != 1 && stage != 2) throw std::runtime_error("stage must be 1 or 2");
}
void Strategy::reset() {
  tiles_.clear(); history_.clear(); events_.clear(); capture_progress_.clear(); spawn_.reset();
  width_ = height_ = 0; reversed_ = reverse_unknown_ = empowered_ = false;
  vulnerability_ = 1; attack_damage_ = 20; defended_damage_ = 10; healing_until_ = -1;
  reason_ = "new_session";
}
void Strategy::merge_events(const std::vector<rmtc::Event> &events) {
  if (stage_ == 1) return;
  for (const auto &e : events) events_[e.key] = e;
}
Json Strategy::known_events() const {
  auto result = Json::array();
  for (const auto &[key, event] : events_) { (void)key; result.push_back(rmtc::event_summary(event)); }
  return result;
}
bool Strategy::split_wall(Cell cell, Tick tick) const {
  if (spawn_ && cell == *spawn_) return false;
  for (const auto &[key, e] : events_) {
    (void)key;
    if (e.kind == Kind::split && e.active(tick) && e.contains(cell)) return true;
  }
  return false;
}
bool Strategy::dangerous(Cell cell, Tick tick) const {
  if (split_wall(cell, tick)) return true;
  for (const auto &[key, e] : events_) {
    (void)key;
    if (e.kind == Kind::explosion && e.start == tick && e.contains(cell) && e.amount > 0) return true;
  }
  return false;
}
void Strategy::update_effects(Tick tick) {
  reversed_ = reverse_unknown_ = empowered_ = false;
  vulnerability_ = 1; attack_damage_ = 20; defended_damage_ = 10; healing_until_ = -1;
  capture_progress_.clear();
  for (const auto &[key, e] : events_) {
    if (e.kind == Kind::illusion && e.active(tick)) {
      const auto initial = history_.find(e.start);
      if (initial == history_.end()) reverse_unknown_ = true;
      else if (initial->second.alive && e.contains(initial->second.position)) reversed_ = true;
    }
    int consecutive = 0;
    Tick previous = -2;
    bool awarded = false;
    for (const auto &[state_tick, state] : history_) {
      (void)state_tick;
      const auto executed = state.receipt_tick;
      if (executed < e.start || executed >= e.end) continue;
      if (e.kind == Kind::vulnerability && e.active(tick) && state.moved && executed < e.trigger_end)
        vulnerability_ = std::max(vulnerability_, e.multiplier);
      if (e.kind == Kind::heal && state.moved && !state.respawn && e.contains(state.position))
        healing_until_ = std::max(healing_until_, executed + e.effect_ticks);
      if (e.kind == Kind::fortress && e.active(tick)) {
        if (executed != previous + 1) consecutive = 0;
        consecutive = state.alive && !state.respawn && e.contains(state.position) ? consecutive + 1 : 0;
        previous = executed;
        if (consecutive >= e.capture_ticks) awarded = true;
      }
    }
    if (e.kind == Kind::fortress && e.active(tick)) {
      capture_progress_[key] = consecutive;
      if (awarded) {
        empowered_ = true;
        attack_damage_ = std::max(attack_damage_, e.amount);
        defended_damage_ = e.defended_damage;
      }
    }
  }
}

Json Strategy::decide(const Json &p) {
  const auto wait = [&](const char *reason) { reason_ = reason; return Json{{"action", "wait"}}; };
  const auto &self = p.at("self");
  const Tick tick = p.at("tick").get<Tick>();
  const bool alive = self.at("alive").get<bool>();
  const Cell start = absolute_position(self.at("position"));
  const int width = integer(p.at("map").at("width")), height = integer(p.at("map").at("height"));
  const int radius = integer(p.at("vision_radius"));
  if (width <= 0 || height <= 0 || width > 1000000 || height > 1000000 ||
      start.first < 0 || start.first >= width || start.second < 0 || start.second >= height ||
      radius < 0 || radius > 4 || !p.at("visible_entities").is_array()) return wait("invalid_frame");
  if (width_ != 0 && (width != width_ || height != height_)) return wait("map_changed_without_session");
  const bool protected_spawn = self.value("protected", false);
  const double hp = self.value("hp", 100.0);
  if (!std::isfinite(hp) || hp < 0) return wait("invalid_hp");
  const bool respawn = self.value("position_reason", std::string()) == "respawn";
  const auto receipt = self.value("last_action", Json());
  bool moved = false;
  Tick receipt_tick = -1;
  if (receipt.is_object() && receipt.contains("tick") && receipt.at("tick").is_number_integer()) {
    const auto reported = receipt.at("tick").get<Tick>();
    if (reported == tick - 1) {
      receipt_tick = reported;
      moved = receipt.value("moved", false) && receipt.value("action", std::string()) == "move";
    }
  }
  // 坐标永远取感知；回执只确认成功移动和已结算的效果，不用于累加位置。
  history_[tick] = Snapshot{start, alive, protected_spawn, respawn, moved, receipt_tick};
  if (alive && (tick == 0 || self.value("position_reason", std::string()) == "initial")) spawn_ = start;
  update_effects(tick);
  if (!alive) return wait("dead");

  std::vector<Entity> entities;
  std::vector<Enemy> enemies;
  std::set<Cell> occupied;
  for (const auto &j : p.at("visible_entities")) {
    const auto type = j.at("type").get<std::string>();
    if (type != "obstacle" && type != "robot" && type != "resource_normal" && type != "resource_high") continue;
    const auto offset = relative_position(j.at("position"));
    if (!offset) return wait("invalid_entity_grid");
    const auto [dx, dy] = *offset;
    if (std::abs(dx) > radius || std::abs(dy) > radius || dx < -start.first ||
        dx >= width - start.first || dy < -start.second || dy >= height - start.second)
      return wait("invalid_entity_coordinate");
    const Cell cell{start.first + dx, start.second + dy};
    const double value = j.value("value", type == "resource_high" ? 20.0 : 10.0);
    if (!std::isfinite(value) || value < 0) return wait("invalid_resource_value");
    entities.push_back({type, cell, value, value >= 20});
    if (type == "robot") {
      const double enemy_hp = j.value("hp", 100.0);
      if (!std::isfinite(enemy_hp) || enemy_hp < 0) return wait("invalid_enemy_hp");
      occupied.insert(cell);
      enemies.push_back({cell, enemy_hp, j.value("defending", false), j.value("protected", false)});
    }
  }
  width_ = width; height_ = height;
  for (int y = std::max(0, start.second - radius); y <= std::min(height - 1, start.second + radius); ++y) {
    for (int x = std::max(0, start.first - radius); x <= std::min(width - 1, start.first + radius); ++x) {
      auto &tile = tiles_[{x, y}];
      tile.seen = true; tile.wall = false; tile.value = 0; tile.last_seen = tick;
    }
  }
  for (const auto &entity : entities) {
    auto &tile = tiles_.at(entity.position);
    if (entity.type == "obstacle") {
      tile.wall = true;
      if (tile.wall_since < 0) tile.wall_since = tick;
    } else if (entity.type == "resource_normal" || entity.type == "resource_high") {
      tile.value = entity.value; tile.high = entity.high;
    }
  }
  for (auto &[cell, tile] : tiles_) {
    if (!tile.wall) tile.wall_since = -1;
    // 没看到墙消失时只退回未知，不能把临时墙所在格直接认作空地。
    for (const auto &[key, e] : events_) {
      (void)key;
      if (e.kind == Kind::split && tick >= e.end && e.contains(cell) &&
          tile.wall && tile.wall_since >= e.start && tile.last_seen < e.end) {
        tile.wall = false; tile.seen = false; tile.wall_since = -1;
      }
    }
  }
  ++tiles_.at(start).visits;
  const auto move = [&](int step, const char *reason) {
    reason_ = reason;
    return Json{{"action", "move"}, {"direction", directions[reversed_ ? (step + 2) % 4 : step]}};
  };
  bool reflection = false, potential_area_enemy = false;
  double reflection_amount = 0, pre_damage = 0, potential_area_damage = 0;
  for (const auto &[key, e] : events_) {
    (void)key;
    if (e.kind == Kind::reflect && e.active(tick)) {
      reflection = true; reflection_amount = std::max(reflection_amount, e.amount);
    }
    if (e.kind == Kind::fortress && e.active(tick) && tick >= e.start + e.capture_ticks) {
      potential_area_enemy = true;
      potential_area_damage = std::max(potential_area_damage, e.amount);
    }
    if (e.kind == Kind::explosion && e.start == tick && e.contains(start) && !protected_spawn)
      pre_damage += e.amount;
    if (e.kind == Kind::split && e.start == tick && split_wall(start, tick)) return wait("split_forced_wait");
  }
  if (pre_damage >= hp) return wait("lethal_activation_before_action");
  double incoming = 0, defended_incoming = 0;
  for (const auto &enemy : enemies) {
    if (enemy.protected_spawn || protected_spawn) continue;
    const bool area = potential_area_enemy && std::abs(enemy.position.first - start.first) <= 2 &&
                      std::abs(enemy.position.second - start.second) <= 2;
    if (area || ray(enemy.position, start) != -1) {
      incoming += (area ? std::max(20.0, potential_area_damage) : 20) * vulnerability_;
      defended_incoming += reflection ? 0 : 10 * vulnerability_;
    }
  }
  // MOVE 无法躲开本 Tick 已经先结算的战斗；防御可能保命时先防御。
  if (stage_ == 2 && incoming >= hp - pre_damage && defended_incoming < hp - pre_damage) {
    reason_ = "defend_lethal_threat";
    return {{"action", "defend"}};
  }

  // 对手下一步可能踩辣椒，属于保守估计；不把其未知动作当作事实。
  const auto possible_fire = [&](Cell cell) {
    double damage = 0;
    if (protected_spawn && cell == start) return damage;
    for (const auto &[key, e] : events_) {
      (void)key;
      if (e.kind != Kind::chili || !e.active(tick)) continue;
      for (const auto &point : e.points) {
        if (cell == point || (cell.first != point.first && cell.second != point.second)) continue;
        if (std::any_of(enemies.begin(), enemies.end(), [&](const Enemy &enemy) {
              return manhattan(enemy.position, point) == 1;
            })) damage += e.amount;
      }
    }
    return damage;
  };

  struct Node { int first, distance; };
  std::map<Cell, Node> paths;
  std::vector<Cell> order;
  std::queue<Cell> queue;
  paths[start] = {-1, 0}; queue.push(start);
  while (!queue.empty()) {
    const Cell current = queue.front(); queue.pop(); order.push_back(current);
    const auto node = paths.at(current);
    for (int i = 0; i < 4; ++i) {
      const Cell next{current.first + steps[i].first, current.second + steps[i].second};
      const auto tile = tiles_.find(next);
      if (tile == tiles_.end() || !tile->second.seen || tile->second.wall || occupied.count(next) ||
          paths.count(next) || split_wall(next, tick + node.distance) ||
          (node.distance == 0 && possible_fire(next) >= hp - pre_damage) ||
          dangerous(next, tick + node.distance + 1)) continue;
      paths[next] = {node.first == -1 ? i : node.first, node.distance + 1};
      queue.push(next);
    }
  }
  if (stage_ == 2 && possible_fire(start) >= hp - pre_damage && !reverse_unknown_) {
    for (const auto &cell : order) {
      const auto node = paths.at(cell);
      if (node.distance == 1 && possible_fire(cell) < hp - pre_damage)
        return move(node.first, "avoid_possible_chili_fire");
    }
  }
  // 即将到期的危险先退出；本 Tick 才激活的爆炸不能靠移动补救。
  for (const auto &[key, e] : events_) {
    (void)key;
    if ((e.kind != Kind::explosion && e.kind != Kind::split) || e.start <= tick || !e.contains(start)) continue;
    if (e.kind == Kind::split && spawn_ && start == *spawn_) continue;
    int nearest_exit = std::numeric_limits<int>::max(), escape = -1;
    for (const auto &cell : order) {
      const auto node = paths.at(cell);
      if (!e.contains(cell) && node.distance < nearest_exit) { nearest_exit = node.distance; escape = node.first; }
    }
    if (e.start - tick <= nearest_exit + 2LL && escape >= 0 && !reverse_unknown_)
      return move(escape, "escape_announced_hazard");
  }

  std::set<std::string> completed;
  const auto score = p.value("score", Json::object());
  if (score.contains("mission_completed_ids")) {
    for (const auto &id : score.at("mission_completed_ids")) completed.insert(id.get<std::string>());
  }
  const auto missions = p.value("missions", Json::array());
  bool need_normal = false, need_high = false, need_damage = false, need_value = false;
  std::vector<Cell> reach;
  for (const auto &mission : missions) {
    if (completed.count(mission.at("id").get<std::string>())) continue;
    const auto type = mission.at("type").get<std::string>();
    need_normal = need_normal || type == "normal_resources";
    need_high = need_high || type == "high_resources";
    need_damage = need_damage || type == "effective_damage";
    need_value = need_value || type == "resource_value";
    if (type == "reach") {
      const Cell target = absolute_position(mission.at("position"));
      if (target.first >= 0 && target.first < width && target.second >= 0 && target.second < height)
        reach.push_back(target);
    }
  }
  int best_step = -1;
  double best_utility = -1;
  const char *best_reason = "explore";
  const auto consider = [&](int step, double utility, const char *reason) {
    if (utility > best_utility + 1e-9) { best_utility = utility; best_step = step; best_reason = reason; }
  };
  unsigned least_visits = std::numeric_limits<unsigned>::max();
  int exploration = -1;
  for (const auto &cell : order) {
    const auto node = paths.at(cell);
    const auto &tile = tiles_.at(cell);
    if (node.first >= 0 && tile.visits < least_visits) { least_visits = tile.visits; exploration = node.first; }
    if (tile.value > 0) {
      const double freshness = 1.0 / (1.0 + std::max<Tick>(0, tick - tile.last_seen) * 0.01);
      double benefit = tile.value;
      if (stage_ == 2) {
        benefit = tile.value * (score.value("resource_value", 0.0) < 120 ? 0.25 : 0.02);
        if ((tile.high && need_high) || (!tile.high && need_normal)) benefit += 5;
        if (need_value) benefit += tile.value * 0.1;
      }
      consider(node.first, benefit * freshness / (node.distance + 1), "collect_resource");
    }
    if (stage_ == 1) continue;
    for (const auto &target : reach) {
      if (cell == target) consider(node.first, 8.0 / (node.distance + 1), "reach_mission");
      else if (node.first >= 0) {
        // 目标尚未探明时只走已知路线中靠近目标的格子，不穿过未知墙。
        int unknown_neighbors = 0;
        for (const auto &delta : steps) {
          const Cell next{cell.first + delta.first, cell.second + delta.second};
          if (next.first >= 0 && next.first < width && next.second >= 0 && next.second < height &&
              !tiles_.count(next)) ++unknown_neighbors;
        }
        if (unknown_neighbors && manhattan(cell, target) < manhattan(start, target))
          consider(node.first, 2.0 / (node.distance + 1) + 1.0 / (manhattan(cell, target) + 1), "explore_toward_mission");
      }
    }
    for (const auto &[key, e] : events_) {
      if (!e.active(tick)) continue;
      if (e.kind == Kind::heal && hp < 70 && healing_until_ <= tick + 2 && e.contains(cell) && cell != start &&
          tick + node.distance - 1 < e.end)
        consider(node.first, std::min(100.0 - hp, e.amount * e.effect_ticks) * 0.2 / (node.distance + 1), "seek_healing");
      if (e.kind == Kind::fortress && !empowered_ && e.contains(cell) &&
          e.end - tick >= node.distance + e.capture_ticks - capture_progress_[key] - (node.distance > 0 ? 1 : 0)) {
        if (cell == start) consider(-1, 20.0, "hold_fortress");
        else consider(node.first, 12.0 / (node.distance + e.capture_ticks), "capture_fortress");
      }
      if (e.kind == Kind::chili && (e.contains(cell) || (e.contains(start) && node.distance == 1))) {
        const Cell fire_point = e.contains(start) ? start : cell;
        double reward = 0;
        for (const auto &enemy : enemies) {
          if (!enemy.protected_spawn && enemy.position != fire_point &&
              (enemy.position.first == fire_point.first || enemy.position.second == fire_point.second))
            reward += enemy.hp <= e.amount ? 3.0 : 0.3;
        }
        if (reward > 0 && cell != start)
          consider(node.first, reward / (node.distance + (e.contains(start) ? 2 : 1)), "trigger_chili");
      }
    }
    for (const auto &enemy : enemies) {
      if (!enemy.protected_spawn && node.first >= 0 && ray(cell, enemy.position) >= 0)
        consider(node.first, (need_damage ? 4.0 : 0.4) / (node.distance + 1), "approach_enemy");
    }
  }
  if (best_utility < 0 && exploration >= 0) consider(exploration, 0.01, "explore");

  int attack_direction = -1;
  double attack_utility = -1;
  if (stage_ == 2 && !protected_spawn) {
    for (int direction = 0; direction < 4; ++direction) {
      std::vector<const Enemy *> targets;
      if (empowered_) {
        for (const auto &enemy : enemies)
          if (std::abs(enemy.position.first - start.first) <= 2 && std::abs(enemy.position.second - start.second) <= 2)
            targets.push_back(&enemy);
      } else {
        const Enemy *nearest = nullptr;
        for (const auto &enemy : enemies)
          if (ray(start, enemy.position) == direction && (!nearest ||
              manhattan(start, enemy.position) < manhattan(start, nearest->position))) nearest = &enemy;
        if (nearest) targets.push_back(nearest);  // 近处保护敌车仍阻挡后方，障碍不挡攻击。
      }
      double utility = 0, reflected = 0;
      for (const auto *enemy : targets) {
        if (enemy->protected_spawn) continue;
        if (reflection && enemy->defending) { reflected += reflection_amount; continue; }
        const double damage = enemy->defending ? defended_damage_ : attack_damage_;
        utility += std::min(damage, enemy->hp) * 0.03 + (enemy->hp <= damage ? 3.0 : 0.0);
        if (need_damage) utility += 0.5;
      }
      utility -= reflected * 0.05;
      if (reflected + pre_damage >= hp) continue;
      if (utility > attack_utility) { attack_utility = utility; attack_direction = direction; }
    }
  }
  // 留在堡垒仍可攻击/防御；不能把上一帧 defending 当成对手本 Tick 的承诺。
  if (attack_direction >= 0 && attack_utility > 0 &&
      (attack_utility > best_utility || std::string(best_reason) == "hold_fortress")) {
    reason_ = "attack_opportunity";
    return {{"action", "attack"}, {"direction", directions[attack_direction]}};
  }
  if (best_step >= 0 && !reverse_unknown_) return move(best_step, best_reason);
  if (stage_ == 2 && incoming > 0) { reason_ = "defend_while_holding"; return {{"action", "defend"}}; }
  return wait(reverse_unknown_ ? "uncertain_illusion" : best_utility >= 0 ? best_reason : "no_path");
}
