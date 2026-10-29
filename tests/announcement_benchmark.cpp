// 公告前置规划的配对夹具。生产策略只收到本队局部感知与已经公告的解释，没有未来真值输入。
#include "arena.hpp"
#include "rmtc_bot/strategy.hpp"
#include <iostream>

using namespace rmtc;
using Json=nlohmann::json; // 本测试输出轨迹，不向ROS发布。
namespace {
Json run(int delay) { // delay是测试oracle交付解释的Tick，不是真实模型延迟测量。
  arena::Scenario scene{"announced-fortress","corridor",0,2,9,9,30,4,delay,true,{},{{{3,4},10}},
    {{{4,4},{0,0},{8,0},{8,8}}},{},Json::array()};
  for(int x=0;x<9;++x) {scene.walls.insert({x,3});scene.walls.insert({x,5});}
  Event event;event.kind=EventKind::fortress;event.key="announced-fortress";
  event.start=5;event.end=20;event.area={8,4,8,4};event.capture_ticks=4;event.amount=35;event.defended_damage=7;
  event.source="测试公告：Tick5在[8,4]出现堡垒，持续至Tick20，连续驻留4Tick可获得无双。";
  scene.announcements={{event,0}};arena::Game game(scene);auto &cars=game.fixture_cars();
  cars[0].resource=120; // 已收集的历史资源；还剩左侧一份普通资源，R已封顶，D尚为0。
  for(int i=1;i<4;++i) {cars[i].fighter.alive=false;cars[i].respawn_in=1000;} // 夹具停用其他车，生产复活规则不变。
  // 显式注入开启参数：被测对象是堡垒预告提前占点链路，不是默认关闭态（默认0只测关闭行为）。
  auto weights=StrategyParameters::competition();weights.prepare_fortress_value=12.0;
  Strategy bot(2,weights);int gained=-1,empowered_ticks=0;Json trace=Json::array();
  while(game.tick()<scene.ticks) {
    const int tick=game.tick();bot.merge_events(game.interpreted_events()); // 公告后延迟交付，不提前泄漏未公告安排。
    std::array<rules::Action,4> actions{};const auto action=bot.decide(game.observe(0));actions[0]=rules::decode_action(action);
    empowered_ticks+=cars[0].fighter.area_attack;const auto reason=bot.reason();game.step(actions);
    if(cars[0].fighter.area_attack && gained<0) gained=tick; // 本Tick结算授予；下一Tick才有攻击窗口。
    trace.push_back({{"tick",tick},{"position",{cars[0].fighter.position.first,cars[0].fighter.position.second}},
      {"action",action},{"reason",reason},{"empowered_after_settlement",cars[0].fighter.area_attack}});
  }
  return {{"interpretation_delay",delay},{"gain_settlement_tick",gained},{"empowered_action_windows",empowered_ticks},
    {"result",game.result()[0]},{"trace",trace}};
}
}
int main() {
  for(int delay:{0,8,20}) std::cout<<run(delay).dump()<<'\n';
}
