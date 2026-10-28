#include "arena.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>
#include <tuple>

namespace rmtc::arena {
namespace {
std::string team(int seat) { return std::string(1,static_cast<char>('A'+seat)); }
Cell position(const Json &j) { return {j.at(0).get<int>(),j.at(1).get<int>()}; }
}
Json relative_entity(const std::string &type,Cell delta) {
  std::string grid;
  for (int y=0;y<9;++y) {
    if (y) grid+='\n';
    for (int x=0;x<9;++x) grid+=x==4 && y==4 ? '1' : x==delta.first+4 && y==delta.second+4 ? '2' : '0';
  }
  return {{"type",type},{"position",grid}};
}
Game::Game(Scenario scenario) : scenario_(std::move(scenario)),resources_(scenario_.resources) {
  for (int i=0;i<4;++i) {
    cars_[i].fighter.position=scenario_.spawns[i];
    cars_[i].fighter.alive=scenario_.stage==2 || i==0;
  }
}
std::set<Cell> Game::current_walls() const {
  auto walls=scenario_.walls;
  for (const auto &announcement:scenario_.announcements) {
    const auto &event=announcement.event;
    if (event.kind!=EventKind::split || !event.active(tick_)) continue;
    for (int y=0;y<scenario_.height;++y) for (int x=0;x<scenario_.width;++x) {
      const Cell cell{x,y};
      if (event.contains(cell) && std::find(scenario_.spawns.begin(),scenario_.spawns.end(),cell)==scenario_.spawns.end()) walls.insert(cell);
    }
  }
  return walls;
}
void Game::refresh_effects() {
  for (auto &car:cars_) {
    car.fighter.attack=20; car.fighter.defended=10; car.fighter.area_attack=false; car.fighter.vulnerability=1;
    for (const auto &announcement:scenario_.announcements) {
      const auto &event=announcement.event;
      if (!event.active(tick_)) continue;
      if (event.kind==EventKind::fortress && car.empowered.count(event.key)) {
        car.fighter.area_attack=true;
        car.fighter.attack=std::max(car.fighter.attack,event.amount); car.fighter.defended=event.defended_damage;
      }
      if (event.kind==EventKind::vulnerability && car.vulnerable.count(event.key))
        car.fighter.vulnerability=std::max(car.fighter.vulnerability,event.multiplier);
    }
  }
}
bool Game::resurrect(int seat) {
  auto blocked=current_walls();
  for (int other=0;other<4;++other) if (other!=seat && cars_[other].fighter.alive) blocked.insert(cars_[other].fighter.position);
  const Cell landing=rules::respawn(scenario_.spawns[seat],scenario_.width,scenario_.height,blocked);
  if (landing.first<0) return false;
  auto &car=cars_[seat];
  car.fighter.position=landing; car.fighter.hp=100; car.fighter.alive=true;
  car.fighter.protected_spawn=true; car.protected_left=rules::protection_ticks; car.respawn_in=0;
  car.position_reason="respawn";
  return true;
}
std::vector<Event> Game::interpreted_events() const {
  std::vector<Event> known;
  if (!scenario_.simulated_ai_success) return known; // 无模拟AI资格时也不赠送“免费解释”。
  for (const auto &a:scenario_.announcements)
    if (a.notice+scenario_.interpretation_delay<=tick_) known.push_back(a.event);
  return known;
}
Json Game::observe(int seat) const {
  const auto &car=cars_.at(seat); const Cell current=car.fighter.position;
  Json completed=Json::array(); for (const auto &id:car.completed) completed.push_back(id);
  Json self={{"team",team(seat)},{"robot_id",team(seat)},{"position",{current.first,current.second}},
    {"hp",car.fighter.hp},{"alive",car.fighter.alive},{"protected",car.fighter.protected_spawn},
    {"respawn_in",car.respawn_in},{"last_action",car.last_action}};
  if (!car.position_reason.empty()) self["position_reason"]=car.position_reason;
  Json entities=Json::array();
  const auto add=[&](const std::string &type,Cell cell) {
    return relative_entity(type,{cell.first-current.first,cell.second-current.second});
  };
  const auto visible=[&](Cell cell) { return car.fighter.alive && std::abs(cell.first-current.first)<=scenario_.radius && std::abs(cell.second-current.second)<=scenario_.radius; };
  for (Cell wall:current_walls()) if (visible(wall)) entities.push_back(add("obstacle",wall));
  for (const auto &[cell,value]:resources_) if (visible(cell)) {
    auto entity=add(value>=20 ? "resource_high" : "resource_normal",cell); entity["value"]=value; entities.push_back(entity);
  }
  for (int other=0;other<4;++other) if (other!=seat && cars_[other].fighter.alive && visible(cars_[other].fighter.position)) {
    auto entity=add("robot",cars_[other].fighter.position);
    entity["team"]=team(other); entity["robot_id"]=team(other); entity["hp"]=cars_[other].fighter.hp;
    entity["protected"]=cars_[other].fighter.protected_spawn;
    entity["defending"]=cars_[other].last_action.is_object() && cars_[other].last_action.value("action",std::string())=="defend";
    entities.push_back(entity);
  }
  std::string notices;
  for (const auto &a:scenario_.announcements) if (a.notice<=tick_) {
    if (!notices.empty()) notices+='\n';
    notices+=a.event.source;
  }
  return {{"session_id",scenario_.name},{"tick",tick_},{"vision_radius",scenario_.radius},
    {"map",{{"width",scenario_.width},{"height",scenario_.height}}},{"self",self},{"visible_entities",entities},
    {"score",{{"resource_value",car.resource},{"effective_damage",car.damage},{"mission_completed_ids",completed},{"invalid_actions",car.invalid}}},
    {"missions",scenario_.stage==2 ? scenario_.missions : Json::array()},{"event_text",notices}};
}
void Game::step(const std::array<rules::Action,4> &submitted) {
  using K=rules::ActionKind;
  auto actions=submitted;
  std::array<int,4> deaths{};
  std::array<bool,4> forced{},born{},new_dead{};
  const auto die=[&](int seat) {
    auto &car=cars_[seat];car.fighter.hp=0;car.fighter.alive=false;car.respawn_in=rules::respawn_wait;
    car.capture.clear();++car.deaths;++deaths[seat];new_dead[seat]=true;
  };
  for (auto &car:cars_) car.position_reason.clear();
  // 1–2：移动前环境；结构化安排只存在测试裁判里，选手接收有延迟的公告解释。
  for (const auto &a:scenario_.announcements) {
    const auto &event=a.event;
    if (event.start!=tick_) continue;
    for (int i=0;i<4;++i) if (cars_[i].fighter.alive && event.contains(cars_[i].fighter.position)) {
      auto &car=cars_[i];
      if (event.kind==EventKind::illusion) car.reversed.insert(event.key);
      if (event.kind==EventKind::explosion && !car.fighter.protected_spawn) {
        car.fighter.hp-=event.amount;if (car.fighter.hp<=0) die(i);
      }
      if (event.kind==EventKind::split && std::find(scenario_.spawns.begin(),scenario_.spawns.end(),car.fighter.position)==scenario_.spawns.end()) {
        die(i);forced[i]=true;
      }
    }
    if (event.kind==EventKind::split)
      for (int i=0;i<4;++i) if (forced[i]) born[i]=resurrect(i); // 同时落点按A–D。
  }
  refresh_effects();
  // 3：先冻结动作权限，再统一设置防御。缺动作等于裁判等待，不自动当防御。
  for (int i=0;i<4;++i) {
    auto &car=cars_[i];const auto action=actions[i];
    if (!car.fighter.alive || forced[i]) { actions[i]={};continue; }
    const bool direction_bad=(action.kind==K::move || action.kind==K::attack) && (action.direction<0 || action.direction>3);
    if (direction_bad || (scenario_.stage==1 && (action.kind==K::attack || action.kind==K::defend)) ||
        (action.kind==K::attack && car.fighter.protected_spawn)) { ++car.invalid;actions[i]={}; }
  }
  double reflection=0;
  for (const auto &a:scenario_.announcements)
    if (a.event.kind==EventKind::reflect && a.event.active(tick_)) reflection=std::max(reflection,a.event.amount);
  std::array<rules::Fighter,4> fighters;
  for (int i=0;i<4;++i) fighters[i]=cars_[i].fighter;
  // 4：同时战斗；死亡车取消移动。贡献、反伤、过量损血与分奖见共享确定规则。
  const auto combat=rules::combat(fighters,actions,reflection);
  for (int i=0;i<4;++i) {
    cars_[i].damage+=combat.earned[i];cars_[i].fighter=combat.fighters[i];
    if (combat.deaths[i]) die(i);
    fighters[i]=cars_[i].fighter;
  }
  auto effective=actions;
  for (int i=0;i<4;++i) if (effective[i].kind==K::move) {
    bool reverse=false;
    for (const auto &a:scenario_.announcements)
      reverse=reverse || (a.event.kind==EventKind::illusion && a.event.active(tick_) && cars_[i].reversed.count(a.event.key));
    if (reverse) effective[i].direction=(effective[i].direction+2)%4;
  }
  // 5：移动前占位，争空格全失败，死亡车已不占位。
  const auto moved=rules::move(fighters,effective,scenario_.width,scenario_.height,current_walls());
  for (int i=0;i<4;++i) {
    auto &car=cars_[i];car.fighter=fighters[i];
    if (moved[i]) { ++car.moves;if (car.fighter.position!=scenario_.spawns[i]) { car.protected_left=0;car.fighter.protected_spawn=false; } }
    else if (actions[i].kind==K::move && car.fighter.alive) ++car.blocked;
    car.attacks+=actions[i].kind==K::attack;car.defends+=actions[i].kind==K::defend;
    const char *kind=actions[i].kind==K::move ? "move" : actions[i].kind==K::attack ? "attack" : actions[i].kind==K::defend ? "defend" : "wait";
    car.last_action={{"tick",tick_},{"action",kind},{"moved",moved[i]}};
    if (moved[i]) car.last_action["effective_direction"]=rules::directions[effective[i].direction];
  }
  // 6：入点和辣椒批次；火线普通损血不计D，致死贡献者按席位去重均分60。
  std::array<std::array<double,4>,4> fire{};
  for (const auto &a:scenario_.announcements) {
    const auto &event=a.event;if (!event.active(tick_)) continue;
    for (int source=0;source<4;++source) {
      auto &car=cars_[source];if (!car.fighter.alive || !moved[source]) continue;
      if (event.kind==EventKind::vulnerability && tick_<event.trigger_end) car.vulnerable.insert(event.key);
      if (event.kind==EventKind::heal && event.contains(car.fighter.position)) {
        car.heal_until=tick_+event.effect_ticks;car.heal_amount=event.amount;
      }
      if (event.kind==EventKind::chili) for (Cell point:event.points) if (point==car.fighter.position)
        for (int target=0;target<4;++target) if (cars_[target].fighter.alive && !cars_[target].fighter.protected_spawn &&
            cars_[target].fighter.position!=point && (cars_[target].fighter.position.first==point.first || cars_[target].fighter.position.second==point.second)) fire[target][source]+=event.amount;
    }
  }
  for (int target=0;target<4;++target) if (cars_[target].fighter.alive) {
    double total=0;std::set<int> contributors;
    for (int source=0;source<4;++source) { total+=fire[target][source];if (fire[target][source]>0) contributors.insert(source); }
    cars_[target].fighter.hp=std::max(0.0,cars_[target].fighter.hp-total);
    if (cars_[target].fighter.hp<=0) {
      for (int source:contributors) cars_[source].damage+=rules::kill_reward/contributors.size();
      die(target);
    }
  }
  // 7：存活驻留与回血；新授予无双不能补打本Tick已经结算的攻击。
  for (int i=0;i<4;++i) {
    auto &car=cars_[i];
    for (const auto &a:scenario_.announcements) if (a.event.kind==EventKind::fortress && a.event.active(tick_)) {
      int &count=car.capture[a.event.key];count=car.fighter.alive && a.event.contains(car.fighter.position) ? count+1 : 0;
      if (count>=a.event.capture_ticks) car.empowered.insert(a.event.key);
    }
    if (car.fighter.alive && tick_<car.heal_until) car.fighter.hp=std::min(100.0,car.fighter.hp+car.heal_amount);
    // 8：本Tick全部奖励入账后按死亡次数扣除；环境死亡没有击杀奖励。
    car.damage=rules::adjusted_damage(car.damage,0,deaths[i]);
    // 9：资源、任务和复活。
    if (car.fighter.alive) {
      const auto resource=resources_.find(car.fighter.position);
      if (resource!=resources_.end()) {
        car.resource+=resource->second;if (resource->second>=20) ++car.high;else ++car.normal;resources_.erase(resource);
      }
      for (const auto &mission:scenario_.missions) {
        const auto id=mission.at("id").get<std::string>(),type=mission.at("type").get<std::string>();
        const double target=mission.value("target",1.0);
        if ((type=="normal_resources" && car.normal>=target) || (type=="high_resources" && car.high>=target) ||
            (type=="resource_value" && car.resource>=target) || (type=="effective_damage" && car.damage>=target) ||
            (type=="reach" && car.fighter.position==position(mission.at("position")))) car.completed.insert(id);
      }
      if (!born[i] && car.protected_left>0) { --car.protected_left;car.fighter.protected_spawn=car.protected_left>0; }
    } else if (!new_dead[i] && (scenario_.stage==2 || i==0)) {
      if (car.respawn_in>0) --car.respawn_in;
      if (car.respawn_in==0) born[i]=resurrect(i);
    }
  }
  ++tick_;refresh_effects();
}
double Game::score(int seat) const {
  const auto &car=cars_.at(seat);
  if (scenario_.stage==1) return 40*car.resource/200;
  return 30*std::min(car.resource/120,1.0)+15*std::min(car.damage/500,1.0)+
    (scenario_.simulated_ai_success ? 5*car.completed.size() : 0);
}
Json Game::result() const {
  Json result=Json::array();
  for (int i=0;i<4;++i) {
    const auto &car=cars_[i];
    result.push_back({{"seat",team(i)},{"score",score(i)},{"R",car.resource},{"D",car.damage},{"M",car.completed.size()},
      {"qualified_M",scenario_.simulated_ai_success ? car.completed.size() : 0},{"deaths",car.deaths},{"blocked",car.blocked},
      {"invalid",car.invalid},{"attacks",car.attacks},{"defends",car.defends},{"moves",car.moves},{"hp",car.fighter.hp}});
  }
  return result;
}
} // namespace rmtc::arena
