#pragma once
#include <cmath>
#include <stdexcept>
#include "rmtc_bot/match_config.hpp"

namespace rmtc {
// 正式入口只取编译期配置；第二个Strategy构造入口只供离线评测注入参数，不提供ROS热更新。
struct StrategyParameters {
  double stale_resource_decay, resource_mission_bonus, reach_mission_value, healing_below_hp;
  double fortress_hold_value, fortress_capture_value, prepare_fortress_value, goal_retention_bonus, frontier_value;
  int stale_grace_ticks; // 资源离开视野后前N个Tick不衰减（未观测≠消失）；0=离视野当Tick即开始衰减。
  double opponent_contest_cost, opponent_pressure_cost, opponent_claim_discount;
  double stage1_frontier_value; // 第一阶段边界信息权重；与frontier_value分离。
  double explore_toward_resource_value; // 向绕行过远/未连通的已知资源试探边界格的机会权重。
  double explore_tour_value; // 第一阶段空档期，把散布的边界观察点排成小巡回的机会权重。
  double mission_explore_value; // 沿到点任务方向探路的权重（除以距离+1）；到点任务没有截止时间，不应压过近处扫图。
  double stage1_flood_value; // 第一阶段空档探索的最近边界洪泛权重（距离优先、同距离取更大新视野）；与散点巡回、frontier同场竞争，0为关闭。
  double stage1_ring_value; // 第一阶段近出生点完成覆盖权重：巡回观察点按增益+近出生点加成排序；0为关闭。
  bool stage2_race_gate; // 第二阶段R未封顶且还有已知可收集资源时，把主动交火与到点任务让位给采集竞速。
  double race_reach_holdoff; // 第二阶段已知可达资源未清空时对到点任务目标的折扣（0-1）；0=旧行为。
  int stage2_explore_gate_tick; // 第二阶段资源饼清空后关闭扫图的时刻闸门；0为关闭。
  int stage2_mission_pull_stale_ticks; // 第二阶段资源久未现身时向到点任务方向拉拽探索的饥饿阈值；0为关闭。
  double stage2_assassin_value; // 第二阶段派窗口刺杀：攻击正在扫资源簇的对手（击杀/击退→重生移位）的归属收益权重；0为关闭。
  double stage2_pie_attack_discount; // 第二阶段饼窗口攻击估值折扣（0-1）；还手不受折扣。
  double stage2_shadow_value; // 第二阶段饼窗口影子跟踪权重：对手朝未知区移动时向投影前端探路；0为关闭。
  int defend_stall_break_ticks; // 第二阶段连续致命防御驻守超过该tick数后脱离接触；0为关闭。
  int hazard_lead_ticks, resource_plan_depth, resource_beam_width, resource_candidate_limit, resource_plan_budget_ms;
  bool collision_replan, resource_refinement;
  int resource_refinement_trials;
  bool tactical_search;
  double tactical_risk, tactical_hp_value, tactical_death_cost, tactical_future_discount, tactical_goal_weight;
  bool loop_recovery; // 离线可关闭作对照；正式仍由唯一配置封存。
  int loop_window_ticks, recovery_ticks, stalled_goal_cooldown_ticks; // 检测窗口、脱困期限、坏目标冷却。
  bool worst_case_survival; // 1=生存判定按全敌攻击最坏情形（预测防御只作机会估值，不作伤害上界的确定排除项）；0=旧行为。
  double post_cap_combat_weight; // D≥500封顶后增量D的平局估值权重（同分同任务同R按D决胜）；0=旧行为（封顶后归零）。
  double explore_component_value; // 第二阶段扫图观察点覆盖最大化加权：按视野触及的最大未探索连通区大小加成；0=旧行为。
  double low_hp_attack_discount; // 低血量攻击折扣：hp<60且未被火力波及时攻击估值乘此值（策略层§6+战术层深度0攻击分支同口径）；1.0=旧行为。
  double explore_probe_mission_bonus_scale; // 资源探针的任务加成缩放；1.0=旧行为（探针与采集同额计入任务加成）。
  static StrategyParameters competition() {
    namespace c=match_config;
    return {c::stale_resource_decay,c::resource_mission_bonus,c::reach_mission_value,c::healing_below_hp,
      c::fortress_hold_value,c::fortress_capture_value,c::prepare_fortress_value,c::goal_retention_bonus,c::frontier_value,
      c::stale_grace_ticks,
      c::opponent_contest_cost,c::opponent_pressure_cost,c::opponent_claim_discount,c::stage1_frontier_value,c::explore_toward_resource_value,c::explore_tour_value,c::mission_explore_value,
      c::stage1_flood_value,c::stage1_ring_value,c::stage2_race_gate,c::race_reach_holdoff,c::stage2_explore_gate_tick,c::stage2_mission_pull_stale_ticks,c::stage2_assassin_value,c::stage2_pie_attack_discount,c::stage2_shadow_value,c::defend_stall_break_ticks,c::hazard_lead_ticks,c::resource_plan_depth,
      c::resource_beam_width,c::resource_candidate_limit,c::resource_plan_budget_ms,
      c::collision_replan,c::resource_refinement,c::resource_refinement_trials,c::tactical_search,
      c::tactical_risk,c::tactical_hp_value,c::tactical_death_cost,c::tactical_future_discount,c::tactical_goal_weight,
      c::loop_recovery,c::loop_window_ticks,c::recovery_ticks,c::stalled_goal_cooldown_ticks,
      c::worst_case_survival,c::post_cap_combat_weight,c::explore_component_value,c::low_hp_attack_discount,c::explore_probe_mission_bonus_scale};
  }
  void validate() const {
    for (double value:{stale_resource_decay,resource_mission_bonus,reach_mission_value,healing_below_hp,
         fortress_hold_value,fortress_capture_value,prepare_fortress_value,goal_retention_bonus,frontier_value,opponent_contest_cost,opponent_pressure_cost,
         opponent_claim_discount,stage1_frontier_value,
         explore_toward_resource_value,explore_tour_value,mission_explore_value,stage1_flood_value,stage1_ring_value,stage2_assassin_value,stage2_shadow_value,tactical_risk,tactical_hp_value,tactical_death_cost,tactical_future_discount,tactical_goal_weight,
         post_cap_combat_weight,explore_component_value,low_hp_attack_discount})
      if (!std::isfinite(value) || value<0 || value>1000) throw std::runtime_error("invalid strategy weight");
    if (healing_below_hp>100 || tactical_risk>1 || tactical_future_discount>1 || stage2_pie_attack_discount<0 || stage2_pie_attack_discount>1 || low_hp_attack_discount<0 || low_hp_attack_discount>1 || race_reach_holdoff<0 || race_reach_holdoff>1 || explore_probe_mission_bonus_scale<0 || explore_probe_mission_bonus_scale>1 || stage2_explore_gate_tick<0 || stage2_explore_gate_tick>500 || stage2_mission_pull_stale_ticks<0 || stage2_mission_pull_stale_ticks>500 || defend_stall_break_ticks<0 || defend_stall_break_ticks>100 || hazard_lead_ticks<0 || hazard_lead_ticks>20 || resource_plan_depth<1 || resource_plan_depth>8 ||
        stale_grace_ticks<0 || stale_grace_ticks>20 ||
        resource_beam_width<1 || resource_beam_width>32 || resource_candidate_limit<1 || resource_candidate_limit>32 ||
        resource_refinement_trials<0 || resource_refinement_trials>32 || resource_plan_budget_ms<1 || resource_plan_budget_ms>30 || loop_window_ticks<6 || loop_window_ticks>32 ||
        recovery_ticks<2 || recovery_ticks>64 || stalled_goal_cooldown_ticks<1 || stalled_goal_cooldown_ticks>200) throw std::runtime_error("invalid strategy budget");
  }
};
} // namespace rmtc
