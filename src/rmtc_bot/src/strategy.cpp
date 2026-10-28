#include "rmtc_bot/strategy.hpp"
#include "rmtc_bot/scoring.hpp"
#include "rmtc_bot/match_config.hpp"
#include "rmtc_bot/tactics.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>

namespace { // 匿名命名空间：下面的解析/几何小工具只供本文件使用。
using Json = nlohmann::json; // JSON对象/数组/值。
using Cell = rmtc::Cell;     // 格子坐标。
using Tick = rmtc::Tick;     // 逻辑回合。
using Kind = rmtc::EventKind; // 八种事件枚举。
namespace cfg = rmtc::match_config; // cfg::前缀表示读取赛前固定配置。
constexpr std::array<Cell, 4> steps = {{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}}; // 上右下左的位移。
constexpr std::array<const char *, 4> directions = {{"up", "right", "down", "left"}}; // 同顺序的协议方向名。
// j必须是能装进int的整数，避免小数坐标或超大无符号数被截断。
int integer(const Json &j) {
  if (!j.is_number_integer() || (j.is_number_unsigned() &&
      j.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())))
    throw std::runtime_error("invalid integer field");
  const auto value = j.get<std::int64_t>(); // 先用64位暂存，再检查int范围。
  if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
    throw std::runtime_error("integer out of range");
  return static_cast<int>(value);
}
Cell absolute_position(const Json &j) { // 解析本车/任务的[x,y]数组。
  if (!j.is_array() || j.size() != 2) throw std::runtime_error("invalid absolute position");
  return {integer(j[0]), integer(j[1])};
}
// 每个实体各自一张9×9图：中心1是自己，2是该实体；没有2表示与自己重叠。
// 图中的0只是占位字符，不能据此把整张9×9都当成可走地面。
std::optional<Cell> relative_position(const Json &position) {
  if (!position.is_string()) return std::nullopt;
  const auto &grid = position.get_ref<const std::string &>(); // 引用原字符串，避免复制9×9图。
  if (grid.size() != 89 || grid[44] != '1') return std::nullopt;
  Cell offset{0, 0}; // 实体相对本车的dx/dy；没有2时保持[0,0]。
  bool found = false; // 保证同一实体图最多只有一个2。
  for (int y = 0; y < 9; ++y) {
    if (y < 8 && grid[y * 10 + 9] != '\n') return std::nullopt;
    for (int x = 0; x < 9; ++x) {
      const char ch = grid[y * 10 + x]; // 每行9个字符加换行，所以步长是10。
      if (x == 4 && y == 4) continue;
      if (ch == '2' && !found) { offset = {x - 4, y - 4}; found = true; }
      else if (ch != '0') return std::nullopt;
    }
  }
  return offset;
}
int manhattan(Cell a, Cell b) { // 四向走格子的距离：|dx|+|dy|。
  return std::abs(a.first - b.first) + std::abs(a.second - b.second);
}
int ray(Cell from, Cell target) {
  // 普通攻击只看同一直线1–2格。最近目标筛选在后面做，墙不挡攻击。
  const int dx = target.first - from.first, dy = target.second - from.second; // 目标相对攻击者的位置。
  if (dx == 0 && dy >= -2 && dy <= -1) return 0;
  if (dy == 0 && dx >= 1 && dx <= 2) return 1;
  if (dx == 0 && dy >= 1 && dy <= 2) return 2;
  if (dy == 0 && dx >= -2 && dx <= -1) return 3;
  return -1;
}
struct Enemy {
  Cell position; double hp; bool defending, protected_spawn; // 当前真实位置、血量和上帧状态。
  std::string identity; // 公开席位/车辆ID；空串表示无法可靠关联历史。
  std::optional<Cell> forecast; // 两次连续相同位移后的一步猜测，随时可能落空。
};
struct Entity { std::string type; Cell position; double value; bool high; }; // 解析后的实体类型、坐标和资源信息。
}  // namespace

