#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <set>
#include <vector>
#include "rmtc_bot/events.hpp"

namespace rmtc::rules { // v1.2的确定结算；比赛策略和测试端共用，数值不参与调参。
inline constexpr double max_hp = 100, ordinary_damage = 20, defended_damage = 10;
inline constexpr double kill_reward = 60, death_loss = 60;
inline constexpr int respawn_wait = 3, protection_ticks = 2;
inline constexpr std::array<Cell, 4> steps = {{{0,-1},{1,0},{0,1},{-1,0}}}; // 上、右、下、左。
inline constexpr std::array<const char *, 4> directions = {{"up","right","down","left"}};
enum class ActionKind { none, move, attack, defend }; // none是不提交/裁判等待，不是选手默认WAIT。
struct Action {
  ActionKind kind = ActionKind::none;
  int direction = -1; // MOVE/ATTACK为0–3，其余不使用。
};
struct Fighter {
  Cell position{};
  double hp = max_hp, attack = ordinary_damage, defended = defended_damage, vulnerability = 1;
  bool alive = true, protected_spawn = false, area_attack = false;
};
struct CombatResult {
  std::array<Fighter, 4> fighters; // 战斗后位置不变，HP/存活已统一更新。
  std::array<double, 4> earned{}; // 本战斗批次D贡献及击杀奖励；死亡扣除由整个Tick最后统一处理。
  std::array<int, 4> deaths{};
};
inline int manhattan(Cell a, Cell b) { return std::abs(a.first-b.first)+std::abs(a.second-b.second); }
inline int ray(Cell a, Cell b) {
  const int x=b.first-a.first, y=b.second-a.second;
  if (x==0 && y>=-2 && y<=-1) return 0;
  if (y==0 && x>=1 && x<=2) return 1;
  if (x==0 && y>=1 && y<=2) return 2;
  if (y==0 && x>=-2 && x<=-1) return 3;
  return -1;
}
inline Action decode_action(const nlohmann::json &value) {
  if (value.is_null() || !value.is_object()) return {};
  const auto type=value.value("action",std::string());
  if (type=="defend") return {ActionKind::defend,-1};
  if (type!="move" && type!="attack") return {};
  const auto direction=value.value("direction",std::string());
  for (int i=0;i<4;++i) if (direction==directions[i])
    return {type=="move" ? ActionKind::move : ActionKind::attack,i};
  return {};
}
inline nlohmann::json encode_action(Action action) {
  if (action.kind==ActionKind::none) return nullptr;
  if (action.kind==ActionKind::defend) return {{"action","defend"}};
  return {{"action",action.kind==ActionKind::move ? "move" : "attack"},{"direction",directions.at(action.direction)}};
}
inline CombatResult combat(const std::array<Fighter,4> &fighters,
                           const std::array<Action,4> &actions, double reflection) {
  CombatResult result{fighters,{},{}};
  std::array<std::array<double,4>,4> ordinary{}, reflected{}; // [受害者][贡献者]的名义伤害。
  for (int attacker=0;attacker<4;++attacker) {
    const auto &self=fighters[attacker];
    if (!self.alive || self.protected_spawn || actions[attacker].kind!=ActionKind::attack) continue;
    std::vector<int> targets;
    if (self.area_attack) {
      for (int target=0;target<4;++target)
        if (target!=attacker && fighters[target].alive &&
            std::abs(fighters[target].position.first-self.position.first)<=2 &&
            std::abs(fighters[target].position.second-self.position.second)<=2) targets.push_back(target);
    } else {
      int nearest=-1;
      for (int target=0;target<4;++target)
        if (target!=attacker && fighters[target].alive && ray(self.position,fighters[target].position)==actions[attacker].direction &&
            (nearest<0 || manhattan(self.position,fighters[target].position)<manhattan(self.position,fighters[nearest].position))) nearest=target;
      if (nearest>=0) targets.push_back(nearest); // 最近保护车仍挡住后车；墙与本函数无关。
    }
    for (int target:targets) {
      const auto &victim=fighters[target];
      if (victim.protected_spawn) continue;
      const bool defending=actions[target].kind==ActionKind::defend; // 只认本Tick动作，绝不沿用上帧defending。
      if (reflection>0 && defending) reflected[attacker][target]+=reflection;
      else ordinary[target][attacker]+=(defending ? self.defended : self.attack)*victim.vulnerability;
    }
  }
  for (int target=0;target<4;++target) {
    if (!fighters[target].alive) continue;
    double total=0;
    std::set<int> contributors;
    for (int source=0;source<4;++source) {
      const double nominal=ordinary[target][source]+reflected[target][source];
      total+=nominal;
      if (nominal>0) contributors.insert(source);
    }
    if (total<=0) continue;
    const double actual=std::min(fighters[target].hp,total); // 过量伤害按比例分摊，不多计D。
    for (int source=0;source<4;++source) result.earned[source]+=actual*ordinary[target][source]/total;
    auto &victim=result.fighters[target];
    victim.hp=std::max(0.0,victim.hp-total);
    if (victim.hp<=0) {
      victim.alive=false; result.deaths[target]=1;
      for (int source:contributors) result.earned[source]+=kill_reward/contributors.size();
    }
  }
  return result;
}
inline double adjusted_damage(double previous,double earned,int deaths) {
  return std::max(0.0,previous+earned-death_loss*deaths); // D不在500截断；互杀也是先入账再扣除。
}
inline std::array<bool,4> move(std::array<Fighter,4> &fighters,const std::array<Action,4> &actions,
                             int width,int height,const std::set<Cell> &walls) {
  const auto before=fighters; // 所有移动按战斗后、移动前占位判断，不能边改边走。
  std::array<Cell,4> wanted{};
  std::array<bool,4> eligible{},moved{};
  for (int i=0;i<4;++i) {
    if (!before[i].alive || actions[i].kind!=ActionKind::move || actions[i].direction<0 || actions[i].direction>3) continue;
    const auto delta=steps[actions[i].direction];
    const Cell next{before[i].position.first+delta.first,before[i].position.second+delta.second};
    if (next.first<0 || next.second<0 || next.first>=width || next.second>=height || walls.count(next)) continue;
    bool occupied=false;
    for (int other=0;other<4;++other) occupied=occupied || (before[other].alive && before[other].position==next);
    if (!occupied) { eligible[i]=true; wanted[i]=next; }
  }
  for (int i=0;i<4;++i) if (eligible[i]) {
    int claims=0;
    for (int j=0;j<4;++j) claims+=eligible[j] && wanted[j]==wanted[i];
    if (claims==1) { fighters[i].position=wanted[i]; moved[i]=true; }
  }
  return moved;
}
inline Cell respawn(Cell origin,int width,int height,const std::set<Cell> &blocked) {
  // 墙只排除落点，不阻断“曼哈顿层、上右下左”的选点搜索。
  std::queue<Cell> pending; std::set<Cell> visited;
  pending.push(origin); visited.insert(origin);
  while (!pending.empty()) {
    const Cell cell=pending.front();pending.pop();
    if (!blocked.count(cell)) return cell;
    for (Cell step:steps) {
      const Cell next{cell.first+step.first,cell.second+step.second};
      if (next.first>=0 && next.second>=0 && next.first<width && next.second<height && visited.insert(next).second) pending.push(next);
    }
  }
  return {-1,-1}; // 无合法格则继续等待，不能把坐标伪造成出生点。
}
} // namespace rmtc::rules
