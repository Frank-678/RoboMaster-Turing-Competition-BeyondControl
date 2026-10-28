#pragma once
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "rmtc_bot/events.hpp"
#include "rmtc_bot/strategy_parameters.hpp"

class Strategy {
public:
  explicit Strategy(int stage = 2); // stage：比赛阶段；实际入口明确传入配置中的1或2。
  Strategy(int stage, rmtc::StrategyParameters parameters); // 仅离线评测注入；正式构造仍使用封存常量。
  // 只在换场调用。普通死亡不会清掉地图和仍有效的增益。
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
  struct OpponentTrack {
    Cell position, delta; // 上次真实位置、最近一次世界方向位移；不预测隐藏坐标。
    Tick tick; // 最近一次连续可见观测的Tick。
    unsigned consistent; // 相同合法位移连续出现的次数，上限2；2次才提出一步预测。
  };
  struct Goal {
    Cell position; // 上一Tick选择的真实目标坐标；不是预测后的本车坐标。
    std::string kind; // 资源、任务、治疗等目标类别，避免同坐标不同用途被误认。
    Tick tick; // 做出这次选择的Tick；断帧、复活和换场后不沿用迟滞。
  };
  struct ResourcePlan {
    int first; // 本Tick世界方向，最后发送时才处理幻术反转。
    Cell first_resource; // 路上最先拿到的资源，也是目标迟滞使用的坐标。
    double utility, benefit; // 整串收益/耗时、整串收益；均为估值，不回写官方成绩。
  };
  // 在已知格上比较最多三个资源目标；每步核对公告期限和剩余Tick，只返回各首方向的最好资源串。
  std::vector<ResourcePlan> resource_plans(Cell start, Tick tick, Tick remaining_ticks,
      const std::vector<Cell> &targets, const std::set<Cell> &occupied,
      const std::array<bool, 4> &first_allowed, double current_r,
      bool need_normal, bool need_high, double value_target) const;
  std::optional<Cell> predict_opponent(const std::string &identity, Cell position, Tick tick);
  // 预测仅用同一敌车连续观测；断帧/瞬移重置，预测只降低候选收益，不当成必然占位。
  bool split_wall(Cell cell, Tick tick) const;
  bool dangerous(Cell cell, Tick tick) const;
  void update_effects(Tick tick);
  int stage_, width_ = 0, height_ = 0; // 比赛阶段、本场地图宽高；0表示尚未收到地图。
  rmtc::StrategyParameters parameters_; // 本对象固定取舍；一场中不修改。
  bool reversed_ = false, reverse_unknown_ = false, empowered_ = false; // 幻术反转、资格未知、已获无双。
  // 不知道幻术激活时的位置时，reverse_unknown_=true，避免猜反方向。
  double vulnerability_ = 1, attack_damage_ = 20, defended_damage_ = 10; // 本车易伤倍率、攻击/对防御者伤害。
  Tick healing_until_ = -1; // 已知回血效果的绝对结束Tick。
  std::optional<Cell> spawn_; // 已确认的初始出生点；未知时为空。
  std::map<Cell, Tile> tiles_; // 格子坐标→地图记忆。
  std::map<Cell, unsigned> probe_attempts_;  // 未知格的试探次数；不算已到达或已探明。
  std::map<Tick, Snapshot> history_; // 感知Tick→本车历史快照。
  std::map<std::string, OpponentTrack> opponents_; // 本场公开敌车ID→短期轨迹，换场清空。
  std::optional<Goal> goal_; // 只保留目标，绝不缓存一串必须执行的未来动作。
  std::map<std::string, rmtc::Event> events_; // 稳定事件key→最后一次校验通过的解释。
  std::map<std::string, int> capture_progress_; // 堡垒key→当前已确认连续驻留Tick数。
  std::string reason_; // 决策日志使用的短原因。
};