Strategy::Strategy(int stage) : Strategy(stage, rmtc::StrategyParameters::competition()) {}
Strategy::Strategy(int stage, rmtc::StrategyParameters parameters) : stage_(stage), parameters_(parameters) {
  if (stage != 1 && stage != 2) throw std::runtime_error("stage must be 1 or 2");
  parameters_.validate();
}
void Strategy::reset() {
  tiles_.clear(); probe_attempts_.clear(); history_.clear(); events_.clear(); capture_progress_.clear(); spawn_.reset();
  opponents_.clear(); // 不把上一场或旧席位的轨迹带入新场。
  planned_resources_.clear();move_attempt_.reset();failed_destination_.reset();failed_moves_=0;
  goal_.reset(); // 新场没有上一目标，也没有必须继续执行的旧路线。
  model_reply_seen_ = resource_counts_known_ = false;
  observed_normal_ = observed_high_ = 0; previous_score_tick_ = -1; previous_resource_total_ = 0;
  width_ = height_ = 0; reversed_ = reverse_unknown_ = empowered_ = false;
  vulnerability_ = 1; attack_damage_ = 20; defended_damage_ = 10; healing_until_ = -1;
  healing_amount_ = 0; reason_ = "new_session";
}
std::optional<Strategy::Cell> Strategy::predict_opponent(const std::string &identity, Cell position, Tick tick) {
  if (identity.empty()) return std::nullopt;
  const auto found = opponents_.find(identity); // 查找同一辆公开可见敌车的上次观测。
  if (found == opponents_.end()) {
    opponents_.emplace(identity, OpponentTrack{position, {0, 0}, tick, 0});
    return std::nullopt; // 一帧位置不足以推断它的运动习惯。
  }
  auto &track = found->second; // 该敌车的最新轨迹；只在新的连续Tick推进。
  if (tick <= track.tick) return std::nullopt;
  const Cell delta{position.first - track.position.first, position.second - track.position.second}; // 世界坐标位移。
  const bool consecutive = tick == track.tick + 1 && manhattan(position, track.position) <= 1; // 断帧/复活跳格不连续。
  track.consistent = !consecutive ? 0 : delta == track.delta ? std::min(2U, track.consistent + 1) : 1;
  track.position = position; track.delta = delta; track.tick = tick;
  if (track.consistent < 2) return std::nullopt;
  return Cell{position.first + delta.first, position.second + delta.second}; // 最多猜下一步，不外推长路线。
}
void Strategy::merge_events(const std::vector<rmtc::Event> &events) {
  if (stage_ == 1) return;
  // 同一事件的新解释覆盖旧解释，不重复叠加；不同区域的事件有不同key。
  for (const auto &e : events) events_[e.key] = e;
}
Json Strategy::known_events() const {
  auto result = Json::array(); // 返回给LLM的已知事件摘要列表。
  for (const auto &[key, event] : events_) { (void)key; result.push_back(rmtc::event_summary(event)); }
  return result;
}
bool Strategy::split_wall(Cell cell, Tick tick) const {
  // 规则排除初始出生点。只豁免自己确实知道的出生点，不猜地图对称性。
  if (spawn_ && cell == *spawn_) return false;
  for (const auto &[key, e] : events_) {
    (void)key;
    if (e.kind == Kind::split && e.active(tick) && e.contains(cell)) return true;
  }
  return false;
}
bool Strategy::dangerous(Cell cell, Tick tick) const {
  if (split_wall(cell, tick)) return true;
  for (const auto &[key, e] : events_) {
    (void)key;
    if (e.kind == Kind::explosion && e.start == tick && e.contains(cell) && e.amount > 0) return true;
  }
  return false;
}
std::array<bool, 5> Strategy::safe_first_steps(Cell start, Tick tick, const std::set<Cell> &occupied) const {
  constexpr int horizon = 8; // 向前检查的Tick数，也是可检查格子的半径。
  const std::array<bool, 5> unrestricted = {true, true, true, true, true}; // 未能筛选时交回原策略。
  const bool relevant = std::any_of(events_.begin(), events_.end(), [&](const auto &entry) {
    const auto &e = entry.second; // entry是key/事件对；e是这个事件。
    return (e.kind == Kind::split && e.end > tick && e.start <= tick + horizon) ||
           (e.kind == Kind::explosion && e.start > tick && e.start <= tick + horizon);
  });
  if (!relevant) return unrestricted;

  // 只查8格内最多145个已知格。这里只筛第一步，不缓存一串未来动作。
  std::vector<Cell> cells; // 本次时空检查涉及的格子。
  std::map<Cell, std::size_t> index; // 坐标→cells下标，方便查邻格。
  for (int dy = -horizon; dy <= horizon; ++dy) {
    for (int dx = -horizon; dx <= horizon; ++dx) {
      if (std::abs(dx) + std::abs(dy) > horizon) continue;
      const Cell cell{start.first + dx, start.second + dy}; // dx/dy是对当前格的扫描偏移。
      const auto tile = tiles_.find(cell); // 格子的历史观测；找不到就属于未知。
      if (tile == tiles_.end() || !tile->second.seen || tile->second.wall || occupied.count(cell)) continue;
      index[cell] = cells.size(); cells.push_back(cell);
    }
  }
  std::array<std::vector<bool>, horizon + 1> good; // k是未来回合偏移，i是cells中的格子下标。
  // good[k][i]：到第tick+k回合开始时站在格i，之后还有一条能避开已知危险的路。
  // 从第8回合倒着算；第五种转移是留在原地，实际可以提交DEFEND。
  for (auto &layer : good) layer.resize(cells.size(), false);
  for (std::size_t i = 0; i < cells.size(); ++i) good[horizon][i] = !dangerous(cells[i], tick + horizon);
  for (int k = horizon - 1; k >= 1; --k) {
    for (std::size_t i = 0; i < cells.size(); ++i) {
      const auto cell = cells[i]; // 倒推当前检查的格子。
      if (dangerous(cell, tick + k)) continue;
      for (int action = 0; action < 5; ++action) {
        const Cell next = action == 4 ? cell : Cell{cell.first + steps[action].first, cell.second + steps[action].second}; // 后一回合的候选落点。
        const auto found = index.find(next); // action=4留在原格，否则按steps移动。
        // 爆炸先于移动：可在爆炸已结算后进入该格，但下一Tick开始时必须安全。
        if (found != index.end() && !split_wall(next, tick + k) && good[k + 1][found->second]) {
          good[k][i] = true; break;
        }
      }
    }
  }
  std::array<bool, 5> safe = {}; // 回到当前时刻，只提取能接上good[1]的第一步。
  for (int action = 0; action < 5; ++action) {
    const Cell next = action == 4 ? start : Cell{start.first + steps[action].first, start.second + steps[action].second}; // 本Tick第一步落点。
    const auto found = index.find(next); // 查落点是否在已知格列表中。
    safe[action] = found != index.end() && !split_wall(next, tick) && good[1][found->second];
  }
  // 找不到可认证路线时继续原策略；未知地图或远期危险不能据此判成“必死”。
  return std::any_of(safe.begin(), safe.end(), [](bool value) { return value; }) ? safe : unrestricted;
}
std::vector<Strategy::ResourcePlan> Strategy::resource_plans(Cell start, Tick tick, Tick remaining_ticks,
    const std::vector<Cell> &targets, const std::set<Cell> &occupied,
    const std::array<bool, 4> &first_allowed, double current_r,
    bool need_normal, bool need_high, double value_target, const std::vector<Cell> &previous_plan) const {
  using Clock = std::chrono::steady_clock; // 墙钟只管计算预算，事件仍用逻辑Tick。
  const auto deadline = Clock::now() + std::chrono::milliseconds(parameters_.resource_plan_budget_ms);
  const auto time_up = [&] { return Clock::now() >= deadline; };
  struct State {
    Cell position{}, first_resource{}; // 计划末位置、路上首次拾取格；不是本车定位。
    int first = -1, elapsed = 0; // 首方向、总MOVE数。
    double virtual_r = 0, benefit = 0; // 计划R和收益，绝不回写官方成绩。
    unsigned mission_bonus = 0; // 普通/高价值任务机会各估一次。
    std::set<Cell> collected; // 包括顺路拾取，防止同一资源计两遍。
    std::vector<Cell> stops; // 显式目标顺序，不包含已执行动作。
    double utility() const { return benefit / (elapsed + 1); }
  };
  // 一帧内地图、占位和事件固定；同起点/终点/到达时刻的BFS可复用。下一帧全部丢弃。
  std::map<std::tuple<Cell,Cell,int>,std::vector<Cell>> route_cache;
  const auto route = [&](const State &state, Cell target) {
    const auto key=std::make_tuple(state.position,target,state.elapsed);
    const auto cached=route_cache.find(key);
    if (cached!=route_cache.end()) return cached->second;
    std::map<Cell,std::pair<Cell,int>> parent; // 前驱及本段距离，用来恢复实际经过格子。
    std::queue<Cell> pending;
    parent[state.position]={state.position,0};pending.push(state.position);
    std::vector<Cell> path;
    while (!pending.empty() && !time_up()) {
      const Cell cell=pending.front();pending.pop();
      const int distance=parent.at(cell).second;
      if (cell==target) {
        for (Cell at=target;at!=state.position;at=parent.at(at).first) path.push_back(at);
        std::reverse(path.begin(),path.end());break;
      }
      if (state.elapsed+distance>=remaining_ticks) continue;
      for (int direction=0;direction<4;++direction) {
        if (state.elapsed==0 && distance==0 && !first_allowed[direction]) continue;
        const Cell next{cell.first+steps[direction].first,cell.second+steps[direction].second};
        const auto tile=tiles_.find(next);
        const Tick action_tick=tick+state.elapsed+distance;
        if (tile==tiles_.end() || !tile->second.seen || tile->second.wall || occupied.count(next) ||
            parent.count(next) || split_wall(next,action_tick) || dangerous(next,action_tick+1)) continue;
        parent[next]={cell,distance+1};pending.push(next);
      }
    }
    if (!time_up()) route_cache.emplace(key,path); // 不把预算中断误记成不可达。
    return path;
  };
  // 每条修改后的串都从真实位置完整重算路径、沿途拾取、期限和收益，不只交换一个距离数字。
  const auto extend = [&](const State &state,Cell target) -> std::optional<State> {
    const auto tile=tiles_.find(target);
    if (target==state.position || state.collected.count(target) || tile==tiles_.end() || tile->second.value<=0) return std::nullopt;
    const auto path=route(state,target);
    if (path.empty()) return std::nullopt;
    State child=state;child.stops.push_back(target);
    if (child.first<0) child.first=ray(start,path.front());
    for (Cell cell:path) {
      ++child.elapsed;child.position=cell;
      const auto &item=tiles_.at(cell);
      if (item.value<=0 || !child.collected.insert(cell).second) continue;
      if (child.collected.size()==1) child.first_resource=cell;
      const double freshness=1.0/(1.0+std::max<Tick>(0,tick-item.last_seen)*parameters_.stale_resource_decay);
      double gain=item.value;
      if (stage_==2) {
        gain=rmtc::score_delta(child.virtual_r,item.value,120,30)+std::min(item.value,20.0)*0.001;
        const unsigned bit=item.high ? 2U : 1U;
        if ((item.high ? need_high : need_normal) && !(child.mission_bonus&bit)) {
          gain+=parameters_.resource_mission_bonus;child.mission_bonus|=bit;
        }
        gain+=0.1*std::min(item.value,std::max(0.0,value_target-child.virtual_r));
      }
      child.virtual_r+=item.value;child.benefit+=gain*freshness;
    }
    if (child.first<0 || !std::isfinite(child.benefit) || !std::isfinite(child.virtual_r)) return std::nullopt;
    return child;
  };
  const auto ranked_before=[](const State &a,const State &b) {
    return std::make_tuple(-a.utility(),-a.benefit,a.elapsed,a.first,a.position)<
           std::make_tuple(-b.utility(),-b.benefit,b.elapsed,b.first,b.position);
  };
  std::array<std::optional<State>,4> best; // 各首方向各留最好串，供争格或战术改选。
  const auto record=[&](const State &state) {
    if (state.first<0 || state.collected.size()<2) return;
    auto &old=best[state.first];
    if (!old || state.utility()>old->utility()+1e-9 ||
        (std::abs(state.utility()-old->utility())<=1e-9 && state.benefit>old->benefit)) old=state;
  };
  State initial;initial.position=start;initial.virtual_r=current_r;
  const auto evaluate_stops=[&](const std::vector<Cell> &stops) {
    State state=initial;
    for (Cell stop:stops) {
      if (time_up()) break;
      if (state.collected.count(stop) || stop==state.position) continue;
      const auto child=extend(state,stop);
      if (!child) break; // 留下仍可用前缀，绝不穿过已失效的一段。
      state=*child;record(state);
    }
    return state;
  };
  if (parameters_.resource_refinement) evaluate_stops(previous_plan);
  std::vector<State> beam{initial};
  for (int depth=0;depth<parameters_.resource_plan_depth && !time_up();++depth) {
    std::vector<State> expanded;
    for (const auto &state:beam) for (Cell target:targets) {
      if (time_up()) break;
      const auto child=extend(state,target);
      if (child) {record(*child);expanded.push_back(*child);}
    }
    if (expanded.empty()) break;
    std::stable_sort(expanded.begin(),expanded.end(),ranked_before);
    std::vector<State> selected;
    std::vector<bool> used(expanded.size(),false);std::array<bool,4> used_direction{};
    std::set<std::tuple<int,Cell,std::set<Cell>,int>> unique;
    for (int pass=0;pass<2;++pass) for (std::size_t i=0;i<expanded.size() && selected.size()<static_cast<std::size_t>(parameters_.resource_beam_width);++i) {
      const auto &candidate=expanded[i];
      if (used[i] || (pass==0 && used_direction[candidate.first]) ||
          !unique.emplace(candidate.first,candidate.position,candidate.collected,candidate.elapsed).second) continue;
      used[i]=true;used_direction[candidate.first]=true;selected.push_back(candidate);
    }
    beam=std::move(selected);
  }
  // 小范围局部改进：从当前最好串出发，轮流反转/插入/删除；每次都按相同规则重评。
  for (int trial=0;parameters_.resource_refinement && trial<parameters_.resource_refinement_trials && !time_up();++trial) {
    const State *seed=nullptr;
    for (const auto &candidate:best) if (candidate && (!seed || ranked_before(*candidate,*seed))) seed=&*candidate;
    if (!seed) break;
    auto stops=seed->stops;
    const std::size_t n=stops.size(),position=(trial/3)%n;
    if (trial%3==0 && n>1) {
      const std::size_t other=(position+1+(trial/9))%n;
      std::reverse(stops.begin()+std::min(position,other),stops.begin()+std::max(position,other)+1);
    } else if (trial%3==1 && !targets.empty()) {
      const Cell target=targets[(trial/3)%targets.size()];
      if (std::find(stops.begin(),stops.end(),target)!=stops.end()) continue;
      stops.insert(stops.begin()+position,target);
      if (stops.size()>static_cast<std::size_t>(parameters_.resource_plan_depth)) stops.pop_back();
    } else if (n>1) stops.erase(stops.begin()+position);
    evaluate_stops(stops);
  }
  std::vector<ResourcePlan> result;
  for (const auto &state:best) if (state)
    result.push_back({state->first,state->first_resource,state->utility(),state->benefit,state->stops});
  return result;
}

