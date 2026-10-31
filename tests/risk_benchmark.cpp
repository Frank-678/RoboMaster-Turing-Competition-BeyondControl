// 同一局面旋转四次、轮换四席位，分别结算对手防御/踩点；只测试，不调用真实LLM。
#include "arena.hpp"
#include "rmtc_bot/strategy.hpp"
#include <array>
#include <iostream>

using namespace rmtc;
using Json=nlohmann::json; // 仅在本测试文件构造输入、轨迹和指标。
namespace {
// 把9×9地图上的cell绕中心[4,4]顺时针转rotation次；规则和方向随地图一起旋转。
Cell rotate(Cell cell,int rotation) {
  for(int i=0;i<rotation;++i) cell={8-cell.second,cell.first};
  return cell;
}
// seat为被测车席位；rotation旋转地图，trigger决定敌车是否真的移动踩辣椒。
Json run(int seat,int rotation,bool trigger) {
  arena::Scenario scene{"uncertain-fire","minimal",0,2,9,9,2,4,0,true,{}, {},
    {{{0,0},{8,0},{8,8},{0,8}}}, {}, Json::array()};
  const int enemy=(seat+1)%4; // 仅一个可见敌车，不假设多车协同行动。
  scene.spawns[seat]={4,4};scene.spawns[enemy]=rotate({5,2},rotation);
  for(Cell wall:{Cell{3,4},Cell{5,4},Cell{4,5}}) scene.walls.insert(rotate(wall,rotation));
  Event fire;fire.kind=EventKind::chili;fire.key="fire";fire.start=0;fire.end=10;
  fire.points={rotate({4,2},rotation)};fire.amount=50;
  fire.source="测试公告：Tick0起辣椒点有效10Tick，入点触发50伤害火线。";
  Event blast;blast.kind=EventKind::explosion;blast.key="blast";blast.start=1;blast.end=2;
  blast.area={4,4,4,4};blast.amount=80;
  blast.source="测试公告：Tick1在[4,4]发生80伤害爆炸。";
  scene.announcements={{fire,0},{blast,0}};
  arena::Game game(scene);auto &cars=game.fixture_cars();cars[seat].fighter.hp=40;
  for(int i=0;i<4;++i) if(i!=seat && i!=enemy) {
    cars[i].fighter.alive=false;cars[i].respawn_in=100; // 仅保持这两个Tick不参与；生产复活规则不变。
  }
  Strategy bot(2);Json trace=Json::array(); // 每例独立策略，不沿用其他席位的记忆。
  bot.merge_events(game.interpreted_events()); // 已公告、手工正确解释夹具；没有真实模型成功计数。
  for(int tick=0;tick<2;++tick) {
    const auto action=bot.decide(game.observe(seat));std::array<rules::Action,4> actions{};
    actions[seat]=rules::decode_action(action);
    actions[enemy]=trigger && tick==0 ? rules::Action{rules::ActionKind::move,(3+rotation)%4} :
      rules::Action{rules::ActionKind::defend,-1};
    const auto reason=bot.reason();game.step(actions);
    trace.push_back({{"tick",tick},{"action",action},{"reason",reason},{"alive",cars[seat].fighter.alive},
      {"position",{cars[seat].fighter.position.first,cars[seat].fighter.position.second}}});
  }
  return {{"seat",seat},{"rotation",rotation},{"enemy_triggers_chili",trigger},{"trace",trace},{"result",game.result()[seat]}};
}
}
int main() {
  for(int seat=0;seat<4;++seat) for(int rotation=0;rotation<4;++rotation) for(bool trigger:{false,true})
    std::cout<<run(seat,rotation,trigger).dump()<<'\n';
}
