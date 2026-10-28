#pragma once
#include <array>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include "rmtc_bot/rules.hpp"
#include "rmtc_bot/strategy_parameters.hpp"

namespace rmtc {
struct TacticalMission { // 一个尚未完成的公开任务；完成标记只留在本次推演副本。
  std::string type; // 规则规定的任务类型。
  double target; // 次数、资源价值或有效伤害门槛。
  Cell position; // 仅reach任务使用的绝对目标格。
};
struct TacticalInput {
  Tick tick,remaining_ticks; // 当前回合、含本回合在内的剩余动作数。
  int width,height; // 感知给出的地图边界。
  std::array<rules::Fighter,4> fighters; // 0永远是本车，其余只来自本帧可见敌车。
  std::array<std::optional<Cell>,4> forecast; // 连续轨迹猜测，不是确定的未来坐标。
  std::set<Cell> walkable; // 只含记忆确认可走的格子，不补齐未知地图。
  std::map<Cell,double> resources; // 只含本帧确认的资源；旧记忆仅用于战略目标估值。
  std::vector<Event> events; // 仅本场已经LLM校验的事件；不带未公告安排。
  std::vector<TacticalMission> missions; // 仅尚未完成项。
  std::array<double,5> goal_value; // 上右下左/驻留的既有目标评价。
  std::array<std::optional<Cell>,5> goal_position; // 用预测末位置衡量真实推进，不能走一步再退回仍领目标价值。
  std::array<bool,5> safe_first; // 已知危险的首步筛选；第五项为留在原地。
  double resource_total,damage_total; // 裁判当前R/D，收益以此为基数计算封顶分差。
  int normal_count,high_count; // 连续官方R增量确认的普通/高价值拾取次数。
  bool counts_known,model_reply_seen,possible_enemy_area; // 次数可信、见过模型正文、敌车可能获无双。
  double possible_area_damage; // 已知堡垒公告的无双伤害，只进入额外不确定情景。
  Tick healing_until; // 本车已确认回血效果的绝对结束Tick。
  double healing_amount; // 每Tick实际回血量。
  std::optional<Cell> spawn; // 仅本车已确认初始出生点。
  std::map<std::string,int> capture_progress; // 事件key对应的已确认堡垒驻留次数。
};
struct TacticalChoice {
  rules::Action action; // 只执行首动作，后续推演不会直接发布。
  double value; // 评分分差与机会成本的合计估值，不回写官方成绩。
  int scenarios,simulated_steps; // 对手情景数、实际推进的模拟状态数。
};
// 最多三Tick短搜索：每层保留5个不同首动作方案，所有对手情景共用同一续步，只发布首步。
TacticalChoice tactical_choice(const TacticalInput &input,const StrategyParameters &parameters);
} // namespace rmtc
