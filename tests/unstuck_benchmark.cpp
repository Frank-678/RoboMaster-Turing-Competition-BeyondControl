// 只在测试端生成地图；选手仍只读Game::observe，不直接读取场景真值。
#include "arena.hpp"
#include "rmtc_bot/strategy.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace rmtc;
using namespace rmtc::arena;
namespace {
struct Fixture {
  Scenario scenario;
  bool two_strategies=false; // 两车使用同一策略，检验争同格造成的对称循环。
  bool closed=false; // 无物理出口用例只检查停止无益往返，不要求穿墙。
};
Fixture fixture(const std::string &name) {
  Fixture f;auto &s=f.scenario;
  s={name,"adversarial",1,2,20,16,500,4,0,false,{}, {},{{{4,0},{4,2},{18,14},{1,14}}},{},Json::array()};
  if (name=="top_edge_reflection" || name=="top_edge_reflection_short") {
    s.resources[{16,12}]=20;
    Event e;e.kind=EventKind::reflect;e.key="fixture-reflection";e.start=0;
    e.end=name=="top_edge_reflection" ? 500 : 30;e.amount=20;
    e.source="测试公告：本事件有效期间，攻击防御车辆会受20点反伤。";s.announcements={{e,0}};
  } else if (name=="symmetric_contention") {
    s.spawns[0]={3,1};s.spawns[1]={5,1};f.two_strategies=true;
    s.missions={{{"id","center"},{"type","reach"},{"position",{4,1}},{"target",1}}};
    s.resources[{16,12}]=20;
  } else if (name=="wall_detour") {
    s.spawns[0]={4,0};
    for (int x=0;x<15;++x) s.walls.insert({x,2});
    s.resources[{4,3}]=20;
    s.missions={{{"id","beyond-wall"},{"type","reach"},{"position",{4,3}},{"target",1}}};
  } else if (name=="closed_two_cells") {
    s.spawns[0]={4,0};f.closed=true;
    for (int y=0;y<s.height;++y) for (int x=0;x<s.width;++x)
      if (Cell{x,y}!=Cell{4,0} && Cell{x,y}!=Cell{4,1}) s.walls.insert({x,y});
    s.missions={{{"id","unreachable"},{"type","reach"},{"position",{16,12}},{"target",1}}};
  } else if (name=="long_corridor") {
    s.spawns[0]={1,1};
    for (int y=0;y<s.height;++y) for (int x=0;x<s.width;++x) if (y!=1) s.walls.insert({x,y});
    s.resources[{18,1}]=20;s.resources[{0,1}]=10;
  } else throw std::runtime_error("unknown unstuck fixture");
  return f;
}
Json run(const std::string &name) {
  auto setup=fixture(name);Game game(setup.scenario);
  auto &cars=game.fixture_cars();cars[2].fighter.alive=cars[3].fighter.alive=false;
  if (!setup.two_strategies && name!="top_edge_reflection" && name!="top_edge_reflection_short") cars[1].fighter.alive=false;
  for(auto &car:cars) if(!car.fighter.alive) car.respawn_in=1000000; // 导航夹具停用其他车；不将此计时交给选手，也不是正式复活规则。
  std::array<std::unique_ptr<Strategy>,2> policies;
  policies[0]=std::make_unique<Strategy>(2);if(setup.two_strategies) policies[1]=std::make_unique<Strategy>(2);
  const auto &announcements=setup.scenario.announcements;
  for (auto &policy:policies) if(policy) for(const auto &a:announcements) policy->merge_events({a.event}); // 已公告、手工正确解释夹具，无真实LLM。
  std::vector<Cell> positions;
  std::array<int,7> repeats{};int longest=0,first_resource=-1,first_task=-1;
  std::map<std::string,int> reasons;double previous_r=0,previous_d=0;std::size_t previous_m=0;
  double max_ms=0;Json trace=Json::array();
  for (int tick=0;tick<setup.scenario.ticks;++tick) {
    std::array<rules::Action,4> actions{};
    for (int i=0;i<2;++i) {
      if (policies[i]) {
        const auto begin=std::chrono::steady_clock::now();const auto action=policies[i]->decide(game.observe(i));
        if(i==0) max_ms=std::max(max_ms,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
        actions[i]=rules::decode_action(action);
      } else if(cars[i].fighter.alive) actions[i]={rules::ActionKind::defend,-1}; // 顽固驻守，不追逐也不配合让路。
    }
    const auto reason=policies[0]->reason();++reasons[reason];game.step(actions);
    const auto &own=game.cars()[0];positions.push_back(own.fighter.position);
    const bool progress=own.resource>previous_r || own.damage>previous_d || own.completed.size()>previous_m;
    previous_r=own.resource;previous_d=own.damage;previous_m=own.completed.size();
    if(own.resource>0 && first_resource<0) first_resource=tick;
    if(!own.completed.empty() && first_task<0) first_task=tick;
    for(int period=2;period<=6;++period) {
      const bool matching=!progress && positions.size()>static_cast<std::size_t>(period) &&
        positions.back()==positions[positions.size()-period-1] && positions.back()!=positions[positions.size()-2];
      repeats[period]=matching ? repeats[period]+1 : 0;
      if(repeats[period]>=period) longest=std::max(longest,repeats[period]+period);
    }
    if(tick<40 || tick>=460) trace.push_back({{"tick",tick},{"position",{own.fighter.position.first,own.fighter.position.second}},
      {"action",rules::encode_action(actions[0])},{"reason",reason},{"R",own.resource},{"D",own.damage}});
  }
  return {{"name",name},{"ticks",setup.scenario.ticks},{"result",game.result()[0]},
    {"longest_periodic_motion_without_score_progress",longest},{"first_resource_tick",first_resource},{"first_task_tick",first_task},
    {"reasons",reasons},{"decide_max_ms",max_ms},{"trace_first_last_40",trace},
    {"scope","Independent synthetic judge and manual already-announced event oracle; not user map, original referee or real AI"}};
}
}
int main(int argc,char **argv) {
  const std::string version=argc>1 ? argv[1] : "unspecified";
  for(const char *name:{"top_edge_reflection","top_edge_reflection_short","symmetric_contention","wall_detour","closed_two_cells","long_corridor"}) {
    auto result=run(name);result["version"]=version;std::cout<<result.dump()<<'\n';
  }
}