void Strategy::update_effects(Tick tick) {
  // 每次从历史重建，避免重复消息把效果累计两次。有效期一律[start,end)。
  reversed_ = reverse_unknown_ = empowered_ = false;
  vulnerability_ = 1; attack_damage_ = 20; defended_damage_ = 10; healing_until_ = -1; healing_amount_ = 0;
  capture_progress_.clear();
  for (const auto &[key, e] : events_) {
    if (e.kind == Kind::illusion && e.active(tick)) {
      // 幻术在激活时判定谁中招；离开区域、死亡都不能提前解除。
      const auto initial = history_.find(e.start); // 幻术激活时本车的历史状态。
      if (initial == history_.end()) reverse_unknown_ = true;
      else if (initial->second.alive && e.contains(initial->second.position)) reversed_ = true;
    }
    int consecutive = 0; // 当前重建到的连续堡垒驻留次数。
    Tick previous = -2;  // 上一条有效回执的执行Tick；-2用于让首条触发连续性检查。
    bool awarded = false; // 本事件内是否曾经达标；达标后死亡不会撤销。
    for (const auto &[state_tick, state] : history_) {
      (void)state_tick;
      const auto executed = state.receipt_tick; // state是历史快照，executed是其回执对应的执行Tick。
      // 当前感知N的回执解释N-1；用执行Tick判断触发窗口，不能用收到消息的时刻。
      if (executed < e.start || executed >= e.end) continue;
      if (e.kind == Kind::vulnerability && e.active(tick) && state.moved && executed < e.trigger_end)
        vulnerability_ = std::max(vulnerability_, e.multiplier);
      if (e.kind == Kind::heal && state.moved && !state.respawn && e.contains(state.position) && executed + e.effect_ticks > healing_until_) {
        healing_until_ = executed + e.effect_ticks; healing_amount_ = e.amount;
      }
      if (e.kind == Kind::fortress && e.active(tick)) {
        // 中间漏了回执，就不猜连续驻留。死亡、复活或离开会打断尚未完成的进度。
        if (executed != previous + 1) consecutive = 0;
        consecutive = state.alive && !state.respawn && e.contains(state.position) ? consecutive + 1 : 0;
        previous = executed;
        if (consecutive >= e.capture_ticks) awarded = true;
      }
    }
    if (e.kind == Kind::fortress && e.active(tick)) {
      capture_progress_[key] = consecutive;
      if (awarded) {
        // 已拿到的无双跨死亡保留，但堡垒结束就失效，不是整场永久。
        empowered_ = true;
        attack_damage_ = std::max(attack_damage_, e.amount);
        defended_damage_ = e.defended_damage;
      }
    }
  }
}

