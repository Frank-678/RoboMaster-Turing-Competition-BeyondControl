// 离线多风格对战入口：JSON只给评测器，不被ROS选手读取。
#include "arena.hpp"
#include "rmtc_bot/strategy.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

using Json=nlohmann::json;
namespace {
void configure(rmtc::StrategyParameters &p,const Json &values) {
  std::set<std::string> recognized;
#define PARAMETER(name) if(values.contains(#name)){p.name=values.at(#name).get<decltype(p.name)>();recognized.insert(#name);}
  PARAMETER(stale_resource_decay) PARAMETER(resource_mission_bonus) PARAMETER(reach_mission_value)
  PARAMETER(healing_below_hp) PARAMETER(fortress_hold_value) PARAMETER(fortress_capture_value) PARAMETER(prepare_fortress_value)
  PARAMETER(goal_retention_bonus) PARAMETER(stale_grace_ticks) PARAMETER(frontier_value) PARAMETER(stage1_frontier_value) PARAMETER(opponent_contest_cost) PARAMETER(opponent_pressure_cost)
  PARAMETER(opponent_claim_discount)
  PARAMETER(explore_toward_resource_value) PARAMETER(explore_tour_value) PARAMETER(mission_explore_value)
  PARAMETER(stage1_flood_value) PARAMETER(stage1_ring_value) PARAMETER(stage2_race_gate) PARAMETER(stage2_explore_gate_tick) PARAMETER(stage2_mission_pull_stale_ticks)
  PARAMETER(stage2_assassin_value) PARAMETER(stage2_pie_attack_discount) PARAMETER(stage2_shadow_value) PARAMETER(defend_stall_break_ticks)
  PARAMETER(hazard_lead_ticks) PARAMETER(resource_plan_depth) PARAMETER(resource_beam_width)
  PARAMETER(resource_candidate_limit) PARAMETER(resource_plan_budget_ms)
  PARAMETER(loop_recovery) PARAMETER(loop_window_ticks) PARAMETER(recovery_ticks) PARAMETER(stalled_goal_cooldown_ticks)
  PARAMETER(collision_replan) PARAMETER(resource_refinement) PARAMETER(resource_refinement_trials)
  PARAMETER(tactical_search) PARAMETER(tactical_risk) PARAMETER(tactical_hp_value) PARAMETER(tactical_death_cost)
  PARAMETER(tactical_future_discount) PARAMETER(tactical_goal_weight)
  PARAMETER(worst_case_survival) PARAMETER(post_cap_combat_weight) PARAMETER(explore_component_value) PARAMETER(low_hp_attack_discount)
#undef PARAMETER
  for (const auto &entry:values.items()) if (!recognized.count(entry.key())) throw std::runtime_error("unknown arena parameter: "+entry.key());
  p.validate(); // 规则伤害、距离、D扣除等根本不在可写参数名单。
}
Json play(const Json &spec,const rmtc::StrategyParameters &parameters,const rmtc::StrategyParameters &baseline,bool trace,
          const std::map<std::string,rmtc::StrategyParameters> &named) {
  using namespace rmtc;
  const int stage=spec.at("stage"),seat=spec.at("seat");
  if (seat<0 || seat>3 || (stage==1 && seat!=0)) throw std::runtime_error("invalid focal seat");
  auto scenario=arena::make_scenario(spec.at("style"),spec.at("seed"),stage,spec.at("delay"),spec.at("ai_success"));
  arena::Game game(scenario);
  std::array<std::unique_ptr<Strategy>,4> players;
  std::array<std::unique_ptr<arena::Opponent>,4> opponents;
  const auto styles=spec.at("opponents").get<std::vector<std::string>>();
  if (styles.size()!=4) throw std::runtime_error("four explicit opponent entries required");
  for (int i=0;i<4;++i) {
    if (i==seat || styles[i]=="baseline") players[i]=std::make_unique<Strategy>(stage,i==seat ? parameters : baseline);
    else if (styles[i].rfind("profile:",0)==0) { // 自己打自己：指定本文件外的具名候选参数。
      const auto found=named.find(styles[i].substr(8));
      if (found==named.end()) throw std::runtime_error("unknown profile opponent: "+styles[i]);
      players[i]=std::make_unique<Strategy>(stage,found->second);
    }
    else opponents[i]=std::make_unique<arena::Opponent>(styles[i],scenario.seed+101*i,stage);
  }
  std::vector<double> durations;
  Json frames=Json::array();
  while (game.tick()<scenario.ticks) {
    std::array<rules::Action,4> actions{};
    const auto known=game.interpreted_events();
    Json decisions=Json::array();
    for (int i=0;i<4;++i) {
      if (stage==1 && i!=0) continue;
      const auto perception=game.observe(i); // 这是策略唯一的地图/车辆输入。
      if (players[i]) {
        if (scenario.simulated_ai_success && game.tick()>=scenario.interpretation_delay) players[i]->note_model_reply(); // 明确的离线资格夹具，不是真实HTTP。
        players[i]->merge_events(known);
        const auto start=std::chrono::steady_clock::now();
        const Json action=players[i]->decide(perception);
        if (i==seat) durations.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        actions[i]=rules::decode_action(action);
        if (trace) decisions.push_back({{"seat",i},{"action",action},{"reason",players[i]->reason()}});
      } else {
        actions[i]=opponents[i]->act(perception,known);
        if (trace) decisions.push_back({{"seat",i},{"action",rules::encode_action(actions[i])},{"reason",styles[i]}});
      }
    }
    game.step(actions);
    if (trace) {
      Json positions=Json::array();for (const auto &car:game.cars()) positions.push_back({car.fighter.position.first,car.fighter.position.second});
      frames.push_back({{"tick",game.tick()-1},{"decisions",decisions},{"positions",positions},{"scores",game.result()}});
    }
  }
  const auto results=game.result();const auto &own=results.at(seat);
  double margin=own.at("score").get<double>(),win=1;int rank=1,ties=1;
  const auto order=[&](int a,int b) {
    for (const char *key:{"score","qualified_M","R","D"}) {
      const double delta=results[a].at(key).get<double>()-results[b].at(key).get<double>();
      if (std::abs(delta)>1e-8) return delta>0 ? 1 : -1;
    }
    const int delta=results[b].at("invalid").get<int>()-results[a].at("invalid").get<int>();
    return delta>0 ? 1 : delta<0 ? -1 : 0;
  };
  if (stage==2) {
    for (int other=0;other<4;++other) if (other!=seat) {
      margin-=results[other].at("score").get<double>()/3;
      const int comparison=order(other,seat);rank+=comparison>0;ties+=comparison==0;
    }
    win=rank==1 ? 1.0/ties : 0;
  }
  std::sort(durations.begin(),durations.end());
  Json output={{"scenario",spec},{"result",results},{"focal",seat},{"rank",rank},{"win_share",stage==2 ? Json(win) : Json()},
    {"score_margin",margin},{"decide_p95_ms",durations.at((durations.size()*95+99)/100-1)},
    {"decide_max_ms",durations.back()},{"frames",frames}};
  if (trace) { // 单局轨迹附带初态地图真值，仅离线诊断用；不传给策略。
    Json walls=Json::array();for (const auto &wall:scenario.walls) walls.push_back({wall.first,wall.second});
    Json res=Json::array();for (const auto &entry:scenario.resources) res.push_back({{"x",entry.first.first},{"y",entry.first.second},{"v",entry.second}});
    Json spawns=Json::array();for (const auto &spawn:scenario.spawns) spawns.push_back({spawn.first,spawn.second});
    output["trace_map"]={{"walls",walls},{"resources",res},{"spawns",spawns},{"missions",scenario.missions},{"width",scenario.width},{"height",scenario.height}};
  }
  return output;
}
// --dump-competition：把编译进二进制的比赛参数（match_config→competition()）原样导出为JSON。
// 供 run_arena.py 在每份报告里记录最终生效参数，避免把历史 selected-parameters.json 误当当前配置。
Json competition_dump() {
  const auto p=rmtc::StrategyParameters::competition();
  return Json{
    {"stale_resource_decay",p.stale_resource_decay},{"resource_mission_bonus",p.resource_mission_bonus},
    {"reach_mission_value",p.reach_mission_value},{"healing_below_hp",p.healing_below_hp},
    {"fortress_hold_value",p.fortress_hold_value},{"fortress_capture_value",p.fortress_capture_value},
    {"prepare_fortress_value",p.prepare_fortress_value},{"goal_retention_bonus",p.goal_retention_bonus},
    {"stale_grace_ticks",p.stale_grace_ticks},
    {"frontier_value",p.frontier_value},{"stage1_frontier_value",p.stage1_frontier_value},
    {"opponent_contest_cost",p.opponent_contest_cost},{"opponent_pressure_cost",p.opponent_pressure_cost},
    {"opponent_claim_discount",p.opponent_claim_discount},
    {"explore_toward_resource_value",p.explore_toward_resource_value},{"explore_tour_value",p.explore_tour_value},
    {"mission_explore_value",p.mission_explore_value},
    {"stage1_flood_value",p.stage1_flood_value},{"stage1_ring_value",p.stage1_ring_value},
    {"stage2_race_gate",p.stage2_race_gate},{"stage2_explore_gate_tick",p.stage2_explore_gate_tick},
    {"stage2_mission_pull_stale_ticks",p.stage2_mission_pull_stale_ticks},
    {"stage2_assassin_value",p.stage2_assassin_value},{"stage2_pie_attack_discount",p.stage2_pie_attack_discount},
    {"stage2_shadow_value",p.stage2_shadow_value},{"defend_stall_break_ticks",p.defend_stall_break_ticks},
    {"hazard_lead_ticks",p.hazard_lead_ticks},{"resource_plan_depth",p.resource_plan_depth},
    {"resource_beam_width",p.resource_beam_width},{"resource_candidate_limit",p.resource_candidate_limit},
    {"resource_plan_budget_ms",p.resource_plan_budget_ms},
    {"loop_recovery",p.loop_recovery},{"loop_window_ticks",p.loop_window_ticks},{"recovery_ticks",p.recovery_ticks},
    {"stalled_goal_cooldown_ticks",p.stalled_goal_cooldown_ticks},
    {"collision_replan",p.collision_replan},{"resource_refinement",p.resource_refinement},
    {"resource_refinement_trials",p.resource_refinement_trials},
    {"tactical_search",p.tactical_search},{"tactical_risk",p.tactical_risk},{"tactical_hp_value",p.tactical_hp_value},
    {"tactical_death_cost",p.tactical_death_cost},{"tactical_future_discount",p.tactical_future_discount},
    {"tactical_goal_weight",p.tactical_goal_weight},
    {"worst_case_survival",p.worst_case_survival},{"post_cap_combat_weight",p.post_cap_combat_weight},{"explore_component_value",p.explore_component_value},{"low_hp_attack_discount",p.low_hp_attack_discount}};
}
}
int main(int argc,char **argv) {
  try {
    if (argc>1 && std::string(argv[1])=="--dump-competition") {
      std::cout<<competition_dump().dump()<<'\n';return 0;
    }
    const Json input=Json::parse(std::cin);
    auto baseline=rmtc::StrategyParameters::competition();configure(baseline,input.at("baseline_parameters"));
    std::map<std::string,rmtc::StrategyParameters> named; // profile: 对手按名字解析到同一份已校验参数。
    for (const auto &profile:input.at("profiles")) {
      auto parameters=rmtc::StrategyParameters::competition();configure(parameters,profile.at("parameters"));
      named[profile.at("name").get<std::string>()]=parameters;
    }
    for (const auto &profile:input.at("profiles")) {
      int index=0;
      for (const auto &scene:input.at("scenes")) {
        auto result=play(scene,named.at(profile.at("name").get<std::string>()),baseline,input.at("trace_first").get<bool>() && index++==0,named);
        result["profile"]=profile.at("name");
        std::cout<<result.dump()<<std::endl;
      }
    }
  } catch (const std::exception &error) { std::cerr<<"arena: "<<error.what()<<'\n';return 2; }
}
