#pragma once
#include <array>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "rmtc_bot/events.hpp"
#include "rmtc_bot/rules.hpp"

namespace rmtc::arena { // 仅离线测试端；参赛源码不包含这个文件。
using Json=nlohmann::json;
struct Announcement {
  Event event; // 测试裁判掌握的结构化安排；解释交付前不出现在选手输入。
  int notice; // 裁判何时公告；未公告的安排不会传给任何选手。
};
struct Scenario {
  std::string name, map_style; // 场次标识、地图风格，均只用于离线组织。
  unsigned seed; // 可复现生成种子。
  int stage, width, height, ticks, radius, interpretation_delay; // 阶段、边界、动作数、视野、公告解释延迟。
  bool simulated_ai_success; // 仅计分实验假设，不冒充真实模型或比赛资格。
  std::set<Cell> walls; // 基础墙，临时分裂墙另算。
  std::map<Cell,int> resources; // 初态资源坐标及价值，生产策略看不到此全图。
  std::array<Cell,4> spawns; // A至D初始出生点。
  std::vector<Announcement> announcements; // 测试裁判未来安排，严格隔离于选手。
  Json missions; // 本场公开任务。
};
struct Car {
  rules::Fighter fighter; // 当前真实位置、HP和战斗效果。
  double resource=0, damage=0; // 官方口径R/D，D不在500截断。
  int normal=0, high=0, deaths=0, blocked=0, invalid=0, respawn_in=0, protected_left=0; // 拾取次数、死亡、受阻、非法动作、复活/保护剩余窗口。
  int attacks=0, defends=0, moves=0; // 攻击/防御提交次数与实际成功移动次数。
  Tick heal_until=-1; // 已领回血效果截止，不等于治疗点截止。
  double heal_amount=0; // 每Tick回血量。
  std::string position_reason="initial"; // 当前感知是否初始或刚复活。
  Json last_action=nullptr; // 上Tick实际结算回执。
  std::set<std::string> completed, reversed, vulnerable, empowered; // 完成任务ID，以及曾触发幻术/易伤/无双的事件key。
  std::map<std::string,int> capture; // 尚未达标的连续驻留计数。
};
class Game {
public:
  explicit Game(Scenario scenario);
  Json observe(int seat) const; // 只输出本队公开局部感知，不把全图传给策略。
  std::vector<Event> interpreted_events() const; // 明确的“公告后固定延迟oracle”测试夹具，非自然语言解析器。
  void step(const std::array<rules::Action,4> &actions);
  Json result() const;
  double score(int seat) const;
  std::set<Cell> current_walls() const;
  const Scenario &scenario() const { return scenario_; }
  const std::array<Car,4> &cars() const { return cars_; }
  int tick() const { return tick_; }
  // 测试可以在初态写入精确边界，生产策略永远没有这个接口。
  std::array<Car,4> &fixture_cars() { return cars_; }
private:
  void refresh_effects(); // 按当前Tick重建仍有效加成，保留跨死亡授予记录。
  bool resurrect(int seat); // 按A至D调用，使用实际空位选落点。
  Scenario scenario_; // 本地裁判只读配置。
  std::array<Car,4> cars_; // 当前四车真值。
  std::map<Cell,int> resources_; // 尚未领取的资源。
  int tick_=0; // 下一个待结算Tick。
};
Scenario make_scenario(const std::string &style,unsigned seed,int stage,int interpretation_delay,bool ai_success);
Json relative_entity(const std::string &type,Cell offset);

// 陪练只能用observe结果与已经延迟交付的公告解释，不能直接访问Game/Scenario真值。
class Opponent {
public:
  Opponent(std::string style,unsigned seed,int stage);
  rules::Action act(const Json &perception,const std::vector<Event> &known);
private:
  struct KnownTile { bool wall=false; int value=0; unsigned visits=0; }; // 已见墙、资源与真实访问次数。
  std::string style_; // 固定手写风格，不在赛中训练。
  int stage_; // 与选手一样由公开赛前配置指定，不从session_id猜阶段。
  std::mt19937 random_; // 游走风格的固定种子随机流。
  std::map<Cell,KnownTile> memory_; // 仅由该陪练自己的公开观测更新。
  std::map<Tick,Cell> positions_; // 仅记录自己实际坐标，用于幻术激活位置。
};
} // namespace rmtc::arena
