#include "rmtc_bot/tactics.hpp"
#include "rmtc_bot/scoring.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace rmtc {
namespace { // 本文件的状态副本、推进和估值，不向通信层暴露。
using K=rules::ActionKind;
struct State { // 一条候选在一个对手情景下的局面，绝不改真实观测。
  std::array<rules::Fighter,4> fighters;
  std::map<Cell,double> resources;
  double r,d;
  int normal,high,deaths=0;
  double task_reward=0;
  std::vector<bool> done;
  std::map<std::string,int> capture;
  std::array<Tick,4> heal_until{};
  std::array<double,4> heal_amount{};
};
// 候选移动只用已知可走格、当前存活占位和已知分裂墙。
bool can_move(const TacticalInput &input,const State &state,int player,Cell cell,Tick tick) {
  if (!input.walkable.count(cell)) return false;
  for (int i=0;i<4;++i) if (state.fighters[i].alive && state.fighters[i].position==cell) return false;
  for (const auto &event:input.events)
    if (event.kind==EventKind::split && event.active(tick) && event.contains(cell) && (!input.spawn || cell!=*input.spawn)) return false;
  (void)player;return true;
}
// 首层列举有意义且通过避险筛选的动作；保护车不能攻击。
std::vector<rules::Action> choices(const TacticalInput &input,const State &state,Tick tick,bool first) {
  std::vector<rules::Action> actions;
  if (!state.fighters[0].alive) return {{}};
  for (int d=0;d<4;++d) {
    const Cell start=state.fighters[0].position,step=rules::steps[d],next{start.first+step.first,start.second+step.second};
    if ((!first || input.safe_first[d]) && can_move(input,state,0,next,tick)) actions.push_back({K::move,d});
  }
  if ((!first || input.safe_first[4]) && !state.fighters[0].protected_spawn) {
    const auto &self=state.fighters[0];
    for (int d=0;d<4;++d) {
      int nearest=-1;
      for (int i=1;i<4;++i) if (state.fighters[i].alive) {
        const auto &target=state.fighters[i];
        if (self.area_attack && !target.protected_spawn && std::abs(target.position.first-self.position.first)<=2 &&
            std::abs(target.position.second-self.position.second)<=2) nearest=i;
        else if (!self.area_attack && rules::ray(self.position,target.position)==d &&
            (nearest<0 || rules::manhattan(self.position,target.position)<rules::manhattan(self.position,state.fighters[nearest].position))) nearest=i;
      }
      if (nearest>=0 && !state.fighters[nearest].protected_spawn) actions.push_back({K::attack,d});
      if (self.area_attack) break; // 无双四个方向是同一动作集合，不重复占束宽。
    }
  }
  if (!first || input.safe_first[4]) actions.push_back({K::defend,-1});
  return actions;
}
// mode依次是直追、驻守、轨迹延续、抢资源、侧向接近；额外mode5检验潜在无双。
rules::Action opponent_action(const TacticalInput &input,const State &state,int player,int mode,Tick tick) {
  const auto &self=state.fighters[player];const auto &us=state.fighters[0];
  if (!self.alive) return {};
  const int hit=rules::ray(self.position,us.position);
  if (mode==1) return {K::defend,-1}; // 本Tick防御情景，反伤判断也用这个真实候选。
  if (mode==2 && input.forecast[player] && tick==input.tick) {
    const Cell target=*input.forecast[player];
    if (target==self.position) return {K::defend,-1};
    if (can_move(input,state,player,target,tick)) return {K::move,rules::ray(self.position,target)};
  }
  if (!self.protected_spawn && us.alive && ((self.area_attack && std::abs(self.position.first-us.position.first)<=2 &&
      std::abs(self.position.second-us.position.second)<=2) || hit>=0) && mode!=3) return {K::attack,hit>=0 ? hit : 0};
  double best=-1e9;int direction=-1;
  for (int d=0;d<4;++d) {
    const Cell step=rules::steps[d],next{self.position.first+step.first,self.position.second+step.second};
    if (!can_move(input,state,player,next,tick)) continue;
    double utility=-rules::manhattan(next,us.position);
    if (mode==3) {
      utility=-100;
      for (const auto &[cell,value]:state.resources) utility=std::max(utility,value/(rules::manhattan(next,cell)+1));
    }
    if (mode==4) utility+=(d%2 ? 0.2 : 0); // 合法侧向接近，覆盖与直追不同的争格。
    if (utility>best) { best=utility;direction=d; }
  }
  return direction>=0 ? rules::Action{K::move,direction} : rules::Action{K::defend,-1};
}
// 按激活伤害→同时战斗→存活者移动→环境/回血/堡垒→计分任务推进一Tick。
State advance(const TacticalInput &input,State state,rules::Action action,int mode,Tick tick) {
  std::array<int,4> deaths{};
  double reflection=0;
  bool fortress_active=false,vulnerability_active=false;
  for (const auto &event:input.events) if (event.active(tick)) {
    fortress_active=fortress_active || event.kind==EventKind::fortress;
    vulnerability_active=vulnerability_active || event.kind==EventKind::vulnerability;
  }
  for (auto &car:state.fighters) {
    if (!fortress_active) {car.area_attack=false;car.attack=20;car.defended=10;}
    if (!vulnerability_active) car.vulnerability=1;
  }
  for (const auto &event:input.events) {
    if (!event.active(tick)) continue;
    if (event.kind==EventKind::reflect) reflection=std::max(reflection,event.amount);
    if (event.kind==EventKind::explosion && event.start==tick)
      for (int i=0;i<4;++i) if (state.fighters[i].alive && !state.fighters[i].protected_spawn && event.contains(state.fighters[i].position)) {
        auto &car=state.fighters[i];car.hp=std::max(0.0,car.hp-event.amount);
        if (car.hp==0) { car.alive=false;++deaths[i]; }
      }
    // 分裂激活时不猜未知的敌方出生点；该情形由原策略确定避险负责，调用处跳过三Tick战术搜索。
  }
  std::array<rules::Action,4> actions{};actions[0]=action;
  if (state.fighters[0].protected_spawn && action.kind==K::attack) actions[0]={};
  for (int i=1;i<4;++i) actions[i]=opponent_action(input,state,i,mode,tick);
  if (mode==5 && input.possible_enemy_area && fortress_active) for (int i=1;i<4;++i) if (state.fighters[i].alive) {
    state.fighters[i].area_attack=true;state.fighters[i].attack=input.possible_area_damage;
    actions[i]=opponent_action(input,state,i,0,tick);
  }
  const auto combat=rules::combat(state.fighters,actions,reflection);
  state.fighters=combat.fighters;double earned=combat.earned[0];
  for (int i=0;i<4;++i) deaths[i]+=combat.deaths[i];
  std::set<Cell> blocked;
  // 只检查至多4个拟移动落点，不扫描整幅未知地图；未知目的地视为不可用于精确推演。
  for (int i=0;i<4;++i) if (actions[i].kind==K::move && actions[i].direction>=0 && actions[i].direction<4) {
    const Cell at=state.fighters[i].position,step=rules::steps[actions[i].direction],next{at.first+step.first,at.second+step.second};
    if (!input.walkable.count(next)) blocked.insert(next);
    for (const auto &event:input.events) if (event.kind==EventKind::split && event.active(tick) && event.contains(next) &&
        (!input.spawn || next!=*input.spawn)) blocked.insert(next);
  }
  const auto moved=rules::move(state.fighters,actions,input.width,input.height,blocked);
  for (int i=0;i<4;++i) if (moved[i] && (i!=0 || (input.spawn && state.fighters[i].position!=*input.spawn))) state.fighters[i].protected_spawn=false;
  std::array<std::array<double,4>,4> fire{};
  for (const auto &event:input.events) if (event.active(tick)) {
    for (int source=0;source<4;++source) if (state.fighters[source].alive && moved[source]) {
      auto &car=state.fighters[source];
      if (event.kind==EventKind::heal && event.contains(car.position)) { state.heal_until[source]=tick+event.effect_ticks;state.heal_amount[source]=event.amount; }
      if (event.kind==EventKind::vulnerability && tick<event.trigger_end) car.vulnerability=std::max(car.vulnerability,event.multiplier); // 只影响下一次advance的战斗。
      if (event.kind==EventKind::chili) for (Cell point:event.points) if (point==car.position)
        for (int target=0;target<4;++target) if (state.fighters[target].alive && !state.fighters[target].protected_spawn && state.fighters[target].position!=point &&
            (state.fighters[target].position.first==point.first || state.fighters[target].position.second==point.second)) fire[target][source]+=event.amount;
    }
  }
  for (int target=0;target<4;++target) if (state.fighters[target].alive) {
    double damage=0;std::set<int> contributors;
    for (int source=0;source<4;++source) { damage+=fire[target][source];if (fire[target][source]>0) contributors.insert(source); }
    auto &car=state.fighters[target];car.hp=std::max(0.0,car.hp-damage);
    if (car.hp==0) {car.alive=false;++deaths[target];if (contributors.count(0)) earned+=60.0/contributors.size();}
  }
  for (int i=0;i<4;++i) if (state.fighters[i].alive && tick<state.heal_until[i]) state.fighters[i].hp=std::min(100.0,state.fighters[i].hp+state.heal_amount[i]);
  for (const auto &event:input.events) if (event.kind==EventKind::fortress && event.active(tick)) {
    int &count=state.capture[event.key];count=state.fighters[0].alive && event.contains(state.fighters[0].position) ? count+1 : 0;
    if (count>=event.capture_ticks) {state.fighters[0].area_attack=true;state.fighters[0].attack=event.amount;state.fighters[0].defended=event.defended_damage;}
  }
  state.d=rules::adjusted_damage(state.d,earned,deaths[0]);state.deaths+=deaths[0];
  for (int i=0;i<4;++i) if (state.fighters[i].alive) {
    const auto resource=state.resources.find(state.fighters[i].position);
    if (resource!=state.resources.end()) {
      if (i==0) {state.r+=resource->second;if (resource->second>=20) ++state.high;else ++state.normal;}
      state.resources.erase(resource);
    }
  }
  if (state.fighters[0].alive) for (std::size_t i=0;i<input.missions.size();++i) if (!state.done[i]) {
    const auto &task=input.missions[i];
    if ((task.type=="reach" && state.fighters[0].position==task.position) || (task.type=="resource_value" && state.r>=task.target) ||
        (task.type=="effective_damage" && state.d>=task.target) || (input.counts_known &&
        ((task.type=="normal_resources" && state.normal>=task.target) || (task.type=="high_resources" && state.high>=task.target)))) {
      state.done[i]=true;if (input.model_reply_seen) state.task_reward+=5; // 正式资格仍由裁判认定，这里只使用已见模型回复的收益假设。
    }
  }
  return state;
}
// R/D只按官方封顶公式计分；HP和死亡机会成本是可调启发式。
double value(const TacticalInput &input,const State &state,const StrategyParameters &p) {
  return score_delta(input.resource_total,state.r-input.resource_total,120,30)+
    score_delta(input.damage_total,state.d-input.damage_total,500,15)+state.task_reward+
    p.tactical_hp_value*(state.fighters[0].hp-input.fighters[0].hp)-p.tactical_death_cost*state.deaths;
}
// 情景均值与最差值混合，权重不是经校准的概率。
double aggregate(const std::vector<double> &values,double risk) {
  double mean=0,worst=std::numeric_limits<double>::infinity();
  for (double value:values) {mean+=value;worst=std::min(worst,value);}
  return (1-risk)*mean/values.size()+risk*worst; // 情景加权取舍，不称真实概率或纳什最优。
}
}
TacticalChoice tactical_choice(const TacticalInput &input,const StrategyParameters &p) {
  State initial{input.fighters,input.resources,input.resource_total,input.damage_total,input.normal_count,input.high_count,
    0,0,std::vector<bool>(input.missions.size(),false),input.capture_progress,{},{}};
  initial.heal_until[0]=input.healing_until;initial.heal_amount[0]=input.healing_amount;
  const int scenarios=input.possible_enemy_area ? 6 : 5;
  struct Branch {
    rules::Action first;
    std::vector<State> states;
    std::vector<double> discounted;
    double utility=0;
  };
  Branch root;root.states.assign(scenarios,initial);root.discounted.assign(scenarios,0);
  std::vector<Branch> beam{root};int simulated=0;
  const std::vector<rules::Action> continuation={{K::move,0},{K::move,1},{K::move,2},{K::move,3},
    {K::attack,0},{K::attack,1},{K::attack,2},{K::attack,3},{K::defend,-1}};
  const auto action_key=[](rules::Action a) {return a.kind==K::move ? a.direction : a.kind==K::attack ? 4+a.direction : 8;};
  struct GoalDistance { Cell target;double weight;int initial;std::map<Cell,int> distances; };
  std::vector<GoalDistance> goals;
  for (int d=0;d<4;++d) if (input.goal_position[d] && input.goal_value[d]>0) {
    GoalDistance goal{*input.goal_position[d],std::min(5.0,input.goal_value[d]),0,{}};
    if (input.walkable.count(goal.target)) {
      std::queue<Cell> pending;goal.distances[goal.target]=0;pending.push(goal.target);
      while (!pending.empty()) {
        const Cell cell=pending.front();pending.pop();
        for (Cell step:rules::steps) {
          const Cell next{cell.first+step.first,cell.second+step.second};
          if (input.walkable.count(next) && !goal.distances.count(next)) {goal.distances[next]=goal.distances.at(cell)+1;pending.push(next);}
        }
      }
    }
    const auto at=goal.distances.find(input.fighters[0].position);
    goal.initial=at==goal.distances.end() ? rules::manhattan(input.fighters[0].position,goal.target) : at->second;
    goals.push_back(std::move(goal));
  }
  const auto tail_value=[&](const State &state,Tick time) {
    if (!state.fighters[0].alive) return 0.0;
    double best=0;const Cell cell=state.fighters[0].position;
    for (const auto &goal:goals) {
      const auto at=goal.distances.find(cell);
      const int distance=at==goal.distances.end() ? rules::manhattan(cell,goal.target) : at->second;
      best=std::max(best,goal.weight*std::clamp((goal.initial-distance)/3.0,0.0,1.0));
    }
    if (cell==input.fighters[0].position && input.goal_value[4]>0)
      for (const auto &event:input.events) if (event.kind==EventKind::fortress && event.active(time) && event.contains(cell))
        best=std::max(best,std::min(5.0,input.goal_value[4]));
    return best;
  };
  const int horizon=static_cast<int>(std::min<Tick>(3,input.remaining_ticks));
  for (int depth=0;depth<horizon;++depth) {
    std::vector<Branch> expanded;
    for (const auto &branch:beam) {
      const auto options=depth==0 ? choices(input,initial,input.tick,true) : continuation;
      for (auto action:options) {
        Branch child;child.first=depth==0 ? action : branch.first;
        for (int mode=0;mode<scenarios;++mode) {
          auto state=advance(input,branch.states[mode],action,mode,input.tick+depth);++simulated;
          child.discounted.push_back(branch.discounted[mode]+std::pow(p.tactical_future_discount,depth)*
            (value(input,state,p)-value(input,branch.states[mode],p)));
          child.states.push_back(std::move(state));
        }
        std::vector<double> tails;for (const auto &state:child.states) tails.push_back(tail_value(state,input.tick+depth));
        child.utility=aggregate(child.discounted,p.tactical_risk)+p.tactical_goal_weight*aggregate(tails,p.tactical_risk);
        // 低血量攻击折扣（与strategy.cpp§6同口径）：hp<60时压缩深度0攻击分支估值，1.0=旧行为。
        if (p.low_hp_attack_discount < 1.0 && depth == 0 && action.kind == K::attack &&
            input.fighters[0].hp < 60) child.utility *= p.low_hp_attack_discount;
        expanded.push_back(std::move(child));
      }
    }
    if (expanded.empty()) return {{},-1e9,scenarios,simulated};
    std::stable_sort(expanded.begin(),expanded.end(),[](const Branch &a,const Branch &b){return a.utility>b.utility;});
    std::vector<Branch> selected;std::set<int> used;
    std::vector<bool> picked(expanded.size(),false);
    for (int pass=0;pass<2;++pass) for (std::size_t i=0;i<expanded.size() && selected.size()<5;++i) {
      if (picked[i] || (pass==0 && used.count(action_key(expanded[i].first)))) continue;
      used.insert(action_key(expanded[i].first));picked[i]=true;selected.push_back(std::move(expanded[i]));
    }
    beam=std::move(selected);
  }
  std::stable_sort(beam.begin(),beam.end(),[](const Branch &a,const Branch &b){return a.utility>b.utility;});
  return {beam.front().first,beam.front().utility,scenarios,simulated};
}
} // namespace rmtc
