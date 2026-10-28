#include "arena.hpp"
#include <algorithm>
#include <queue>
#include <stdexcept>

namespace rmtc::arena {
Scenario make_scenario(const std::string &style,unsigned seed,int stage,int delay,bool ai_success) {
  if (stage!=1 && stage!=2) throw std::runtime_error("arena stage must be 1 or 2");
  Scenario s{style+"-"+std::to_string(seed)+"-s"+std::to_string(stage),style,seed,stage,24,24,
    stage==1 ? 100 : 500,4,delay,ai_success,{}, {}, {{{1,1},{22,1},{22,22},{1,22}}},{},Json::array()};
  std::mt19937 random(seed);
  const auto roll=[&](int limit) { return static_cast<int>(random()%static_cast<unsigned>(limit)); };
  if (style=="maze") {
    for (int y=0;y<24;++y) for (int x=0;x<24;++x) s.walls.insert({x,y});
    std::vector<Cell> stack{{1,1}};s.walls.erase({1,1});
    while (!stack.empty()) {
      const Cell cell=stack.back();std::vector<Cell> choices;
      for (Cell d:rules::steps) {
        const Cell next{cell.first+2*d.first,cell.second+2*d.second};
        if (next.first>0 && next.second>0 && next.first<23 && next.second<23 && s.walls.count(next)) choices.push_back(next);
      }
      if (choices.empty()) { stack.pop_back();continue; }
      const Cell next=choices[roll(static_cast<int>(choices.size()))];
      s.walls.erase({(cell.first+next.first)/2,(cell.second+next.second)/2});s.walls.erase(next);stack.push_back(next);
    }
  } else if (style=="rooms") {
    for (int line:{8,16}) for (int i=2;i<22;++i) { s.walls.insert({line,i});s.walls.insert({i,line}); }
    for (int line:{8,16}) for (int block=0;block<3;++block) {
      s.walls.erase({line,block*8+3+roll(3)});s.walls.erase({block*8+3+roll(3),line});
    }
  } else if (style=="lanes") {
    for (int y:{4,8,12,16,20}) {
      const int gap=3+roll(17);
      for (int x=2;x<22;++x) if (std::abs(x-gap)>1) s.walls.insert({x,y});
    }
  } else if (style=="crossroads") {
    for (int i=2;i<22;++i) { s.walls.insert({12,i});s.walls.insert({i,12}); }
    for (int gap:{4,19}) { s.walls.erase({12,gap});s.walls.erase({gap,12}); }
  } else if (style=="open" || style=="clusters") {
    for (int i=0;i<18;++i) s.walls.insert({3+roll(18),3+roll(18)});
  } else throw std::runtime_error("unknown arena map style");
  // 保证四出生点连通；不把这条测试端通路送进选手感知。
  for (int i=1;i<=22;++i) { s.walls.erase({i,1});s.walls.erase({i,22});s.walls.erase({1,i});s.walls.erase({22,i}); }
  std::set<Cell> reachable;std::queue<Cell> pending;pending.push(s.spawns[0]);reachable.insert(s.spawns[0]);
  while (!pending.empty()) {
    const Cell cell=pending.front();pending.pop();
    for (Cell d:rules::steps) {
      Cell next{cell.first+d.first,cell.second+d.second};
      if (next.first>=0 && next.second>=0 && next.first<24 && next.second<24 && !s.walls.count(next) && reachable.insert(next).second) pending.push(next);
    }
  }
  std::vector<Cell> free;
  for (Cell cell:reachable) if (std::find(s.spawns.begin(),s.spawns.end(),cell)==s.spawns.end()) free.push_back(cell);
  std::shuffle(free.begin(),free.end(),random);
  if (style=="clusters") {
    const std::array<Cell,3> centers={free[0],free[free.size()/3],free[free.size()*2/3]};
    const auto distance=[&](Cell cell) { int d=1000;for (Cell center:centers) d=std::min(d,rules::manhattan(cell,center));return d; };
    std::stable_sort(free.begin(),free.end(),[&](Cell a,Cell b) { return distance(a)<distance(b); });
  }
  for (int i=0;i<16;++i) s.resources[free.at(i)]=i<4 ? 20 : 10; // 正式基准：4高+12普通=200R。
  if (stage==1) return s;
  const Cell reach=free.at(20+roll(20));
  s.missions={{{"id","normal"},{"type","normal_resources"},{"target",2}},
    {{"id","high"},{"type","high_resources"},{"target",1}},
    {{"id","reach"},{"type","reach"},{"target",1},{"position",{reach.first,reach.second}}}};
  if (seed%3==1) s.missions[1]={{"id","damage"},{"type","effective_damage"},{"target",80}};
  if (seed%3==2) s.missions[0]={{"id","resource"},{"type","resource_value"},{"target",40}};
  const auto add=[&](EventKind kind,int start,int duration,double amount,Cell center,int radius,const char *name) {
    Event e;e.kind=kind;e.start=start;e.end=start+duration;e.amount=amount;
    e.area={std::max(0,center.first-radius),std::max(0,center.second-radius),std::min(23,center.first+radius),std::min(23,center.second+radius)};
    e.key=std::string(name)+"-"+std::to_string(seed)+"-"+std::to_string(start);
    const int notice=std::max(0,start-(5+roll(9)));
    e.source="第"+std::to_string(notice)+"Tick公告："+name+"，Tick"+std::to_string(start)+"生效，Tick"+std::to_string(e.end)+
      "结束，矩形["+std::to_string(e.area[0])+","+std::to_string(e.area[1])+"]至["+std::to_string(e.area[2])+","+std::to_string(e.area[3])+"]。";
    s.announcements.push_back({e,notice});
  };
  add(EventKind::explosion,30+roll(16),1,seed%2 ? 80 : 35,free[roll(static_cast<int>(free.size()))],3,"爆炸");
  add(EventKind::illusion,65+roll(15),35,0,free[roll(static_cast<int>(free.size()))],5,"幻术");
  add(EventKind::heal,100+roll(15),20,10,free[10],0,"天使恩泽");
  s.announcements.back().event.points={free[10],free[11]};s.announcements.back().event.effect_ticks=10;
  add(EventKind::fortress,145+roll(15),65,50,free[25],1,"天下无双");
  s.announcements.back().event.capture_ticks=8;s.announcements.back().event.defended_damage=10;
  add(EventKind::split,225+roll(15),30,0,free[30],0,"世界分裂");
  s.announcements.back().event.points={free[30]};
  add(EventKind::chili,275+roll(15),30,50,free[35],0,"火爆辣椒");s.announcements.back().event.points={free[35],free[36],free[37]};
  add(EventKind::reflect,330+roll(15),20,20,{0,0},0,"洁身自好");
  add(EventKind::vulnerability,390+roll(15),30,0,{0,0},0,"潘潘的神");
  s.announcements.back().event.trigger_end=s.announcements.back().event.start+20;s.announcements.back().event.multiplier=2;
  return s;
}
} // namespace rmtc::arena