Json Strategy::decide(const Json &p) {
  const auto previous_attempt=std::exchange(move_attempt_,std::nullopt); // 只验证意图是否被真实回执执行。
  const auto previous_plan=std::exchange(planned_resources_,std::vector<Cell>()); // 旧串仅是待重评候选，提前返回即失效。
  const auto previous_goal = std::exchange(goal_, std::nullopt); // 先取出旧目标；异常、死亡和提前避险都不会续签它。
  const auto fallback = [&](const char *reason) {
    // reason是本次兜底原因；只在已知存活的第二阶段返回合法DEFEND。
    reason_ = reason;
    return stage_ == 2 && p.at("self").at("alive") == true ? Json{{"action", "defend"}} : Json();
  };
  const auto skip = [&](const char *reason) { reason_ = reason; return Json(); }; // 明确不发布，不产生默认WAIT。
  // 1. 读本车真实状态，记上一Tick实际发生了什么。
  const auto &self = p.at("self"); // p是本队感知，self是其中的本车状态。
  const Tick tick = p.at("tick").get<Tick>(); // 当前动作窗口的逻辑回合。
  const Tick tick_limit = stage_ == 1 ? cfg::stage1_ticks : cfg::stage2_ticks; // 赛前明确的本阶段总动作数。
  if (tick >= tick_limit) return skip("finished"); // 最终状态不是新动作窗口。
  const Tick remaining_ticks = tick_limit - tick; // 包含本Tick；Tick499还能执行一步。
  const bool alive = self.at("alive").get<bool>(); // 阵亡时不得提交防御/攻击/移动。
  const Cell start = absolute_position(self.at("position")); // 本帧确认位置，也是所有路径的起点。
  const int width = integer(p.at("map").at("width")), height = integer(p.at("map").at("height")); // 当前地图宽、高。
  const int radius = integer(p.at("vision_radius")); // 实际视野半径；width/height是本场地图边界。
  if (width <= 0 || height <= 0 || width > 1000000 || height > 1000000 ||
      start.first < 0 || start.first >= width || start.second < 0 || start.second >= height ||
      radius < 0 || radius > 4 || !p.at("visible_entities").is_array()) return fallback("invalid_frame");
  if (width_ != 0 && (width != width_ || height != height_)) return fallback("map_changed_without_session");
  const bool protected_spawn = self.value("protected", false); // 本车是否有出生保护。
  const double hp = self.value("hp", 100.0); // 当前生命值；兼容旧感知缺字段时的100HP。
  if (!std::isfinite(hp) || hp < 0) return fallback("invalid_hp");
  const bool respawn = self.value("position_reason", std::string()) == "respawn"; // 此帧是否刚复活。
  const auto receipt = self.value("last_action", Json()); // 上Tick动作回执，不能当成当前坐标。
  bool moved = false; // 是否有可信的“上一Tick成功MOVE”证据。
  Tick receipt_tick = -1; // -1表示没有当前可采纳的回执。
  if (receipt.is_object() && receipt.contains("tick") && receipt.at("tick").is_number_integer()) {
    const auto reported = receipt.at("tick").get<Tick>(); // 回执自带的执行Tick。
    if (reported == tick - 1) {
      receipt_tick = reported;
      moved = receipt.value("moved", false) && receipt.value("action", std::string()) == "move";
    }
  }
  if (previous_attempt && previous_attempt->tick==tick-1 && previous_attempt->from==start &&
      receipt_tick==tick-1 && receipt.value("action",std::string())=="move" && !moved && !respawn) {
    if (failed_destination_==previous_attempt->destination) ++failed_moves_;
    else {failed_destination_=previous_attempt->destination;failed_moves_=1;}
  } else {failed_destination_.reset();failed_moves_=0;}
  const bool keep_goal = previous_goal && previous_goal->tick == tick - 1 && !respawn &&
      !(receipt_tick >= 0 && receipt.value("action", std::string()) == "move" && !moved);
  // 上次MOVE失败时不发迟滞奖励，让重算路线及时换路；目标是否还存在仍由本帧候选决定。
  // 坐标永远取感知；回执只确认成功移动和已结算的效果，不用于累加位置。
  history_[tick] = Snapshot{start, alive, protected_spawn, respawn, moved, receipt_tick};
  if (alive && (tick == 0 || self.value("position_reason", std::string()) == "initial")) spawn_ = start;
  update_effects(tick);
  if (!alive) return skip("dead");

  // 2. 把当前可见实体换成绝对坐标，覆盖当前视野；视野外保留旧记忆。
  std::vector<Entity> entities; // 当前帧的实体，先校验完再更新地图。
  std::vector<Enemy> enemies; // 当前可见敌车，只在本帧估计攻防。
  std::set<Cell> occupied; // 当前敌车占格，MOVE不能交换位置或跟进离开的敌车。
  for (const auto &j : p.at("visible_entities")) {
    const auto type = j.at("type").get<std::string>(); // j是一个原始JSON实体条目。
    if (type != "obstacle" && type != "robot" && type != "resource_normal" && type != "resource_high") continue;
    const auto offset = relative_position(j.at("position")); // 可选相对坐标；解析失败为空。
    if (!offset) return fallback("invalid_entity_grid");
    const auto [dx, dy] = *offset;
    if (std::abs(dx) > radius || std::abs(dy) > radius || dx < -start.first ||
        dx >= width - start.first || dy < -start.second || dy >= height - start.second)
      return fallback("invalid_entity_coordinate");
    const Cell cell{start.first + dx, start.second + dy}; // 用本车绝对坐标加相对dx/dy。
    const double value = j.value("value", type == "resource_high" ? 20.0 : 10.0); // 资源价值；旧格式缺字段按资源类型处理。
    if (!std::isfinite(value) || value < 0) return fallback("invalid_resource_value");
    entities.push_back({type, cell, value, value >= 20});
    if (type == "robot") {
      const double enemy_hp = j.value("hp", 100.0); // 当前敌车生命；不跨帧累加猜测伤害。
      if (!std::isfinite(enemy_hp) || enemy_hp < 0) return fallback("invalid_enemy_hp");
      occupied.insert(cell);
      std::string identity; // 优先公开席位，缺失时才用公开车辆ID；不按位置冒认同一辆车。
      for (const char *field : {"team", "robot_id", "id"}) {
        const auto value = j.find(field); // 只取公开且确为字符串的身份字段。
        if (value != j.end() && value->is_string() && !value->get_ref<const std::string &>().empty()) {
          identity = value->get<std::string>(); break;
        }
      }
      enemies.push_back({cell, enemy_hp, j.value("defending", false), j.value("protected", false), identity, std::nullopt});
    }
  }
  width_ = width; height_ = height;
  for (int y = std::max(0, start.second - radius); y <= std::min(height - 1, start.second + radius); ++y) {
    for (int x = std::max(0, start.first - radius); x <= std::min(width - 1, start.first + radius); ++x) {
      auto &tile = tiles_[{x, y}]; // x/y扫描本帧有效视野，tile是该格记忆的可写引用。
      tile.seen = true; tile.wall = false; tile.value = 0; tile.last_seen = tick;
    }
  }
  for (const auto &entity : entities) {
    auto &tile = tiles_.at(entity.position); // entity是已校验实体，tile是它所在格的地图记录。
    if (entity.type == "obstacle") {
      tile.wall = true;
      if (tile.wall_since < 0) tile.wall_since = tick;
    } else if (entity.type == "resource_normal" || entity.type == "resource_high") {
      tile.value = entity.value; tile.high = entity.high;
    }
  }
  for (auto &[cell, tile] : tiles_) {
    // cell是记忆坐标，tile是其观测；key/e分别是事件去重键和事件内容。
    if (!tile.wall) tile.wall_since = -1;
    // 没看到墙消失时只退回未知，不能把临时墙所在格直接认作空地。
    for (const auto &[key, e] : events_) {
      (void)key;
      if (e.kind == Kind::split && tick >= e.end && e.contains(cell) &&
          tile.wall && tile.wall_since >= e.start && tile.last_seen < e.end) {
        tile.wall = false; tile.seen = false; tile.wall_since = -1;
      }
    }
  }
  ++tiles_.at(start).visits;
  for (auto &enemy : enemies) {
    enemy.forecast = predict_opponent(enemy.identity, enemy.position, tick);
    if (!enemy.forecast || *enemy.forecast == enemy.position) continue;
    const auto cell = *enemy.forecast; // 只检验预测的下一格；不改当前真实占位。
    const auto tile = tiles_.find(cell); // 未知地形仍未知，不补全地图。
    if (cell.first < 0 || cell.first >= width || cell.second < 0 || cell.second >= height ||
        cell == start || occupied.count(cell) || split_wall(cell, tick) ||
        (tile != tiles_.end() && tile->second.wall)) enemy.forecast.reset(); // 明知走不成就放弃这个猜测。
  }
  // 规划始终用世界方向。只有最后发MOVE时补偿幻术，ATTACK方向保持不变。
  const auto move = [&](int step, const char *reason) {
    // step是世界方向索引；reason记录为什么选它，返回前才补偿幻术。
    reason_ = reason;move_attempt_=MoveAttempt{start,{start.first+steps[step].first,start.second+steps[step].second},tick};
    return Json{{"action", "move"}, {"direction", directions[reversed_ ? (step + 2) % 4 : step]}};
  };
  // 3. 先算移动前的危险。先挨打、存活后才移动，当前MOVE救不了已命中的攻击。
  bool reflection = false, potential_area_enemy = false; // 是否有反伤事件、敌方是否可能已有无双。
  double reflection_amount = 0, pre_damage = 0, potential_area_damage = 0; // 反伤值、动作前爆炸伤害、可能的无双伤害。
  for (const auto &[key, e] : events_) {
    (void)key;
    if (e.kind == Kind::reflect && e.active(tick)) {
      reflection = true; reflection_amount = std::max(reflection_amount, e.amount);
    }
    if (e.kind == Kind::fortress && e.active(tick) && tick >= e.start + e.capture_ticks) {
      potential_area_enemy = true;
      potential_area_damage = std::max(potential_area_damage, e.amount);
    }
    if (e.kind == Kind::explosion && e.start == tick && e.contains(start) && !protected_spawn)
      pre_damage += e.amount;
    if (e.kind == Kind::split && e.start == tick && split_wall(start, tick)) return skip("split_forced_wait");
  }
  if (pre_damage >= hp) return skip("lethal_activation_before_action");
  double incoming = 0, defended_incoming = 0; // 不防御/防御时的本Tick可能受伤总量。
  // 这是可见敌车可能造成的伤害上界，不是对手已提交的动作或实测概率。
  for (const auto &enemy : enemies) {
    if (enemy.protected_spawn || protected_spawn) continue;
    bool nearest = ray(enemy.position, start) != -1;
    if (nearest) for (const auto &other : enemies)
      if (other.position != enemy.position && ray(enemy.position, other.position) == ray(enemy.position, start) &&
          manhattan(enemy.position, other.position) < manhattan(enemy.position, start)) nearest = false;
    if (nearest) {
      // 未观测确认的敌方无双只进入后面的不确定情景，不能把所有敌车一律当50伤害强制原地防御。
      incoming += 20 * vulnerability_;
      defended_incoming += reflection ? 0 : 10 * vulnerability_;
    }
  }
  // MOVE 无法躲开本 Tick 已经先结算的战斗；防御可能保命时先防御。
  if (stage_ == 2 && incoming >= hp - pre_damage && defended_incoming < hp - pre_damage) {
    reason_ = "defend_lethal_threat";
    return {{"action", "defend"}};
  }

  // 对手下一步可能踩辣椒，属于保守估计；不把其未知动作当作事实。
  const auto possible_fire = [&](Cell cell) {
    double damage = 0; // cell被可见对手下一步踩辣椒所波及的可能环境伤害。
    if (protected_spawn && cell == start) return damage;
    for (const auto &[key, e] : events_) {
      (void)key;
      if (e.kind != Kind::chili || !e.active(tick)) continue;
      for (const auto &point : e.points) {
        if (cell == point || (cell.first != point.first && cell.second != point.second)) continue;
        if (std::any_of(enemies.begin(), enemies.end(), [&](const Enemy &enemy) {
              return manhattan(enemy.position, point) == 1;
            })) damage += e.amount;
      }
    }
    return damage;
  };

  // 4. 在已知地图上BFS。每格只保存第一步和距离，每帧都会重算。
  auto safe_steps = safe_first_steps(start, tick, occupied); // 五个第一步的安全接续标志。
  if (parameters_.collision_replan && failed_moves_>=2 && failed_destination_) {
    int alternative=-1;
    for (int direction=0;direction<4;++direction) {
      const Cell next{start.first+steps[direction].first,start.second+steps[direction].second};
      const auto tile=tiles_.find(next);
      if (next!=*failed_destination_ && safe_steps[direction] && tile!=tiles_.end() && tile->second.seen &&
          !tile->second.wall && !occupied.count(next) && !split_wall(next,tick) && !dangerous(next,tick+1) &&
          possible_fire(next)<hp-pre_damage) alternative=direction;
    }
    if (alternative>=0) {
      const int repeated=ray(start,*failed_destination_);
      if (repeated>=0) safe_steps[repeated]=false; // 只避开本Tick首步；目标和后续路段仍重新BFS。
    }
  }
  struct Node { int first, distance; }; // first是起点第一步方向，-1表示起点；distance是最少步数。
  std::map<Cell, Node> paths; // 每个已到达格子的最短路径摘要。
  std::vector<Cell> order; // BFS弹出顺序，后续按这个稳定顺序评估目标。
  std::queue<Cell> queue; // 等待扩展的格子队列。
  paths[start] = {-1, 0}; queue.push(start);
  while (!queue.empty()) {
    const Cell current = queue.front(); queue.pop(); order.push_back(current); // 取出当前BFS格。
    const auto node = paths.at(current); // current为当前扩展格，node为它的最短路径摘要。
    if (node.distance >= remaining_ticks) continue; // 不规划比赛结束后才能到达的资源。
    for (int i = 0; i < 4; ++i) {
      if (node.first == -1 && !safe_steps[i]) continue;
      const Cell next{current.first + steps[i].first, current.second + steps[i].second}; // i是上右下左索引，next是邻格。
      const auto tile = tiles_.find(next); // 邻格的已知地图记录，可能不存在。
      if (tile == tiles_.end() || !tile->second.seen || tile->second.wall || occupied.count(next) ||
          paths.count(next) || split_wall(next, tick + node.distance) ||
          (node.distance == 0 && possible_fire(next) >= hp - pre_damage) ||
          dangerous(next, tick + node.distance + 1)) continue;
      paths[next] = {node.first == -1 ? i : node.first, node.distance + 1};
      queue.push(next);
    }
  }
  if (stage_ == 2 && possible_fire(start) >= hp - pre_damage && !reverse_unknown_) {
    for (const auto &cell : order) {
      const auto node = paths.at(cell); // 这个候选格的第一步和最短步数。
      if (node.distance == 1 && possible_fire(cell) < hp - pre_damage)
        return move(node.first, "avoid_possible_chili_fire");
    }
  }
  // 即将到期的危险先退出；本 Tick 才激活的爆炸不能靠移动补救。
  for (const auto &[key, e] : events_) {
    (void)key;
    if ((e.kind != Kind::explosion && e.kind != Kind::split) || e.start <= tick || !e.contains(start)) continue;
    if (e.kind == Kind::split && spawn_ && start == *spawn_) continue;
    int nearest_exit = std::numeric_limits<int>::max(), escape = -1; // 最短退出步数、对应第一步。
    for (const auto &cell : order) {
      const auto node = paths.at(cell); // 这个候选格的第一步和最短步数。
      if (!e.contains(cell) && node.distance < nearest_exit) { nearest_exit = node.distance; escape = node.first; }
    }
    if (e.start - tick <= nearest_exit + static_cast<Tick>(parameters_.hazard_lead_ticks) && escape >= 0 && !reverse_unknown_)
      return move(escape, "escape_announced_hazard");
  }

  // 5. 给采集、任务、回血、堡垒和踩辣椒机会算收益，统一比较第一步。
  std::set<std::string> completed; // 裁判已经确认完成的任务ID集合。
  const auto score = p.value("score", Json::object()); // 最新官方计分状态。
  const double resource_value = score.value("resource_value", 0.0); // 官方当前资源累计R。
  const double effective_damage = score.value("effective_damage", 0.0); // 官方当前D，可下降，也可超过500。
  if (previous_score_tick_ < 0) resource_counts_known_ = tick == 0 && resource_value == 0;
  else if (previous_score_tick_ == tick - 1 && resource_value >= previous_resource_total_) {
    const double collected = resource_value - previous_resource_total_; // 每Tick每车至多拾取当前格一份资源。
    if (collected > 0) { if (collected >= 20) ++observed_high_; else ++observed_normal_; }
  } else resource_counts_known_ = false; // 漏帧/晚加入后不把多份累计值猜成一次拾取。
  previous_score_tick_ = tick; previous_resource_total_ = resource_value;
  // 封顶后的直接分差接近0；再给很小的争夺权重，不把它算成官方额外加分。
  const auto combat_value = [&](double contribution) {
    // contribution是某候选攻击预计新增的D；返回的是决策估值，不是修改官方成绩。
    return rmtc::score_delta(effective_damage, contribution, 500, 15) +
           std::min(contribution, 60.0) * 0.001;
  };
  if (score.contains("mission_completed_ids")) {
    for (const auto &id : score.at("mission_completed_ids")) completed.insert(id.get<std::string>());
  }
  const auto missions = p.value("missions", Json::array()); // 结构化任务；与自然语言随机事件分开处理。
  bool need_normal = false, need_high = false, need_damage = false, need_value = false; // 尚未完成的四类任务需求。
  double value_target = 0; // 尚未完成的资源价值任务最高门槛；资源串只在门槛内加机会估值。
  std::vector<Cell> reach; // 尚未完成的到点任务坐标。
  for (const auto &mission : missions) {
    // mission是一个任务；先按id跳过已完成项，再按type决定需要什么收益。
    if (completed.count(mission.at("id").get<std::string>())) continue;
    const auto type = mission.at("type").get<std::string>(); // 当前任务类型。
    need_normal = need_normal || type == "normal_resources";
    need_high = need_high || type == "high_resources";
    need_damage = need_damage || (type == "effective_damage" && effective_damage < mission.value("target", 0.0));
    need_value = need_value || (type == "resource_value" && resource_value < mission.value("target", 0.0));
    if (type == "resource_value") value_target = std::max(value_target, mission.value("target", 0.0));
    if (type == "reach") {
      const Cell target = absolute_position(mission.at("position")); // 公开到点任务的绝对坐标。
      if (target.first >= 0 && target.first < width && target.second >= 0 && target.second < height)
        reach.push_back(target);
    }
  }
  int best_step = -1; // 当前收益最高的第一步；-1也可能表示值得留在原地。
  double best_utility = -1; // 当前最高启发式收益，-1表示还没有候选。
  const char *best_reason = "explore"; // 与best_step一起更新，便于日志解释。
  std::optional<Goal> best_goal; // 只有最终提交MOVE时才续签这一目标。
  double best_benefit = 0; // 同类资源的单位耗时收益打平时，优先兑现总收益更多的串。
  std::vector<std::pair<double, Cell>> resource_options; // 单目标基线排序，供有界资源搜索挑候选。
  std::array<double, 5> direction_value; direction_value.fill(-1);
  std::array<double,5> direction_benefit{};
  std::array<std::vector<Cell>,5> direction_plan; // 只在该方向最终仍以资源串最优时保存。
  std::array<std::optional<Goal>, 5> direction_goal; // 战术改选另一首步时，保留那个方向真实对应的目标。
  const auto consider = [&](int step, double utility, const char *reason, std::optional<Cell> target = std::nullopt, double benefit = 0) {
    // step=-1代表留在当前格；安全检查也必须允许这一Tick继续原地驻留。
    if (!safe_steps[step < 0 ? 4 : step]) return false;
    if (stage_ == 2 && step >= 0 && utility > 0) {
      const Cell next{start.first + steps[step].first, start.second + steps[step].second}; // 我方本Tick拟移动落点。
      unsigned contests = 0, pressures = 0; // 预计争同格、下一Tick可能入射线的稳定轨迹数；不是实测概率。
      for (const auto &enemy : enemies) {
        if (!enemy.forecast) continue;
        contests += *enemy.forecast == next;
        pressures += ray(*enemy.forecast, next) >= 0;
      }
      // 预测错了也只影响收益排序；不封路、不伪造敌车、不用MOVE躲本Tick已经命中的攻击。
      utility /= 1.0 + parameters_.opponent_contest_cost * contests + parameters_.opponent_pressure_cost * pressures;
    }
    const std::string kind = std::string(reason) == "collect_resource_chain" ? "collect_resource" : reason;
    if (keep_goal && target && previous_goal->kind == kind && previous_goal->position == *target)
      utility *= 1.0 + parameters_.goal_retention_bonus; // 只影响排序，危险守卫已在前面执行。
    const int index = step < 0 ? 4 : step;
    const bool improves_direction=utility>direction_value[index]+1e-9 ||
      (std::abs(utility-direction_value[index])<=1e-9 && kind=="collect_resource" &&
       direction_goal[index] && direction_goal[index]->kind==kind && benefit>direction_benefit[index]+1e-9);
    if (improves_direction) {
      direction_value[index] = utility;direction_benefit[index]=benefit;direction_plan[index].clear();
      direction_goal[index] = target ? std::optional<Goal>(Goal{*target, kind, tick}) : std::nullopt;
    }
    if (utility > best_utility + 1e-9 || (std::abs(utility - best_utility) <= 1e-9 &&
        kind == "collect_resource" && best_goal && best_goal->kind == kind && benefit > best_benefit + 1e-9)) {
      best_utility = utility; best_step = step; best_reason = reason;
      best_benefit = benefit;
      best_goal = target ? std::optional<Goal>(Goal{*target, kind, tick}) : std::nullopt;
    }
    return improves_direction;
  };
  unsigned least_visits = std::numeric_limits<unsigned>::max(); // 已知路线候选中最少访问次数。
  int exploration = -1; // 没有更高收益时采用的探索第一步。
  for (const auto &cell : order) {
    const auto node = paths.at(cell); // 这个候选格的第一步和最短步数。
    const auto &tile = tiles_.at(cell); // 当前候选格的资源、观测时间和访问次数。
    if (node.first >= 0 && tile.visits < least_visits) { least_visits = tile.visits; exploration = node.first; }
    if (tile.value > 0) {
      const double freshness = 1.0 / (1.0 + std::max<Tick>(0, tick - tile.last_seen) * parameters_.stale_resource_decay); // 旧资源记忆可信权重。
      double benefit = tile.value; // 第一阶段按资源价值；第二阶段改用封顶分差和任务机会。
      if (stage_ == 2) {
        // R=119再拿20，只剩1点资源价值参与封顶计分，不能按20全额估值。
        benefit = rmtc::score_delta(resource_value, tile.value, 120, 30) + std::min(tile.value, 20.0) * 0.001;
        if ((tile.high && need_high) || (!tile.high && need_normal)) benefit += parameters_.resource_mission_bonus;
        if (need_value) benefit += tile.value * 0.1;
      }
      const double utility = benefit * freshness / (node.distance + 1);
      consider(node.first, utility, "collect_resource", cell, benefit * freshness);
      if (node.first >= 0) resource_options.push_back({utility, cell});
    }
    if (stage_ == 1) continue;
    for (const auto &target : reach) {
      if (cell == target) consider(node.first, parameters_.reach_mission_value / (node.distance + 1), "reach_mission", target);
      else if (node.first >= 0) {
        // 目标尚未探明时只走已知路线中靠近目标的格子，不穿过未知墙。
        int unknown_neighbors = 0; // 这个格子旁有多少尚未探明的地图内邻格。
        for (const auto &delta : steps) {
          const Cell next{cell.first + delta.first, cell.second + delta.second}; // delta为四向位移，next用于检查未知边界。
          if (next.first >= 0 && next.first < width && next.second >= 0 && next.second < height &&
              !tiles_.count(next)) ++unknown_neighbors;
        }
        if (unknown_neighbors && manhattan(cell, target) < manhattan(start, target))
          consider(node.first, 2.0 / (node.distance + 1) + 1.0 / (manhattan(cell, target) + 1), "explore_toward_mission", target);
      }
    }
    for (const auto &[key, e] : events_) {
      if (!e.active(tick)) continue;
      if (e.kind == Kind::heal && hp < parameters_.healing_below_hp && healing_until_ <= tick + 2 && e.contains(cell) && cell != start &&
          tick + node.distance - 1 < e.end)
        consider(node.first, std::min(100.0 - hp, e.amount * e.effect_ticks) * 0.2 / (node.distance + 1), "seek_healing", cell);
      if (e.kind == Kind::fortress && !empowered_ && e.contains(cell) &&
          e.end - tick >= node.distance + e.capture_ticks - capture_progress_[key] - (node.distance > 0 ? 1 : 0)) {
        // 路程太远、事件剩余时间不足就不追；已在堡垒内则重视保住进度。
        if (cell == start) consider(-1, parameters_.fortress_hold_value, "hold_fortress");
        else consider(node.first, parameters_.fortress_capture_value / (node.distance + e.capture_ticks), "capture_fortress", cell);
      }
      if (e.kind == Kind::chili && (e.contains(cell) || (e.contains(start) && node.distance == 1))) {
        const Cell fire_point = e.contains(start) ? start : cell; // 计划进入/重新进入的辣椒点。
        double reward = 0; // 这条火线的机会收益；尚未发生，不能加到官方D里。
        for (const auto &enemy : enemies) {
          if (!enemy.protected_spawn && enemy.position != fire_point &&
              (enemy.position.first == fire_point.first || enemy.position.second == fire_point.second))
            // 辣椒损血本身不加D；击杀才可能分到60D奖励，不能按普通伤害刷分。
            reward += enemy.hp <= e.amount ? combat_value(60) : 0.02;
        }
        if (reward > 0 && cell != start)
          consider(node.first, reward / (node.distance + (e.contains(start) ? 2 : 1)), "trigger_chili", fire_point);
      }
    }
    for (const auto &enemy : enemies) {
      if (!enemy.protected_spawn && node.first >= 0 && ray(cell, enemy.position) >= 0) {
        const double damage = enemy.defending ? defended_damage_ : attack_damage_; // 接近后按当前可见状态估计伤害。
        const double contribution = std::min(damage, enemy.hp) + (enemy.hp <= damage ? 60.0 : 0.0); // 实际损血加可能击杀奖励。
        consider(node.first, (combat_value(contribution) + (need_damage ? 0.5 : 0.0)) /
                 (node.distance + 2), "approach_enemy");
      }
    }
  }
  if (!reverse_unknown_ && resource_options.size() >= 2) {
    std::stable_sort(resource_options.begin(), resource_options.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<Cell> targets; // 只截取少量已知资源；候选数与地图资源总数无关。
    for (const auto &[utility, cell] : resource_options) {
      (void)utility;
      if (targets.size() >= static_cast<std::size_t>(parameters_.resource_candidate_limit)) break;
      targets.push_back(cell);
    }
    std::array<bool, 4> first_allowed = {}; // 复用已做完的本Tick时序、占位、火线和安全接续检查。
    for (int i = 0; i < 4; ++i) {
      const Cell next{start.first + steps[i].first, start.second + steps[i].second};
      const auto route = paths.find(next);
      first_allowed[i] = route != paths.end() && route->second.distance == 1 && route->second.first == i;
    }
    for (const auto &plan : resource_plans(start, tick, remaining_ticks, targets, occupied,
                                         first_allowed, resource_value, need_normal, need_high, value_target,
                                         keep_goal ? previous_plan : std::vector<Cell>())) {
      if (consider(plan.first, plan.utility, "collect_resource_chain", plan.first_resource, plan.benefit))
        direction_plan[plan.first]=plan.stops;
    }
  }
  if (best_utility < 0 && remaining_ticks > 1) {
    // 没有资源/任务等更明确目标时，前往能增加可见范围的已知落点；不假设未知格里有什么。
    for (const Cell &cell : order) {
      const auto node = paths.at(cell); // 到观察点的真实绕障距离和首方向。
      if (node.first < 0 || node.distance >= remaining_ticks) continue;
      int newly_visible = 0; // 用本帧实际视野半径计数，不固定写成9×9。
      for (int y = std::max(0, cell.second - radius); y <= std::min(height - 1, cell.second + radius); ++y) {
        for (int x = std::max(0, cell.first - radius); x <= std::min(width - 1, cell.first + radius); ++x) {
          const auto tile = tiles_.find({x, y}); // 只估计能多看多少格，未知内容仍然未知。
          newly_visible += tile == tiles_.end() || !tile->second.seen;
        }
      }
      if (newly_visible > 0)
        consider(node.first, parameters_.frontier_value * newly_visible / (node.distance + 1), "explore_frontier", cell);
    }
  }
  if (best_utility < 0 && exploration >= 0) consider(exploration, 0.01, "explore");

  // 6. 用移动前站位评估ATTACK。普通攻击只取最近一辆，无双才取整个5×5。
  int attack_direction = -1; // 当前最好的攻击方向；-1表示没有有价值的攻击。
  double attack_utility = -1; // 与其他任务收益比较的攻击估值。
  if (stage_ == 2 && !protected_spawn) {
    for (int direction = 0; direction < 4; ++direction) {
      std::vector<const Enemy *> targets; // 此方向实际可命中的敌车引用，不复制敌车数据。
      if (empowered_) {
        for (const auto &enemy : enemies)
          if (std::abs(enemy.position.first - start.first) <= 2 && std::abs(enemy.position.second - start.second) <= 2)
            targets.push_back(&enemy);
      } else {
        const Enemy *nearest = nullptr; // 普通攻击只能命中的最近一辆，包括有保护的挡路目标。
        for (const auto &enemy : enemies)
          if (ray(start, enemy.position) == direction && (!nearest ||
              manhattan(start, enemy.position) < manhattan(start, nearest->position))) nearest = &enemy;
        if (nearest) targets.push_back(nearest);  // 近处保护敌车仍阻挡后方，障碍不挡攻击。
      }
      double contribution = 0, reflected = 0; // 预计获得D、预计自己承受的反伤。
      for (const auto *enemy : targets) {
        if (enemy->protected_spawn) continue;
        if (reflection && enemy->defending) { reflected += reflection_amount; continue; }
        const double damage = enemy->defending ? defended_damage_ : attack_damage_; // 此目标预计受到的普通/防御伤害。
        // 按独自击杀估值；多人同时贡献时奖励会平分，所以只是机会估计。
        contribution += std::min(damage, enemy->hp) + (enemy->hp <= damage ? 60.0 : 0.0);
      }
      double utility = combat_value(contribution) + (need_damage && contribution > 0 ? 0.5 : 0.0); // 此攻击方向的最终机会分。
      utility -= reflected * 0.05;
      if (reflected + pre_damage >= hp) continue;
      if (utility > attack_utility) { attack_utility = utility; attack_direction = direction; }
    }
  }
  // 7. 选本Tick动作。驻留时仍可攻击/防御；上一帧defending不是对手这一Tick的承诺。
  bool split_activation = false;
  for (const auto &[key, event] : events_) {
    (void)key;
    split_activation = split_activation || (event.kind == Kind::split && event.start >= tick && event.start <= tick + 2);
  }
  if (stage_ == 2 && parameters_.tactical_search && !enemies.empty() && enemies.size() <= 3 &&
      !reverse_unknown_ && !split_activation && remaining_ticks >= 2) {
    rmtc::TacticalInput input{};
    input.tick=tick; input.remaining_ticks=remaining_ticks; input.width=width; input.height=height;
    for (auto &car : input.fighters) car.alive=false;
    input.fighters[0]={start,hp,attack_damage_,defended_damage_,vulnerability_,true,protected_spawn,empowered_};
    for (std::size_t i=0;i<enemies.size();++i) {
      const auto &enemy=enemies[i];
      input.fighters[i+1]={enemy.position,enemy.hp,20,10,1,true,enemy.protected_spawn,false};
      input.forecast[i+1]=enemy.forecast;
    }
    for (const auto &[cell,tile] : tiles_) if (tile.seen && !tile.wall) {
      input.walkable.insert(cell);
      if (tile.value > 0 && tile.last_seen == tick) input.resources[cell]=tile.value; // 两步精确拾取只用本帧资源，旧记忆仍走目标估值。
    }
    for (const auto &[key,event] : events_) { (void)key;input.events.push_back(event); }
    for (const auto &mission : missions) if (!completed.count(mission.at("id").get<std::string>())) {
      rmtc::TacticalMission task{mission.at("type"),mission.value("target",1.0),{0,0}};
      if (task.type == "reach") task.position=absolute_position(mission.at("position"));
      input.missions.push_back(task);
    }
    input.goal_value=direction_value; input.safe_first=safe_steps;
    for (int direction=0;direction<5;++direction) if (direction_goal[direction]) input.goal_position[direction]=direction_goal[direction]->position;
    input.resource_total=resource_value;input.damage_total=effective_damage;
    input.normal_count=observed_normal_;input.high_count=observed_high_;input.counts_known=resource_counts_known_;
    input.model_reply_seen=model_reply_seen_;input.possible_enemy_area=potential_area_enemy;input.possible_area_damage=potential_area_damage;
    input.healing_until=healing_until_;input.healing_amount=healing_amount_;input.spawn=spawn_;input.capture_progress=capture_progress_;
    const auto choice=rmtc::tactical_choice(input,parameters_);
    if (choice.action.kind == rmtc::rules::ActionKind::move) {
      goal_=direction_goal[choice.action.direction];planned_resources_=direction_plan[choice.action.direction];return move(choice.action.direction,"tactical_reposition");
    }
    if (choice.action.kind == rmtc::rules::ActionKind::attack) {
      reason_="tactical_attack";return {{"action","attack"},{"direction",directions[choice.action.direction]}};
    }
    if (choice.action.kind == rmtc::rules::ActionKind::defend) return fallback("tactical_defend");
  }
  if (safe_steps[4] && attack_direction >= 0 && attack_utility > 0 &&
      (attack_utility > best_utility || std::string(best_reason) == "hold_fortress")) {
    reason_ = "attack_opportunity";
    return {{"action", "attack"}, {"direction", directions[attack_direction]}};
  }
  if (best_step >= 0 && !reverse_unknown_) {
    planned_resources_=direction_plan[best_step];
    goal_ = std::move(best_goal); // 记的是当前目标；下一帧仍重新BFS、重读真实位置与回执。
    return move(best_step, best_reason);
  }
  // 没有已知路线时，试探地图内的未知邻格。移动失败不加无效动作；位置仍取下一帧感知。
  if (best_utility < 0 && !reverse_unknown_) {
    int probe = -1; // 未知邻格中本次准备试探的方向。
    unsigned fewest = std::numeric_limits<unsigned>::max(); // 优先试以前较少试过的格子。
    for (int i = 0; i < 4; ++i) {
      const Cell next{start.first + steps[i].first, start.second + steps[i].second}; // i方向的待试探邻格。
      if (next.first < 0 || next.first >= width || next.second < 0 || next.second >= height ||
          occupied.count(next) || !safe_steps[i] || split_wall(next, tick) || dangerous(next, tick + 1) ||
          possible_fire(next) >= hp - pre_damage) continue;
      const auto tile = tiles_.find(next); // 邻格的已知地图记录，可能不存在。
      if (tile != tiles_.end() && (tile->second.seen || tile->second.wall)) continue;
      if (probe_attempts_[next] < fewest) { fewest = probe_attempts_[next]; probe = i; }
    }
    if (probe >= 0) {
      ++probe_attempts_[{start.first + steps[probe].first, start.second + steps[probe].second}];
      return move(probe, "probe_unknown_cell");
    }
  }
  if (stage_ == 2 && incoming > 0 && safe_steps[4]) { reason_ = "defend_while_holding"; return {{"action", "defend"}}; }
  if (!reverse_unknown_ && safe_steps[4] &&
      std::none_of(safe_steps.begin(), safe_steps.begin() + 4, [](bool value) { return value; }))
    return fallback("defend_for_safe_route");
  return fallback(reverse_unknown_ ? "uncertain_illusion" : best_utility >= 0 ? best_reason : "no_path");
}

