// 离线裁判的世界快照。只供场景工具链接；正式机器人不包含本文件。
#include "arena.hpp"
#include <cmath>
#include <stdexcept>

namespace rmtc::arena {
namespace {
// 有限数/整数分开检查，避免把 200.5 Tick 或负血量偷偷变成合法状态。
int integer(const Json &value,int low,int high) {
  if(!value.is_number_integer() || value.get<double>()<low || value.get<double>()>high)
    throw std::runtime_error("checkpoint integer out of range");
  return value.get<int>();
}
double number(const Json &value,double low,double high) {
  if(!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>()<low || value.get<double>()>high)
    throw std::runtime_error("checkpoint number out of range");
  return value.get<double>();
}
Cell cell(const Json &value,int width,int height) {
  if(!value.is_array() || value.size()!=2) throw std::runtime_error("checkpoint position must be [x,y]");
  return {integer(value[0],0,width-1),integer(value[1],0,height-1)};
}
std::string seat_name(int seat) { return std::string(1,static_cast<char>('A'+seat)); }
// 集合必须去重；关联事件还要存在且类型匹配，不能从不存在的增益凭空获得能力。
std::set<std::string> names(const Json &value) {
  if(!value.is_array()) throw std::runtime_error("checkpoint names must be an array");
  std::set<std::string> result;
  for(const auto &entry:value) {
    const auto key=entry.get<std::string>();
    if(key.empty() || !result.insert(key).second) throw std::runtime_error("checkpoint empty/duplicate name");
  }
  return result;
}
Json encode_car(const Car &car) {
  return {{"position",{car.fighter.position.first,car.fighter.position.second}},
    {"hp",car.fighter.hp},{"alive",car.fighter.alive},{"protected",car.fighter.protected_spawn},
    {"R",car.resource},{"D",car.damage},{"normal",car.normal},{"high",car.high},
    {"deaths",car.deaths},{"blocked",car.blocked},{"invalid",car.invalid},
    {"respawn_in",car.respawn_in},{"protected_left",car.protected_left},
    {"attacks",car.attacks},{"defends",car.defends},{"moves",car.moves},
    {"heal_until",car.heal_until},{"heal_amount",car.heal_amount},
    {"position_reason",car.position_reason},{"last_action",car.last_action},
    {"completed",car.completed},{"reversed",car.reversed},{"vulnerable",car.vulnerable},
    {"empowered",car.empowered},{"capture",car.capture}};
}
} // namespace

Json Game::checkpoint() const {
  Json cars=Json::object(),resources=Json::array();
  for(int i=0;i<4;++i) cars[seat_name(i)]=encode_car(cars_[i]);
  for(const auto &[position,value]:resources_)
    resources.push_back({{"position",{position.first,position.second}},{"value",value}});
  return {{"schema",1},{"phase","before_tick"},{"tick",tick_},{"cars",cars},{"resources",resources}};
}

void Game::restore(const Json &state) {
  if(state.at("schema")!=1 || state.at("phase")!="before_tick")
    throw std::runtime_error("checkpoint requires schema=1 and phase=before_tick");
  const int next_tick=integer(state.at("tick"),0,scenario_.ticks);
  auto cars=cars_; // 所有校验都先作用于副本；失败时不留下半恢复的世界。
  std::map<Cell,int> resources;
  for(const auto &entry:state.at("resources")) {
    const Cell p=cell(entry.at("position"),scenario_.width,scenario_.height);
    if(scenario_.walls.count(p) || !resources.emplace(p,integer(entry.at("value"),1,1000000)).second)
      throw std::runtime_error("checkpoint resource overlaps wall/duplicate resource");
  }
  std::set<Cell> occupied;
  std::set<std::string> mission_ids;
  for(const auto &mission:scenario_.missions) mission_ids.insert(mission.at("id").get<std::string>());
  const auto validate_effects=[&](const std::set<std::string> &keys,EventKind kind) {
    for(const auto &key:keys) {
      bool found=false;
      for(const auto &a:scenario_.announcements) found=found || (a.event.key==key && a.event.kind==kind);
      if(!found) throw std::runtime_error("checkpoint effect references unknown/wrong event: "+key);
    }
  };
  for(int i=0;i<4;++i) {
    const auto &j=state.at("cars").at(seat_name(i));auto &car=cars[i];
    car.fighter.position=cell(j.at("position"),scenario_.width,scenario_.height);
    car.fighter.hp=number(j.at("hp"),0,100);car.fighter.alive=j.at("alive").get<bool>();
    car.fighter.protected_spawn=j.at("protected").get<bool>();
    if(car.fighter.alive!=(car.fighter.hp>0)) throw std::runtime_error("checkpoint alive/hp disagree");
    if(car.fighter.alive && (scenario_.walls.count(car.fighter.position) || !occupied.insert(car.fighter.position).second))
      throw std::runtime_error("checkpoint alive car overlaps wall/other car");
    car.resource=number(j.at("R"),0,1000000);car.damage=number(j.at("D"),0,1000000000);
#define COUNT(field) car.field=integer(j.at(#field),0,1000000000)
    COUNT(normal);COUNT(high);COUNT(deaths);COUNT(blocked);COUNT(invalid);COUNT(respawn_in);
    COUNT(protected_left);COUNT(attacks);COUNT(defends);COUNT(moves);
#undef COUNT
    if(car.fighter.protected_spawn!=(car.protected_left>0) || (car.fighter.alive && car.respawn_in!=0))
      throw std::runtime_error("checkpoint protection/respawn fields disagree");
    car.heal_until=integer(j.at("heal_until"),-1,1000000000);car.heal_amount=number(j.at("heal_amount"),0,10000);
    car.position_reason=j.at("position_reason").get<std::string>();car.last_action=j.at("last_action");
    if(!car.last_action.is_null() && integer(car.last_action.at("tick"),0,1000000000)!=next_tick-1)
      throw std::runtime_error("checkpoint last_action must belong to preceding tick");
    car.completed=names(j.at("completed"));
    for(const auto &key:car.completed) if(!mission_ids.count(key)) throw std::runtime_error("checkpoint unknown completed mission");
    car.reversed=names(j.at("reversed"));car.vulnerable=names(j.at("vulnerable"));car.empowered=names(j.at("empowered"));
    validate_effects(car.reversed,EventKind::illusion);validate_effects(car.vulnerable,EventKind::vulnerability);
    validate_effects(car.empowered,EventKind::fortress);car.capture.clear();
    for(const auto &entry:j.at("capture").items()) {
      validate_effects({entry.key()},EventKind::fortress);car.capture[entry.key()]=integer(entry.value(),0,1000000);
    }
  }
  cars_=std::move(cars);resources_=std::move(resources);tick_=next_tick;
  refresh_effects(); // 伤害/无双/易伤由已保存的授予记录和当前时间重新计算。
}
} // namespace rmtc::arena
