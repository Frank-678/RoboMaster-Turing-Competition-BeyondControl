#pragma once
#include <cmath>
#include <stdexcept>
#include "rmtc_bot/match_config.hpp"

namespace rmtc {
// 正式入口只取编译期配置；第二个Strategy构造入口只供离线评测注入参数，不提供ROS热更新。
struct StrategyParameters {
  double stale_resource_decay, resource_mission_bonus, reach_mission_value, healing_below_hp;
  double fortress_hold_value, fortress_capture_value, goal_retention_bonus, frontier_value;
  double opponent_contest_cost, opponent_pressure_cost;
  int hazard_lead_ticks, resource_plan_depth, resource_beam_width, resource_candidate_limit, resource_plan_budget_ms;
  static StrategyParameters competition() {
    namespace c=match_config;
    return {c::stale_resource_decay,c::resource_mission_bonus,c::reach_mission_value,c::healing_below_hp,
      c::fortress_hold_value,c::fortress_capture_value,c::goal_retention_bonus,c::frontier_value,
      c::opponent_contest_cost,c::opponent_pressure_cost,c::hazard_lead_ticks,c::resource_plan_depth,
      c::resource_beam_width,c::resource_candidate_limit,c::resource_plan_budget_ms};
  }
  void validate() const {
    for (double value:{stale_resource_decay,resource_mission_bonus,reach_mission_value,healing_below_hp,
         fortress_hold_value,fortress_capture_value,goal_retention_bonus,frontier_value,opponent_contest_cost,opponent_pressure_cost})
      if (!std::isfinite(value) || value<0 || value>1000) throw std::runtime_error("invalid strategy weight");
    if (healing_below_hp>100 || hazard_lead_ticks<0 || hazard_lead_ticks>20 || resource_plan_depth<1 || resource_plan_depth>8 ||
        resource_beam_width<1 || resource_beam_width>32 || resource_candidate_limit<1 || resource_candidate_limit>32 ||
        resource_plan_budget_ms<1 || resource_plan_budget_ms>30) throw std::runtime_error("invalid strategy budget");
  }
};
} // namespace rmtc
