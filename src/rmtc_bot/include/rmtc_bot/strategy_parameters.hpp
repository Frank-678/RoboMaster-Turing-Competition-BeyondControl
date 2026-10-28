#pragma once
#include <cmath>
#include <stdexcept>
#include "rmtc_bot/match_config.hpp"

namespace rmtc {
// 正式入口只取编译期配置；第二个Strategy构造入口只供离线评测注入参数，不提供ROS热更新。
struct StrategyParameters {
  double stale_resource_decay, resource_mission_bonus, reach_mission_value, healing_below_hp;
  double fortress_hold_value, fortress_capture_value, goal_retention_bonus, frontier_value;
  double opponent_contest_cost, opponent_pressure_cost, opponent_claim_discount;
  double explore_toward_resource_value; // 向绕行过远/未连通的已知资源试探边界格的机会权重。
  double explore_tour_value; // 第一阶段空档期，把散布的边界观察点排成小巡回的机会权重。
  double mission_explore_value; // 沿到点任务方向探路的权重（除以距离+1）；到点任务没有截止时间，不应压过近处扫图。
  double stage1_flood_value; // 第一阶段空档探索的最近边界洪泛权重（距离优先、同距离取更大新视野）；与散点巡回、frontier同场竞争，0为关闭。
  bool stage2_race_gate; // 第二阶段R未封顶且还有已知可收集资源时，把主动交火与到点任务让位给采集竞速。
  int hazard_lead_ticks, resource_plan_depth, resource_beam_width, resource_candidate_limit, resource_plan_budget_ms;
  bool collision_replan, resource_refinement;
  int resource_refinement_trials;
  bool tactical_search;
  double tactical_risk, tactical_hp_value, tactical_death_cost, tactical_future_discount, tactical_goal_weight;
  bool loop_recovery; // 离线可关闭作对照；正式仍由唯一配置封存。
  int loop_window_ticks, recovery_ticks, stalled_goal_cooldown_ticks; // 检测窗口、脱困期限、坏目标冷却。
  static StrategyParameters competition() {
    namespace c=match_config;
    return {c::stale_resource_decay,c::resource_mission_bonus,c::reach_mission_value,c::healing_below_hp,
      c::fortress_hold_value,c::fortress_capture_value,c::goal_retention_bonus,c::frontier_value,
      c::opponent_contest_cost,c::opponent_pressure_cost,c::opponent_claim_discount,c::explore_toward_resource_value,c::explore_tour_value,c::mission_explore_value,
      c::stage1_flood_value,c::stage2_race_gate,c::hazard_lead_ticks,c::resource_plan_depth,
      c::resource_beam_width,c::resource_candidate_limit,c::resource_plan_budget_ms,
      c::collision_replan,c::resource_refinement,c::resource_refinement_trials,c::tactical_search,
      c::tactical_risk,c::tactical_hp_value,c::tactical_death_cost,c::tactical_future_discount,c::tactical_goal_weight,
      c::loop_recovery,c::loop_window_ticks,c::recovery_ticks,c::stalled_goal_cooldown_ticks};
  }
  void validate() const {
    for (double value:{stale_resource_decay,resource_mission_bonus,reach_mission_value,healing_below_hp,
         fortress_hold_value,fortress_capture_value,goal_retention_bonus,frontier_value,opponent_contest_cost,opponent_pressure_cost,
         opponent_claim_discount,
         explore_toward_resource_value,explore_tour_value,mission_explore_value,stage1_flood_value,tactical_risk,tactical_hp_value,tactical_death_cost,tactical_future_discount,tactical_goal_weight})
      if (!std::isfinite(value) || value<0 || value>1000) throw std::runtime_error("invalid strategy weight");
    if (healing_below_hp>100 || tactical_risk>1 || tactical_future_discount>1 || hazard_lead_ticks<0 || hazard_lead_ticks>20 || resource_plan_depth<1 || resource_plan_depth>8 ||
        resource_beam_width<1 || resource_beam_width>32 || resource_candidate_limit<1 || resource_candidate_limit>32 ||
        resource_refinement_trials<0 || resource_refinement_trials>32 || resource_plan_budget_ms<1 || resource_plan_budget_ms>30 || loop_window_ticks<6 || loop_window_ticks>32 ||
        recovery_ticks<2 || recovery_ticks>64 || stalled_goal_cooldown_ticks<1 || stalled_goal_cooldown_ticks>200) throw std::runtime_error("invalid strategy budget");
  }
};
} // namespace rmtc
