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

// 主线：读真实坐标 → 更新记忆/事件 → 避险 → 比较资源、任务、战斗 → 返回本Tick一个动作。
// 不在这里收发ROS或等待模型；Session保证同一Tick只推进一次。
class Strategy {
public:
  explicit Strategy(int stage = 2);
  // 只在换场调用。普通死亡不会清掉地图和仍有效的增益。
  void reset();

  // 只返回action/direction，场次和Tick由通信层原样回填。
  // null表示本Tick不发布；正常第二阶段的兜底动作是DEFEND。
  nlohmann::json decide(const nlohmann::json &perception);
  void merge_events(const std::vector<rmtc::Event> &events);
  // 把已校验事件交给下一次LLM请求，避免模型反复“重新发现”同一个事件。
  nlohmann::json known_events() const;
  const std::string &reason() const { return reason_; }

private:
  using Cell = rmtc::Cell;
  using Tick = rmtc::Tick;
  struct Tile {
    // seen=false是未知；wall=false不代表已经确认可以走。
    bool seen = false, wall = false;
    Tick last_seen = -1, wall_since = -1;
    // visits只算感知确认到过的格子；发出MOVE不增加目的地访问次数。
    unsigned visits = 0;
    double value = 0;
    bool high = false;
  };
  struct Snapshot {
    // position属于当前感知；moved/receipt_tick解释上一Tick的实际执行结果。
    Cell position;
    bool alive, protected_spawn, respawn, moved;
    Tick receipt_tick;
  };
  bool split_wall(Cell cell, Tick tick) const;
  // 只判断已知爆炸和分裂，不把所有对手预测混成确定危险。
  bool dangerous(Cell cell, Tick tick) const;
  std::array<bool, 5> safe_first_steps(Cell start, Tick tick, const std::set<Cell> &occupied) const;
  // 从实际历史重建本车效果：幻术看激活位置，易伤/回血看成功移动，堡垒看连续驻留。
  void update_effects(Tick tick);
  int stage_, width_ = 0, height_ = 0;
  bool reversed_ = false, reverse_unknown_ = false, empowered_ = false;
  // 不知道幻术激活时的位置时，reverse_unknown_=true，避免猜反方向。
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
