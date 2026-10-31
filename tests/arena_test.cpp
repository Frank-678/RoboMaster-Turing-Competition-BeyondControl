#include "arena.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace rmtc;
using namespace rmtc::arena;
namespace {
void require(bool value,const char *message) { if (!value) throw std::runtime_error(message); }
bool close(double a,double b) { return std::abs(a-b)<1e-8; }
Scenario bare() {
  return {"rules-fixture","unit",0,2,9,9,500,4,0,true,{}, {},{{{4,4},{5,4},{4,5},{0,0}}},{},Json::array()};
}
void combat_contract() {
  std::array<rules::Fighter,4> f{};
  for (auto &car:f) car.alive=false;
  f[0].alive=f[1].alive=true;f[0].position={4,4};f[1].position={5,4};f[0].hp=f[1].hp=10;
  std::array<rules::Action,4> a{};a[0]={rules::ActionKind::attack,1};a[1]={rules::ActionKind::attack,3};
  auto r=rules::combat(f,a,0);
  require(!r.fighters[0].alive && !r.fighters[1].alive,"simultaneous attacks did not trade kills");
  require(close(rules::adjusted_damage(0,r.earned[0],1),10),"kill contribution was not credited before own death penalty");
  f[0].hp=100;f[1].hp=100;f[1].protected_spawn=true;f[2].alive=true;f[2].position={6,4};f[2].hp=10;a[1]={};
  r=rules::combat(f,a,0);require(r.fighters[2].hp==10 && r.earned[0]==0,"protected nearest robot did not block far target");
  f[1].protected_spawn=false;f[0].hp=10;f[2].position={4,5};f[2].hp=100;
  a[1]={rules::ActionKind::defend,-1};a[2]={rules::ActionKind::attack,0};
  r=rules::combat(f,a,20);
  require(close(r.earned[1],30) && close(r.earned[2],35),"ordinary/reflection overkill or unique kill sharing wrong");
  f[0].hp=100;f[0].area_attack=true;f[0].attack=50;f[1].position={5,5};f[2].position={6,6};a[1]={};a[2]={};
  r=rules::combat(f,a,0);require(r.fighters[1].hp==50 && r.fighters[2].hp==50,"area attack missed diagonals");
  require(rules::adjusted_damage(600,0,1)==540 && rules::adjusted_damage(10,0,1)==0,"D was capped or became negative");
}
void movement_contract() {
  std::array<rules::Fighter,4> f{};for (auto &car:f) car.alive=false;
  f[0].alive=f[1].alive=true;f[0].position={1,1};f[1].position={2,1};
  std::array<rules::Action,4> a{};a[0]={rules::ActionKind::move,1};a[1]={rules::ActionKind::move,1};
  auto moved=rules::move(f,a,9,9,{});require(!moved[0] && moved[1],"following vacated starting cell incorrectly succeeded");
  f[0].position={1,1};f[1].position={3,1};a[1].direction=3;
  moved=rules::move(f,a,9,9,{});require(!moved[0] && !moved[1],"contested empty cell did not reject both movers");
  require(rules::respawn({4,4},9,9,{{4,4},{4,3},{5,4},{4,5},{3,4}})==Cell{4,2},"spawn search treated walls as traversal blockers or wrong U/R/D/L order");
}
void engine_contract() {
  auto s=bare();s.resources[{5,4}]=20;Game g(s);auto &cars=g.fixture_cars();cars[0].fighter.hp=20;cars[1].fighter.hp=20;cars[2].fighter.alive=cars[3].fighter.alive=false;
  std::array<rules::Action,4> a{};a[0]={rules::ActionKind::move,1};a[1]={rules::ActionKind::attack,3};g.step(a);
  require(!g.cars()[0].fighter.alive && g.cars()[0].resource==0,"dead mover moved or collected resource");
  require(g.cars()[0].respawn_in==3,"death tick consumed respawn wait");
  for (int i=0;i<2;++i) {g.step({});require(!g.cars()[0].fighter.alive,"respawned too early");}
  g.step({});require(g.cars()[0].fighter.alive && g.cars()[0].fighter.protected_spawn,"did not revive after three subsequent ticks");
  s=bare();s.spawns={{{3,4},{4,3},{4,4},{0,0}}};Event e;e.kind=EventKind::chili;e.start=0;e.end=10;e.amount=50;e.points={{3,3},{4,4}};e.key="fire";
  s.announcements={{e,0}};Game fire(s);fire.fixture_cars()[2].fighter.hp=40;fire.fixture_cars()[2].fighter.position={3,5};
  a={};a[0]={rules::ActionKind::move,0};a[1]={rules::ActionKind::move,2};fire.step(a);
  require(!fire.cars()[2].fighter.alive && close(fire.cars()[0].damage,60),"chili damage credited as ordinary D or kill missed");
  s=bare();s.interpretation_delay=4;e.kind=EventKind::explosion;e.start=10;e.end=11;e.area={7,7,8,8};s.announcements={{e,2}};Game delayed(s);
  for (int i=0;i<6;++i) {require(delayed.interpreted_events().empty(),"future/instant event oracle leaked to agent");delayed.step({});}
  require(delayed.interpreted_events().size()==1,"delayed announcement never delivered");
  auto hidden=s;hidden.resources[{8,8}]=20;s.resources[{8,8}]=10;s.radius=hidden.radius=1;Game first(s),second(hidden);
  require(first.observe(0)==second.observe(0),"hidden map resource leaked into public observation");
  // 四个席位各看自己的身份、坐标和成绩，不能把A的整份观测复用给其他玩家。
  Game actors(bare());
  for(int seat=0;seat<4;++seat) {
    auto &car=actors.fixture_cars()[seat];car.resource=10*seat;car.damage=7*seat;
    const auto observation=actors.observe(seat);const std::string identity(1,static_cast<char>('A'+seat));
    require(observation.at("self").at("team")==identity && observation.at("self").at("robot_id")==identity,
            "actor observation reused another seat identity");
    require(observation.at("self").at("position")==Json({car.fighter.position.first,car.fighter.position.second}) &&
            observation.at("score").at("resource_value")==10*seat && observation.at("score").at("effective_damage")==7*seat,
            "actor observation reused another seat state or score");
    for(const auto &entity:observation.at("visible_entities")) if(entity.at("type")=="robot")
      require(entity.at("team")!=identity,"actor appeared as its own opponent");
  }
}
void event_contract() {
  using K=rules::ActionKind;
  auto s=bare();
  Event e;e.key="event";e.start=0;e.end=8;e.area={4,4,4,4};e.points={{5,4}};
  // 爆炸先于本Tick MOVE；出生保护免疫，环境死亡没有击杀奖励。
  e.kind=EventKind::explosion;e.amount=35;s.announcements={{e,0}};
  Game explosion(s);explosion.fixture_cars()[0].fighter.hp=30;
  std::array<rules::Action,4> a{};a[0]={K::move,0};explosion.step(a);
  require(!explosion.cars()[0].fighter.alive && explosion.cars()[0].damage==0,"explosion activation order or environmental kill reward wrong");
  Game immune(s);immune.fixture_cars()[0].fighter.protected_spawn=true;immune.step({});
  require(immune.cars()[0].fighter.hp==100,"spawn protection failed against explosion");
  // 幻术只改MOVE，不在离开区域后提前结束。
  e.kind=EventKind::illusion;s.announcements={{e,0}};Game illusion(s);illusion.fixture_cars()[2].fighter.alive=false;
  a={};a[0]={K::move,0};illusion.step(a);require(illusion.cars()[0].fighter.position==Cell{4,5},"illusion did not invert move");
  illusion.fixture_cars()[2].fighter.alive=false;illusion.step(a);
  require(illusion.cars()[0].fighter.position==Cell{4,6},"illusion ended when leaving activation area");
  // 治疗入点当Tick生效，点消失后已领效果继续；不会从死亡中回血复活。
  e.kind=EventKind::heal;e.end=1;e.amount=10;e.effect_ticks=3;s.announcements={{e,0}};Game heal(s);
  heal.fixture_cars()[1].fighter.alive=false;heal.fixture_cars()[0].fighter.hp=40;
  a={};a[0]={K::move,1};heal.step(a);require(heal.cars()[0].fighter.hp==50,"heal did not start on entry tick");
  heal.step({});heal.step({});heal.step({});require(heal.cars()[0].fighter.hp==70,"heal expiry confused point lifetime and effect duration");
  // 两次连续存活驻留后才有无双；当Tick不能补打，已领加成跨死亡保留。
  e.kind=EventKind::fortress;e.end=8;e.area={4,4,4,4};e.capture_ticks=2;e.amount=50;e.defended_damage=10;s.announcements={{e,0}};
  Game fortress(s);a={};a[0]={K::attack,1};fortress.step(a);fortress.step(a);
  require(fortress.cars()[1].fighter.hp==60 && fortress.cars()[0].fighter.area_attack,"fortress granted attack before capture settlement");
  fortress.fixture_cars()[0].fighter.alive=false;fortress.fixture_cars()[0].respawn_in=1;fortress.step({});
  require(fortress.cars()[0].fighter.alive && fortress.cars()[0].fighter.area_attack,"fortress buff was erased by death");
  // 分裂排除四个初始出生点；在其他激活格立即重生、取消动作并扣60D。
  e.kind=EventKind::split;e.points={{4,3}};e.area={4,4,4,4};e.end=5;s.announcements={{e,0}};Game split(s);
  split.fixture_cars()[0].fighter.position={4,3};split.fixture_cars()[0].damage=100;
  a={};a[0]={K::move,1};split.step(a);
  require(split.cars()[0].fighter.position==Cell{4,4} && split.cars()[0].damage==40 &&
          split.cars()[0].last_action.at("action")=="wait","split activation did not force immediate protected respawn and cancel movement");
  require(!split.current_walls().count({4,4}) && split.current_walls().count({4,3}),"split wall did not exclude initial spawn");
  // 易伤来自成功移动，下一次战斗才翻倍；静止和失败移动不触发。
  e.kind=EventKind::vulnerability;e.end=4;e.trigger_end=2;e.multiplier=2;s.announcements={{e,0}};Game vulnerable(s);
  vulnerable.fixture_cars()[1].fighter.position={5,3};a={};a[0]={K::move,0};vulnerable.step(a);
  a={};a[1]={K::attack,3};vulnerable.step(a);
  require(vulnerable.cars()[0].fighter.hp==60,"vulnerability did not double next-tick ordinary damage");
  vulnerable.step({});vulnerable.step({});require(vulnerable.cars()[0].fighter.vulnerability==1,"vulnerability did not expire at absolute deadline");
}

void maps_contract() {
  for (const char *style:{"open","maze","rooms","lanes","clusters","crossroads"}) for (unsigned seed:{2U,9U,41U}) {
    auto s=make_scenario(style,seed,2,6,true);int value=0;for (const auto &r:s.resources) {value+=r.second;require(!s.walls.count(r.first),"resource generated inside wall");}
    require(value==200 && s.resources.size()==16,"map violated resource baseline");
    for (Cell spawn:s.spawns) require(!s.walls.count(spawn),"spawn generated inside wall");
  }
}
}
int main() {
  combat_contract();movement_contract();engine_contract();event_contract();maps_contract();
  std::cout<<"arena rules: combat, reflection, overkill, movement, respawn, eight event kinds, oracle isolation and 18 maps passed\n";
}
