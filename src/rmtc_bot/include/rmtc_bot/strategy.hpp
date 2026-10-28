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
#include "rmtc_bot/strategy_parameters.hpp"

// 主线：读真实坐标 → 更新记忆/事件 → 避险 → 比较资源、任务、战斗 → 返回本Tick一个动作。
// 不在这里收发ROS或等待模型；Session保证同一Tick只推进一次。
class Strategy {
public:
  explicit Strategy(int stage = 2); // stage：比赛阶段；实际入口明确传入配置中的1或2。
  Strategy(int stage, rmtc::StrategyParameters parameters); // 仅离线评测注入；正式构造仍使用封存常量。
  // 只在换场调用。普通死亡不会清掉地图和仍有效的增益。
  void reset();

  // 只返回action/direction，场次和Tick由通信层原样回填。
  // null表示本Tick不发布；正常第二阶段的兜底动作是DEFEND。
  nlohmann::json decide(const nlohmann::json &perception);
  void merge_events(const std::vector<rmtc::Event> &events);
  // 把已校验事件交给下一次LLM请求，避免模型反复“重新发现”同一个事件。
  nlohmann::json known_events() const;
  const std::string &reason() const { return reason_; } // 最近动作的原因，供日志解释。
  void note_model_reply() { model_reply_seen_ = true; } // 本场已见非mock模型正文；仅影响收益假设，不冒充裁判资格。

private:
  using Cell = rmtc::Cell; // 一个绝对格子[x,y]，不是相对方向。
  using Tick = rmtc::Tick; // 裁判逻辑回合，不能拿墙钟时间代替。
  struct Tile {
    // seen=false是未知；wall=false不代表已经确认可以走。
    bool seen = false, wall = false; // 是否已探明、最后是否看见墙。
    Tick last_seen = -1, wall_since = -1; // 最后观测Tick、首次连续看见此墙的Tick。
    // visits只算感知确认到过的格子；发出MOVE不增加目的地访问次数。
    unsigned visits = 0; // 实际访问次数。
    double value = 0;    // 最后观测到的资源价值；0表示没有资源记录。
    bool high = false;  // 该资源是否属于高价值资源。
  };
  struct Snapshot {
    // position属于当前感知；moved/receipt_tick解释上一Tick的实际执行结果。
    Cell position; // 此帧裁判确认的本车位置。
    bool alive, protected_spawn, respawn, moved; // 存活、出生保护、刚复活、上Tick成功移动。
    Tick receipt_tick; // 本帧last_action对应的执行Tick；-1表示缺少可信回执。
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
    std::vector<Cell> stops; // 尚未执行的资源目标串，每帧重新验证，不是待发送动作队列。
  };
  // 在已知格上比较最多三个资源目标；每步核对公告期限和剩余Tick，只返回各首方向的最好资源串。
  std::vector<ResourcePlan> resource_plans(Cell start, Tick tick, Tick remaining_ticks,
      const std::vector<Cell> &targets, const std::set<Cell> &occupied,
      const std::array<bool, 4> &first_allowed, double current_r,
      bool need_normal, bool need_high, double value_target, const std::vector<Cell> &previous_plan) const;
  std::optional<Cell> predict_opponent(const std::string &identity, Cell position, Tick tick);
  // 预测仅用同一敌车连续观测；断帧/瞬移重置，预测只降低候选收益，不当成必然占位。
  bool split_wall(Cell cell, Tick tick) const;
  // 只判断已知爆炸和分裂，不把所有对手预测混成确定危险。
  bool dangerous(Cell cell, Tick tick) const;
  // start为当前位置，tick为当前回合，occupied为当前看见的敌车占位。
  // 返回值依次是上/右/下/左/原地，表示第一步是否存在安全接续。
  std::array<bool, 5> safe_first_steps(Cell start, Tick tick, const std::set<Cell> &occupied) const;
  // 从实际历史重建本车效果：幻术看激活位置，易伤/回血看成功移动，堡垒看连续驻留。
  void update_effects(Tick tick);
  int stage_, width_ = 0, height_ = 0; // 比赛阶段、本场地图宽高；0表示尚未收到地图。
  rmtc::StrategyParameters parameters_; // 本对象固定取舍；一场中不修改。
  bool reversed_ = false, reverse_unknown_ = false, empowered_ = false; // 幻术反转、资格未知、已获无双。
  // 不知道幻术激活时的位置时，reverse_unknown_=true，避免猜反方向。
  double vulnerability_ = 1, attack_damage_ = 20, defended_damage_ = 10; // 本车易伤倍率、攻击/对防御者伤害。
  Tick healing_until_ = -1; // 已知回血效果的绝对结束Tick。
  double healing_amount_ = 0; // 与最近有效入点记录对应的每Tick回血量，不固定猜10HP。
  std::optional<Cell> spawn_; // 已确认的初始出生点；未知时为空。
  std::map<Cell, Tile> tiles_; // 格子坐标→地图记忆。
  std::map<Cell, unsigned> probe_attempts_;  // 未知格的试探次数；不算已到达或已探明。
  std::map<Tick, Snapshot> history_; // 感知Tick→本车历史快照。
  std::map<std::string, OpponentTrack> opponents_; // 本场公开敌车ID→短期轨迹，换场清空。
  std::optional<Goal> goal_; // 只保留目标，绝不缓存一串必须执行的未来动作。
  std::vector<Cell> planned_resources_; // 仅供下一帧重评的资源串；换场、断帧、失败和改目标后清空。
  bool model_reply_seen_ = false, resource_counts_known_ = false;
  int observed_normal_ = 0, observed_high_ = 0; // 连续完整历史下，从官方R增量确认的拾取次数。
  Tick previous_score_tick_ = -1;
  double previous_resource_total_ = 0;
  std::map<std::string, rmtc::Event> events_; // 稳定事件key→最后一次校验通过的解释。
  std::map<std::string, int> capture_progress_; // 堡垒key→当前已确认连续驻留Tick数。
  std::string reason_; // 决策日志使用的短原因。
};

