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
  bool collision_replan, resource_refinement;
  int resource_refinement_trials;
  bool tactical_search;
  double tactical_risk, tactical_hp_value, tactical_death_cost, tactical_future_discount, tactical_goal_weight;
  static StrategyParameters competition() {
    namespace c=match_config;
    return {c::stale_resource_decay,c::resource_mission_bonus,c::reach_mission_value,c::healing_below_hp,
      c::fortress_hold_value,c::fortress_capture_value,c::goal_retention_bonus,c::frontier_value,
      c::opponent_contest_cost,c::opponent_pressure_cost,c::hazard_lead_ticks,c::resource_plan_depth,
      c::resource_beam_width,c::resource_candidate_limit,c::resource_plan_budget_ms,
      c::collision_replan,c::resource_refinement,c::resource_refinement_trials,c::tactical_search,
      c::tactical_risk,c::tactical_hp_value,c::tactical_death_cost,c::tactical_future_discount,c::tactical_goal_weight};
  }
  void validate() const {
    for (double value:{stale_resource_decay,resource_mission_bonus,reach_mission_value,healing_below_hp,
         fortress_hold_value,fortress_capture_value,goal_retention_bonus,frontier_value,opponent_contest_cost,opponent_pressure_cost,
         tactical_risk,tactical_hp_value,tactical_death_cost,tactical_future_discount,tactical_goal_weight})
      if (!std::isfinite(value) || value<0 || value>1000) throw std::runtime_error("invalid strategy weight");
    if (healing_below_hp>100 || tactical_risk>1 || tactical_future_discount>1 || hazard_lead_ticks<0 || hazard_lead_ticks>20 || resource_plan_depth<1 || resource_plan_depth>8 ||
        resource_beam_width<1 || resource_beam_width>32 || resource_candidate_limit<1 || resource_candidate_limit>32 ||
        resource_refinement_trials<0 || resource_refinement_trials>32 || resource_plan_budget_ms<1 || resource_plan_budget_ms>30) throw std::runtime_error("invalid strategy budget");
  }
};
} // namespace rmtc
