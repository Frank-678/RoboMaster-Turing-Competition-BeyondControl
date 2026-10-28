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
  PARAMETER(healing_below_hp) PARAMETER(fortress_hold_value) PARAMETER(fortress_capture_value)
  PARAMETER(goal_retention_bonus) PARAMETER(frontier_value) PARAMETER(opponent_contest_cost) PARAMETER(opponent_pressure_cost)
  PARAMETER(hazard_lead_ticks) PARAMETER(resource_plan_depth) PARAMETER(resource_beam_width)
  PARAMETER(resource_candidate_limit) PARAMETER(resource_plan_budget_ms)
  PARAMETER(collision_replan) PARAMETER(resource_refinement) PARAMETER(resource_refinement_trials)
  PARAMETER(tactical_search) PARAMETER(tactical_risk) PARAMETER(tactical_hp_value) PARAMETER(tactical_death_cost)
  PARAMETER(tactical_future_discount) PARAMETER(tactical_goal_weight)
#undef PARAMETER
  for (const auto &entry:values.items()) if (!recognized.count(entry.key())) throw std::runtime_error("unknown arena parameter: "+entry.key());
  p.validate(); // 规则伤害、距离、D扣除等根本不在可写参数名单。
}
Json play(const Json &spec,const rmtc::StrategyParameters &parameters,const rmtc::StrategyParameters &baseline,bool trace) {
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
  return output;
}
}
int main() {
  try {
    const Json input=Json::parse(std::cin);
    auto baseline=rmtc::StrategyParameters::competition();configure(baseline,input.at("baseline_parameters"));
    for (const auto &profile:input.at("profiles")) {
      auto parameters=rmtc::StrategyParameters::competition();configure(parameters,profile.at("parameters"));
      int index=0;
      for (const auto &scene:input.at("scenes")) {
        auto result=play(scene,parameters,baseline,input.at("trace_first").get<bool>() && index++==0);
        result["profile"]=profile.at("name");
        std::cout<<result.dump()<<std::endl;
      }
    }
  } catch (const std::exception &error) { std::cerr<<"arena: "<<error.what()<<'\n';return 2; }
}
