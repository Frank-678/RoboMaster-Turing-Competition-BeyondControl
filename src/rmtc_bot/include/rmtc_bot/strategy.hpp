#pragma once
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "rmtc_bot/events.hpp"

class Strategy {
public:
  explicit Strategy(int stage = 2);
  // Called once on the first frame of each new session.
  void reset();

  // Called once per new tick. Return action and, if needed, direction.
  // The communication layer supplies tick and session_id.
  nlohmann::json decide(const nlohmann::json &perception);
  void merge_events(const std::vector<rmtc::Event> &events);
  nlohmann::json known_events() const;
  const std::string &reason() const { return reason_; }

private:
  using Cell = rmtc::Cell;
  using Tick = rmtc::Tick;
  struct Tile {
    bool seen = false, wall = false;
    Tick last_seen = -1, wall_since = -1;
    unsigned visits = 0;
    double value = 0;
    bool high = false;
  };
  struct Snapshot {
    Cell position;
    bool alive, protected_spawn, respawn, moved;
    Tick receipt_tick;
  };
  bool split_wall(Cell cell, Tick tick) const;
  bool dangerous(Cell cell, Tick tick) const;
  std::array<bool, 5> safe_first_steps(Cell start, Tick tick, const std::set<Cell> &occupied) const;
  void update_effects(Tick tick);
  int stage_, width_ = 0, height_ = 0;
  bool reversed_ = false, reverse_unknown_ = false, empowered_ = false;
  double vulnerability_ = 1, attack_damage_ = 20, defended_damage_ = 10;
  Tick healing_until_ = -1;
  std::optional<Cell> spawn_;
  std::map<Cell, Tile> tiles_;
  std::map<Cell, unsigned> probe_attempts_;  // 未知格的试探次数；不算已到达或已探明。
  std::map<Tick, Snapshot> history_;
  std::map<std::string, rmtc::Event> events_;
  std::map<std::string, int> capture_progress_;
  std::string reason_;
};
