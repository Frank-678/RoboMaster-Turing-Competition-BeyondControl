#include "arena.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace rmtc::arena {
namespace {
Cell grid_offset(const std::string &grid) {
  for (int y=0;y<9;++y) for (int x=0;x<9;++x) if (grid.at(y*10+x)=='2') return {x-4,y-4};
  return {0,0};
}
}
Opponent::Opponent(std::string style,unsigned seed,int stage):style_(std::move(style)),stage_(stage),random_(seed) {}
rules::Action Opponent::act(const Json &p,const std::vector<Event> &known) {
  using K=rules::ActionKind;
  const auto &self=p.at("self");if (!self.at("alive").get<bool>()) return {};
  const Cell start{self.at("position").at(0).get<int>(),self.at("position").at(1).get<int>()};
  const int tick=p.at("tick"),width=p.at("map").at("width"),height=p.at("map").at("height"),radius=p.at("vision_radius");
  positions_[tick]=start;
  const bool protected_spawn=self.value("protected",false);
  const bool stage1=stage_==1;
  struct Enemy {Cell position;double hp;bool protected_spawn;};std::vector<Enemy> enemies;std::set<Cell> occupied;
  for (int y=std::max(0,start.second-radius);y<=std::min(height-1,start.second+radius);++y)
    for (int x=std::max(0,start.first-radius);x<=std::min(width-1,start.first+radius);++x) {
      auto &tile=memory_[{x,y}];tile.wall=false;tile.value=0;
    }
  for (const auto &entity:p.at("visible_entities")) {
    const Cell delta=grid_offset(entity.at("position"));const Cell cell{start.first+delta.first,start.second+delta.second};
    const auto type=entity.at("type").get<std::string>();
    if (type=="obstacle") memory_[cell].wall=true;
    if (type=="resource_normal" || type=="resource_high") memory_[cell].value=entity.at("value").get<int>();
    if (type=="robot") { occupied.insert(cell);enemies.push_back({cell,entity.at("hp").get<double>(),entity.value("protected",false)}); }
  }
  ++memory_[start].visits;
  bool reverse=false;double reflection=0;
  for (const auto &e:known) {
    if (!e.active(tick)) continue;
    if (e.kind==EventKind::illusion && positions_.count(e.start) && e.contains(positions_.at(e.start))) reverse=true;
    if (e.kind==EventKind::reflect) reflection=e.amount;
  }
  const auto blocked=[&](Cell cell,int time) {
    if (cell.first<0 || cell.second<0 || cell.first>=width || cell.second>=height) return true;
    const auto tile=memory_.find(cell);if (tile!=memory_.end() && tile->second.wall) return true;
    for (const auto &e:known)
      if (e.kind==EventKind::split && e.active(time) && e.contains(cell) && cell!=positions_.begin()->second) return true;
    return false;
  };
  const auto move=[&](int direction) { return rules::Action{K::move,reverse ? (direction+2)%4 : direction}; };
  int incoming=0,best_attack=-1;double attack_value=0;
  for (int d=0;d<4;++d) {
    const Enemy *nearest=nullptr;
    for (const auto &enemy:enemies) if (rules::ray(start,enemy.position)==d &&
        (!nearest || rules::manhattan(start,enemy.position)<rules::manhattan(start,nearest->position))) nearest=&enemy;
    if (nearest && !nearest->protected_spawn) {
      incoming+=20;const double value=std::min(20.0,nearest->hp)+(nearest->hp<=20 ? 60 : 0);
      if (value>attack_value) { attack_value=value;best_attack=d; }
    }
  }
  if (!stage1 && incoming>=self.at("hp").get<double>() && incoming/2<self.at("hp").get<double>()) return {K::defend,-1};
  if (!stage1 && !protected_spawn && best_attack>=0 && reflection==0 &&
      (style_=="aggressive" || style_=="fortress" || attack_value>=60 || (style_=="opportunist" && self.at("hp").get<double>()>35))) return {K::attack,best_attack};
  struct Route {int distance,first;};std::map<Cell,Route> paths;std::queue<Cell> pending;paths[start]={0,-1};pending.push(start);
  while (!pending.empty()) {
    const Cell cell=pending.front();pending.pop();const auto route=paths.at(cell);
    for (int d=0;d<4;++d) {
      const Cell next{cell.first+rules::steps[d].first,cell.second+rules::steps[d].second};
      if (!memory_.count(next) || blocked(next,tick+route.distance) || occupied.count(next) || paths.count(next)) continue;
      paths[next]={route.distance+1,route.first<0 ? d : route.first};pending.push(next);
    }
  }
  // 所有陪练都只使用已公告且延迟交付的危险；不查看未来事件安排。
  for (const auto &e:known) if ((e.kind==EventKind::explosion || e.kind==EventKind::split) && e.start>tick && e.contains(start)) {
    int distance=10000,first=-1;
    for (const auto &[cell,path]:paths) if (!e.contains(cell) && path.distance<distance) { distance=path.distance;first=path.first; }
    if (first>=0 && e.start-tick<=distance+2) return move(first);
  }
  double best=-1;int direction=-1;
  for (const auto &[cell,path]:paths) if (path.first>=0) {
    const auto &tile=memory_.at(cell);double utility=tile.value/(path.distance+1.0);
    if (style_!="collector" && p.at("score").at("resource_value").get<double>()>=120) utility*=0.01;
    if (style_=="defender") for (const auto &enemy:enemies) if (rules::ray(enemy.position,cell)>=0) utility*=0.1;
    for (const auto &mission:p.at("missions")) {
      if (mission.at("type")=="reach" && cell==Cell{mission.at("position")[0].get<int>(),mission.at("position")[1].get<int>()}) {
        bool done=false;for (const auto &id:p.at("score").at("mission_completed_ids")) done=done || id==mission.at("id");
        if (!done) utility+=5.0/(path.distance+1);
      }
    }
    if (style_=="aggressive") for (const auto &enemy:enemies)
      if (!enemy.protected_spawn && rules::ray(cell,enemy.position)>=0) utility+=6.0/(path.distance+1);
    if (style_=="fortress") for (const auto &e:known)
      if (e.kind==EventKind::fortress && e.active(tick) && e.contains(cell)) utility+=20.0/(path.distance+1);
    if (utility>best) { best=utility;direction=path.first; }
  }
  if (style_=="fortress") for (const auto &e:known) if (e.kind==EventKind::fortress && e.active(tick) && e.contains(start)) return {K::defend,-1};
  if (best>0 && direction>=0 && style_!="wanderer") return move(direction);
  if (!stage1 && !protected_spawn && best_attack>=0 && reflection==0 && style_!="defender") return {K::attack,best_attack};
  std::vector<int> choices;unsigned visits=std::numeric_limits<unsigned>::max();
  for (int d=0;d<4;++d) {
    const Cell next{start.first+rules::steps[d].first,start.second+rules::steps[d].second};
    if (blocked(next,tick) || occupied.count(next)) continue;
    const unsigned n=memory_.count(next) ? memory_.at(next).visits : 0;
    if (n<visits) { visits=n;choices.clear(); }
    if (n==visits) choices.push_back(d);
  }
  if (!choices.empty()) return move(choices[random_()%choices.size()]);
  return stage1 ? rules::Action{} : rules::Action{K::defend,-1};
}
} // namespace rmtc::arena
