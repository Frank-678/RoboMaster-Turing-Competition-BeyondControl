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

namespace {
using Json = nlohmann::json;
using Cell = rmtc::Cell;
using Tick = rmtc::Tick;
using Kind = rmtc::EventKind;
namespace cfg = rmtc::match_config;
constexpr std::array<Cell, 4> steps = {{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
constexpr std::array<const char *, 4> directions = {{"up", "right", "down", "left"}};
int integer(const Json &j) {
  if (!j.is_number_integer() || (j.is_number_unsigned() &&
      j.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())))
    throw std::runtime_error("invalid integer field");
  const auto value = j.get<std::int64_t>();
  if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
    throw std::runtime_error("integer out of range");
  return static_cast<int>(value);
}
Cell absolute_position(const Json &j) {
  if (!j.is_array() || j.size() != 2) throw std::runtime_error("invalid absolute position");
  return {integer(j[0]), integer(j[1])};
}
std::optional<Cell> relative_position(const Json &position) {
  if (!position.is_string()) return std::nullopt;
  const auto &grid = position.get_ref<const std::string &>();
  if (grid.size() != 89 || grid[44] != '1') return std::nullopt;
  Cell offset{0, 0};
  bool found = false;
  for (int y = 0; y < 9; ++y) {
    if (y < 8 && grid[y * 10 + 9] != '\n') return std::nullopt;
    for (int x = 0; x < 9; ++x) {
      const char ch = grid[y * 10 + x];
      if (x == 4 && y == 4) continue;
      if (ch == '2' && !found) { offset = {x - 4, y - 4}; found = true; }
      else if (ch != '0') return std::nullopt;
    }
  }
  return offset;
}
int manhattan(Cell a, Cell b) {
  return std::abs(a.first - b.first) + std::abs(a.second - b.second);
}
int ray(Cell from, Cell target) {
  const int dx = target.first - from.first, dy = target.second - from.second;
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
  recent_navigation_.clear();loop_cells_.clear();recovery_goal_.reset();deferred_goals_.clear();
  recovery_until_=navigation_tick_=progress_tick_=-1;recovery_count_=0;recovery_blocked_=false;
  navigation_r_=navigation_d_=navigation_hp_=0;navigation_known_=navigation_missions_=0;navigation_capture_=0;
  goal_.reset(); // 新场没有上一目标，也没有必须继续执行的旧路线。
  model_reply_seen_ = resource_counts_known_ = false;
  observed_normal_ = observed_high_ = 0; previous_score_tick_ = -1; previous_resource_total_ = 0;
  width_ = height_ = 0; reversed_ = reverse_unknown_ = empowered_ = false;
  vulnerability_ = 1; attack_damage_ = 20; defended_damage_ = 10; healing_until_ = -1;
  healing_amount_ = 0; reason_ = "new_session";
}
// 只用已执行的连续观测识别“动了但没进展”；没有可信回执就重新积累窗口。
void Strategy::observe_navigation(const Json &p, Cell position, Tick tick, Tick receipt_tick,
    bool respawn, const std::optional<Goal> &previous_goal) {
  if (!parameters_.loop_recovery) return;
  const auto score=p.value("score",Json::object());
  const double resource=score.value("resource_value",0.0),damage=score.value("effective_damage",0.0);
  const double hp=p.at("self").value("hp",0.0);
  const auto missions=score.value("mission_completed_ids",Json::array()).size();
  int capture=0;for(const auto &[key,count]:capture_progress_) { (void)key;capture+=count; }
  const bool discontinuous=navigation_tick_!=tick-1 || (tick>0 && receipt_tick!=tick-1) || respawn;
  for(auto it=deferred_goals_.begin();it!=deferred_goals_.end();) {
    if(it->second<=tick) it=deferred_goals_.erase(it);else ++it;
  }
  if(discontinuous) {
    recent_navigation_.clear();loop_cells_.clear();recovery_goal_.reset();deferred_goals_.clear();
    recovery_until_=-1;progress_tick_=tick;recovery_blocked_=false;
  }
  const bool progress=discontinuous || resource>navigation_r_ || damage>navigation_d_ || hp>navigation_hp_ ||
    tiles_.size()>navigation_known_ || missions>navigation_missions_ || capture>navigation_capture_;
  if(progress) {
    recent_navigation_.clear();progress_tick_=tick;
    if(recovery_blocked_) {recovery_blocked_=false;loop_cells_.clear();recovery_goal_.reset();recovery_until_=-1;}
  }
  navigation_tick_=tick;navigation_r_=resource;navigation_d_=damage;navigation_hp_=hp;
  navigation_known_=tiles_.size();navigation_missions_=missions;navigation_capture_=capture;
  // 新视野本身不会打断尚有效的脱困目标，防止刚挪一步又被旧吸引点拉回。
  recent_navigation_.push_back({tick,position,previous_goal});
  while(recent_navigation_.size()>static_cast<std::size_t>(parameters_.loop_window_ticks)) recent_navigation_.pop_front();
  if(recent_navigation_.size()<static_cast<std::size_t>(parameters_.loop_window_ticks)) return;
  std::set<Cell> cells;for(const auto &sample:recent_navigation_) cells.insert(sample.position);
  bool periodic=false;
  for(std::size_t period=1;period*2<=recent_navigation_.size();++period) {
    bool matches=true;
    for(std::size_t i=period;i<recent_navigation_.size();++i)
      matches=matches && recent_navigation_[i].position==recent_navigation_[i-period].position;
    periodic=periodic || matches;
  }
  if(!periodic && cells.size()>4) return; // 长走廊前进、扩大探索等不会仅因低分就被判卡住。
  for(const auto &sample:recent_navigation_) if(sample.goal && sample.goal->kind!="unstuck")
    deferred_goals_[{sample.goal->kind,sample.goal->position}]=tick+parameters_.stalled_goal_cooldown_ticks;
  if(recovery_blocked_) loop_cells_.insert(cells.begin(),cells.end());else loop_cells_=std::move(cells);
  recovery_goal_.reset();recovery_until_=tick+parameters_.recovery_ticks;
  recent_navigation_.clear();if(!recovery_blocked_) ++recovery_count_;
}
Json Strategy::navigation_diagnostics() const {
  return {{"stagnant_ticks",navigation_tick_<0 || progress_tick_<0 ? 0 : navigation_tick_-progress_tick_},
    {"recoveries",recovery_count_},{"recovering",navigation_tick_>=0 && navigation_tick_<recovery_until_},
    {"target",recovery_goal_ ? Json{recovery_goal_->first,recovery_goal_->second} : Json()},
    {"deferred_goals",deferred_goals_.size()},{"no_safe_exit",recovery_blocked_}};
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
  for (const auto &e : events) events_[e.key] = e;
}
Json Strategy::known_events() const {
  auto result = Json::array();
  for (const auto &[key, event] : events_) { (void)key; result.push_back(rmtc::event_summary(event)); }
  return result;
}
bool Strategy::split_wall(Cell cell, Tick tick) const {
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
  reversed_ = reverse_unknown_ = empowered_ = false;
  vulnerability_ = 1; attack_damage_ = 20; defended_damage_ = 10; healing_until_ = -1; healing_amount_ = 0;
  capture_progress_.clear();
  for (const auto &[key, e] : events_) {
    if (e.kind == Kind::illusion && e.active(tick)) {
      const auto initial = history_.find(e.start);
      if (initial == history_.end()) reverse_unknown_ = true;
      else if (initial->second.alive && e.contains(initial->second.position)) reversed_ = true;
    }
    int consecutive = 0;
    Tick previous = -2;
    bool awarded = false;
    for (const auto &[state_tick, state] : history_) {
      (void)state_tick;
      const auto executed = state.receipt_tick;
      if (executed < e.start || executed >= e.end) continue;
      if (e.kind == Kind::vulnerability && e.active(tick) && state.moved && executed < e.trigger_end)
        vulnerability_ = std::max(vulnerability_, e.multiplier);
      if (e.kind == Kind::heal && state.moved && !state.respawn && e.contains(state.position) && executed + e.effect_ticks > healing_until_) {
        healing_until_ = executed + e.effect_ticks; healing_amount_ = e.amount;
      }
      if (e.kind == Kind::fortress && e.active(tick)) {
        if (executed != previous + 1) consecutive = 0;
        consecutive = state.alive && !state.respawn && e.contains(state.position) ? consecutive + 1 : 0;
        previous = executed;
        if (consecutive >= e.capture_ticks) awarded = true;
      }
    }
    if (e.kind == Kind::fortress && e.active(tick)) {
      capture_progress_[key] = consecutive;
      if (awarded) {
        empowered_ = true;
        attack_damage_ = std::max(attack_damage_, e.amount);
        defended_damage_ = e.defended_damage;
      }
    }
  }
}

std::optional<Strategy::ResourcePlan> Strategy::resource_tour(Cell start, Tick remaining_ticks,
    const std::vector<Cell> &targets, const std::set<Cell> &occupied,
    const std::map<Cell, double> *values, Cell first_hint) const {
  if (targets.size() < 2 || targets.size() > 16) return std::nullopt; // 只对已知资源巡回；数量有界，不承诺全局最优。
  const auto walkable = [&](Cell cell) {
    if (cell.first < 0 || cell.second < 0 || cell.first >= width_ || cell.second >= height_) return false;
    const auto tile = tiles_.find(cell);
    return tile != tiles_.end() && tile->second.seen && !tile->second.wall && !occupied.count(cell);
  };
  // 只走已知格；资源两两距离各跑一次BFS，起点单独一份并保留首方向。全部仅本帧有效。
  const auto bfs = [&](Cell origin) {
    std::map<Cell,std::pair<int,int>> route; // 格子→{第一步方向,距离}；起点自己{4,0}。
    std::queue<Cell> queue;
    if (!walkable(origin)) return route;
    route[origin] = {4, 0}; queue.push(origin);
    while (!queue.empty()) {
      const Cell cell = queue.front(); queue.pop();
      for (int direction = 0; direction < 4; ++direction) {
        const Cell next{cell.first + steps[direction].first, cell.second + steps[direction].second};
        if (!walkable(next) || route.count(next)) continue;
        const auto node = route.at(cell);
        route[next] = {node.second == 0 ? direction : node.first, node.second + 1};
        queue.push(next);
      }
    }
    return route;
  };
  const auto start_route = bfs(start);
  std::vector<Cell> nodes; // 起点已知可达的资源才进入巡回；其余留给探索与下一帧。
  std::map<Cell,std::map<Cell,int>> table; // 资源→到其他已知格的距离。
  for (const Cell &cell : targets) {
    if (cell == start) continue; // 脚下资源由单目标原地拾取，不进巡回。
    const auto found = start_route.find(cell);
    if (found == start_route.end()) continue;
    nodes.push_back(cell);
    std::map<Cell,int> distances;
    for (const auto &entry : bfs(cell)) distances[entry.first] = entry.second.second;
    table[cell] = std::move(distances);
  }
  if (nodes.size() < 2) return std::nullopt;
  const std::size_t size = nodes.size() + 1; // 下标0是当前格，1..k是资源。
  std::vector<std::vector<int>> matrix(size, std::vector<int>(size, 1000000)); // 大数只表示未找到已知路线。
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    matrix[0][i + 1] = matrix[i + 1][0] = start_route.at(nodes[i]).second;
    matrix[i + 1][i + 1] = 0;
    for (std::size_t j = i + 1; j < nodes.size(); ++j) {
      int leg = 1000000;
      const auto row = table.find(nodes[i]);
      if (row != table.end()) {
        const auto found = row->second.find(nodes[j]);
        if (found != row->second.end()) leg = found->second;
      }
      matrix[i + 1][j + 1] = matrix[j + 1][i + 1] = leg;
    }
  }
  std::vector<std::size_t> order{0}; // 近邻构造的访问顺序；不是待执行动作队列。
  std::vector<bool> used(size, false); used[0] = true;
  while (order.size() < size) {
    std::size_t best = size; int best_leg = std::numeric_limits<int>::max();
    for (std::size_t i = 1; i < size; ++i)
      if (!used[i] && matrix[order.back()][i] < best_leg) { best_leg = matrix[order.back()][i]; best = i; }
    if (best == size) break; // 与已知格图断开的资源不硬凑路线。
    order.push_back(best); used[best] = true;
  }
  if (order.size() < 3) return std::nullopt; // 少于两个资源时单目标逻辑已足够。
  if (first_hint.first >= 0) { // 观察点巡回沿用上一帧首停点：它仍在边界上就固定为本周起点，保持扫掠方向稳定。
    for (std::size_t k = 2; k < order.size(); ++k)
      if (nodes[order[k] - 1] == first_hint) {
        std::rotate(order.begin() + 1, order.begin() + k, order.begin() + k + 1);
        break;
      }
  }
  for (int pass = 0; pass < 6; ++pass) { // 有界2-opt：只改善访问顺序，不改距离或已结算Tick。
    bool improved = false;
    // 观察点巡回在已有锚点（上一帧首停点）时保留它：首停点若随2-opt整体翻转，对称边界上会每帧在两端来回跳（原地抖动）。
    // 探索一段的起始帧没有锚点，允许2-opt自由选择整串起点，扫掠方向更有全局眼光。
    for (std::size_t i = (values != nullptr && first_hint.first >= 0) ? 1 : 0; i + 2 < order.size(); ++i) {
      for (std::size_t j = i + 2; j + 1 < order.size(); ++j) {
        const int removed = matrix[order[i]][order[i + 1]] + matrix[order[j]][order[j + 1]];
        const int added = matrix[order[i]][order[j]] + matrix[order[i + 1]][order[j + 1]];
        if (added < removed) { std::reverse(order.begin() + i + 1, order.begin() + j + 1); improved = true; }
      }
    }
    if (!improved) break;
  }
  double benefit = 0; Tick elapsed = 0; // 剩余窗口内能兑现的巡回前缀收益。
  std::vector<Cell> stops;
  for (std::size_t i = 1; i < order.size(); ++i) {
    const int leg = matrix[order[i - 1]][order[i]];
    if (leg >= 1000000 || elapsed + leg > remaining_ticks) break; // 装不下的留给下一帧重评。
    elapsed += leg;
    const auto tile = tiles_.find(nodes[order[i] - 1]);
    double value = 0;
    if (values != nullptr) { // 边界观察点巡回：价值来自调用方估值，不读写资源记录。
      const auto found = values->find(nodes[order[i] - 1]);
      if (found == values->end() || found->second <= 0) continue;
      value = found->second;
    } else {
      if (tile == tiles_.end() || tile->second.value <= 0) continue; // 本帧内消失的资源不计数。
      value = tile->second.value;
    }
    benefit += value;
    stops.push_back(nodes[order[i] - 1]);
  }
  if (stops.empty()) return std::nullopt;
  return ResourcePlan{start_route.at(stops.front()).first, stops.front(),
    benefit / static_cast<double>(elapsed + 1), benefit, stops};
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
      radius < 0 || radius > 4 || !p.at("visible_entities").is_array()) return wait("invalid_frame");
  if (width_ != 0 && (width != width_ || height != height_)) return wait("map_changed_without_session");
  const bool protected_spawn = self.value("protected", false);
  const double hp = self.value("hp", 100.0);
  if (!std::isfinite(hp) || hp < 0) return wait("invalid_hp");
  const bool respawn = self.value("position_reason", std::string()) == "respawn";
  const auto receipt = self.value("last_action", Json());
  bool moved = false;
  Tick receipt_tick = -1;
  if (receipt.is_object() && receipt.contains("tick") && receipt.at("tick").is_number_integer()) {
    const auto reported = receipt.at("tick").get<Tick>();
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
  if (!alive) {
    recent_navigation_.clear();loop_cells_.clear();recovery_goal_.reset();deferred_goals_.clear();
    recovery_until_=navigation_tick_=-1;recovery_blocked_=false;return skip("dead");
  }

  std::vector<Entity> entities;
  std::vector<Enemy> enemies;
  std::set<Cell> occupied;
  for (const auto &j : p.at("visible_entities")) {
    const auto type = j.at("type").get<std::string>();
    if (type != "obstacle" && type != "robot" && type != "resource_normal" && type != "resource_high") continue;
    const auto offset = relative_position(j.at("position"));
    if (!offset) return wait("invalid_entity_grid");
    const auto [dx, dy] = *offset;
    if (std::abs(dx) > radius || std::abs(dy) > radius || dx < -start.first ||
        dx >= width - start.first || dy < -start.second || dy >= height - start.second)
      return wait("invalid_entity_coordinate");
    const Cell cell{start.first + dx, start.second + dy};
    const double value = j.value("value", type == "resource_high" ? 20.0 : 10.0);
    if (!std::isfinite(value) || value < 0) return wait("invalid_resource_value");
    entities.push_back({type, cell, value, value >= 20});
    if (type == "robot") {
      const double enemy_hp = j.value("hp", 100.0);
      if (!std::isfinite(enemy_hp) || enemy_hp < 0) return wait("invalid_enemy_hp");
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
      auto &tile = tiles_[{x, y}];
      tile.seen = true; tile.wall = false; tile.value = 0; tile.last_seen = tick;
    }
  }
  for (const auto &entity : entities) {
    auto &tile = tiles_.at(entity.position);
    if (entity.type == "obstacle") {
      tile.wall = true;
      if (tile.wall_since < 0) tile.wall_since = tick;
    } else if (entity.type == "resource_normal" || entity.type == "resource_high") {
      tile.value = entity.value; tile.high = entity.high;
    }
  }
  for (auto &[cell, tile] : tiles_) {
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
  observe_navigation(p,start,tick,receipt_tick,respawn,previous_goal);
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
  bool reflection = false, potential_area_enemy = false;
  double reflection_amount = 0, pre_damage = 0, potential_area_damage = 0;
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
    if (e.kind == Kind::split && e.start == tick && split_wall(start, tick)) return wait("split_forced_wait");
  }
  if (pre_damage >= hp) return skip("lethal_activation_before_action");
  double incoming = 0, defended_incoming = 0; // 不防御/防御时的本Tick可能受伤总量（机会口径：预测防御的敌车不计入）。
  double incoming_worst = 0, defended_incoming_worst = 0; // 全敌攻击的最坏情形（生存口径：防御预测不是对方已提交动作）。
  // 敌车通用的防御规则只看公开信息（各射线内可见车辆的伤害与自身生命），是对陪练策略的推测，
  // 不是对方本Tick已提交的动作：只用于机会估值，不能作为伤害上界里的确定排除项。
  // 生存判定按全敌攻击的最坏情形算，避免低血量互杀里把预测防御当成免伤承诺而白送一命。
  const auto enemy_will_defend = [&](const Enemy &enemy) {
    if (enemy.protected_spawn) return false;
    double incoming_e = 0;
    for (int direction = 0; direction < 4; ++direction) {
      bool any = ray(enemy.position, start) == direction; // 本车在其射线内。
      for (const auto &other : enemies)
        if (other.position != enemy.position && manhattan(enemy.position, other.position) <= radius &&
            ray(enemy.position, other.position) == direction) any = true; // 视野半径公开，超出的车辆它看不见。
      if (any) incoming_e += 20;
    }
    return incoming_e >= enemy.hp && incoming_e / 2 < enemy.hp;
  };
  // 这是可见敌车可能造成的伤害上界，不是对手已提交的动作或实测概率。
  for (const auto &enemy : enemies) {
    if (enemy.protected_spawn || protected_spawn) continue;
    bool nearest = ray(enemy.position, start) != -1;
    if (nearest) for (const auto &other : enemies)
      if (other.position != enemy.position && ray(enemy.position, other.position) == ray(enemy.position, start) &&
          manhattan(enemy.position, other.position) < manhattan(enemy.position, start)) nearest = false;
    if (nearest) {
      // 未观测确认的敌方无双只进入后面的不确定情景，不能把所有敌车一律当50伤害强制原地防御。
      incoming_worst += 20 * vulnerability_;
      defended_incoming_worst += reflection ? 0 : 10 * vulnerability_;
      if (!enemy_will_defend(enemy)) {
        incoming += 20 * vulnerability_;
        defended_incoming += reflection ? 0 : 10 * vulnerability_;
      }
    }
  }
  // MOVE 无法躲开本 Tick 已经先结算的战斗；防御可能保命时先防御。
  // worst_case_survival=1 时按全敌攻击的最坏情形判定；0 保留旧行为（预测防御的敌车不计入）。
  const double survival_incoming = parameters_.worst_case_survival ? incoming_worst : incoming;
  const double survival_defended = parameters_.worst_case_survival ? defended_incoming_worst : defended_incoming;
  if (stage_ == 2 && survival_incoming >= hp - pre_damage && survival_defended < hp - pre_damage) {
    // 连续致命防御=对手蹲守连击，驻守无进展（crossroads-542曾连续49Tick站防、R仅50）。
    // 超过阈值后脱离接触：本Tick战斗在移动前结算，此时脱离是接受死亡代价的主动重生换位
    // （落点仍选距最近敌车最远的安全邻格，只决定重生后的位置）；无路可退才继续防御保命。
    if (parameters_.defend_stall_break_ticks > 0 && lethal_defend_streak_ >= parameters_.defend_stall_break_ticks) {
      int retreat = -1, retreat_gain = 0;
      for (int direction = 0; direction < 4; ++direction) {
        const Cell next{start.first + steps[direction].first, start.second + steps[direction].second};
        const auto tile = tiles_.find(next);
        if (tile == tiles_.end() || !tile->second.seen || tile->second.wall) continue;
        if (occupied.count(next) || dangerous(next, tick + 1)) continue;
        int nearest = 1 << 30;
        for (const auto &enemy : enemies)
          if (!enemy.protected_spawn) nearest = std::min(nearest, manhattan(enemy.position, next));
        if (nearest > retreat_gain) { retreat_gain = nearest; retreat = direction; }
      }
      if (retreat >= 0) {
        lethal_defend_streak_ = 0; // 脱离后重新计数，避免一步脱离又立刻回防的振荡。
        return move(retreat, "break_defend_stall");
      }
    }
    ++lethal_defend_streak_;
    reason_ = "defend_lethal_threat";
    return {{"action", "defend"}};
  }

  // 对手下一步可能踩辣椒，属于保守估计；不把其未知动作当作事实。
  const auto possible_fire = [&](Cell cell) {
    double damage = 0;
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
  std::array<double,4> fire_damage{}, fire_cost{}; // 四个首步的假设火线伤害、无量纲风险折扣；不是发生概率。
  std::array<int,4> first_order={0,1,2,3}; // 相同距离优先低风险首步；完全相同仍按上右下左。
  for(int direction=0;direction<4;++direction) {
    const Cell next{start.first+steps[direction].first,start.second+steps[direction].second};
    fire_damage[direction]=possible_fire(next);
    fire_cost[direction]=1.0+fire_damage[direction]/std::max(1.0,hp-pre_damage);
  }
  std::stable_sort(first_order.begin(),first_order.end(),[&](int a,int b) {return fire_damage[a]<fire_damage[b];});

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
    for (int ordinal = 0; ordinal < 4; ++ordinal) {
      const int i=node.first==-1 ? first_order[ordinal] : ordinal; // 只调整首层顺序，后续仍是等步长BFS。
      if (node.first == -1 && !safe_steps[i]) continue;
      const Cell next{current.first + steps[i].first, current.second + steps[i].second}; // i是上右下左索引，next是邻格。
      const auto tile = tiles_.find(next); // 邻格的已知地图记录，可能不存在。
      if (tile == tiles_.end() || !tile->second.seen || tile->second.wall || occupied.count(next) ||
          paths.count(next) || split_wall(next, tick + node.distance) ||
          dangerous(next, tick + node.distance + 1)) continue;
      // 对手会不会踩点还未知，不能据此封路。否则可能删掉已公告爆炸的唯一逃路。
      paths[next] = {node.first == -1 ? i : node.first, node.distance + 1};
      queue.push(next);
    }
  }
  if (stage_ == 2 && possible_fire(start) >= hp - pre_damage && !reverse_unknown_) {
    for (const auto &cell : order) {
      const auto node = paths.at(cell);
      if (node.distance == 1 && possible_fire(cell) < hp - pre_damage)
        return move(node.first, "avoid_possible_chili_fire");
    }
  }
  // 即将到期的危险先退出；本 Tick 才激活的爆炸不能靠移动补救。
  for (const auto &[key, e] : events_) {
    (void)key;
    if ((e.kind != Kind::explosion && e.kind != Kind::split) || e.start <= tick || !e.contains(start)) continue;
    if (e.kind == Kind::split && spawn_ && start == *spawn_) continue;
    int nearest_exit = std::numeric_limits<int>::max(), escape = -1;
    for (const auto &cell : order) {
      const auto node = paths.at(cell);
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
    if (completed.count(mission.at("id").get<std::string>())) continue;
    const auto type = mission.at("type").get<std::string>();
    need_normal = need_normal || type == "normal_resources";
    need_high = need_high || type == "high_resources";
    need_damage = need_damage || (type == "effective_damage" && effective_damage < mission.value("target", 0.0));
    need_value = need_value || (type == "resource_value" && resource_value < mission.value("target", 0.0));
    if (type == "resource_value") value_target = std::max(value_target, mission.value("target", 0.0));
    if (type == "reach") {
      const Cell target = absolute_position(mission.at("position"));
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
      utility /= fire_cost[step] *
        (1.0 + parameters_.opponent_contest_cost * contests + parameters_.opponent_pressure_cost * pressures);
    }
    const std::string normalized = std::string(reason);
    const std::string kind = (normalized == "collect_resource_chain" || normalized == "collect_resource_tour") ? "collect_resource" : reason;
    if(target) {
      const auto deferred=deferred_goals_.find({kind,*target});
      if(deferred!=deferred_goals_.end() && tick<deferred->second) return false; // 只暂缓收益目标，不把道路封掉。
    }
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
  unsigned least_visits = std::numeric_limits<unsigned>::max();
  int exploration = -1;
  for (const auto &cell : order) {
    const auto node = paths.at(cell);
    const auto &tile = tiles_.at(cell);
    if (node.first >= 0 && tile.visits < least_visits) { least_visits = tile.visits; exploration = node.first; }
    if (tile.value > 0) {
      // 资源离开视野的第 stale_grace_ticks 个Tick不衰减：未观测≠消失，别为单Tick视野缩小反复换方向。
      // 0=旧行为：离视野当Tick即开始衰减。
      const double freshness = 1.0 / (1.0 + std::max<Tick>(0, tick - tile.last_seen - parameters_.stale_grace_ticks) * parameters_.stale_resource_decay); // 旧资源记忆可信权重。
      double benefit = tile.value; // 第一阶段按资源价值；第二阶段改用封顶分差和任务机会。
      if (stage_ == 2) {
        benefit = rmtc::score_delta(resource_value, tile.value, 120, 30) + std::min(tile.value, 20.0) * 0.001;
        // 资源先到先得：可见敌车的直线距离不比自己绕障距离长时按参数折扣，避开会被先拿走的格子。
        for (const auto &enemy : enemies)
          if (!enemy.protected_spawn && manhattan(enemy.position, cell) <= node.distance) { benefit *= parameters_.opponent_claim_discount; break; }
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
        int unknown_neighbors = 0;
        for (const auto &delta : steps) {
          const Cell next{cell.first + delta.first, cell.second + delta.second};
          if (next.first >= 0 && next.first < width && next.second >= 0 && next.second < height &&
              !tiles_.count(next)) ++unknown_neighbors;
        }
        if (unknown_neighbors && manhattan(cell, target) < manhattan(start, target))
          consider(node.first, parameters_.mission_explore_value / (node.distance + 1) + 1.0 / (manhattan(cell, target) + 1), "explore_toward_mission", target);
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
        const Cell fire_point = e.contains(start) ? start : cell;
        double reward = 0;
        for (const auto &enemy : enemies) {
          if (!enemy.protected_spawn && enemy.position != fire_point &&
              (enemy.position.first == fire_point.first || enemy.position.second == fire_point.second))
            // Chili damage itself adds no D; only a potential kill earns 60 D (§8.7).
            reward += enemy.hp <= e.amount ? combat_value(60) : 0.02;
        }
        if (reward > 0 && cell != start)
          consider(node.first, reward / (node.distance + (e.contains(start) ? 2 : 1)), "trigger_chili", fire_point);
      }
    }
    for (const auto &enemy : enemies) {
      if (!enemy.protected_spawn && node.first >= 0 && ray(cell, enemy.position) >= 0 &&
          !(reflection && (enemy.defending || enemy_will_defend(enemy)))) { // 当前预测攻击只会反伤，就不奖励反复接近同一防御目标。
        const double damage = (enemy.defending || enemy_will_defend(enemy)) ? defended_damage_ : attack_damage_; // 接近后按当前可见状态估计伤害。
        const double contribution = std::min(damage, enemy.hp) + (enemy.hp <= damage ? 60.0 : 0.0); // 实际损血加可能击杀奖励。
        // 竞速期间或D已封顶且没有伤害任务时，不再主动接近敌车开战；被本Tick火力波及时的还手不受限。
        // D封顶只表示官方原始分不再增加；同分同任务同R时裁判按D决胜，故封顶后按 post_cap_combat_weight
        // 给增量D留平局价值（0=旧行为：完全归零）。战例：f728一局A与C同45分同R同M，D556<1160，名次第3。
        const double approach_utility =
          (race_active || (effective_damage >= 500 && !need_damage && incoming == 0)) ?
            parameters_.post_cap_combat_weight * contribution / (node.distance + 2) :
            (combat_value(contribution) + (need_damage ? 0.5 : 0.0)) / (node.distance + 2);
        consider(node.first, approach_utility, "approach_enemy", enemy.position);
      }
    }
  }
  if (!reverse_unknown_ && parameters_.explore_toward_resource_value > 0) {
    // 已知资源尚未连通、或已知绕路比直线远很多时，只沿已探明边界向它试探，不把未知格当空地。
    std::vector<std::pair<Cell,int>> boundary; // 有未知邻格的可达格及其BFS距离；用实际视野判断，不固定9×9。
    boundary.reserve(order.size() / 4);
    for (const auto &cell : order) {
      const auto node = paths.at(cell);
      if (node.first < 0) continue;
      for (const auto &delta : steps) {
        const Cell next{cell.first + delta.first, cell.second + delta.second}; // next用于检查未知边界。
        if (next.first >= 0 && next.first < width && next.second >= 0 && next.second < height &&
            !tiles_.count(next)) { boundary.push_back({cell, node.distance}); break; }
      }
    }
    for (const auto &[cell, tile] : tiles_) {
      if (tile.value <= 0 || tile.wall) continue;
      const auto node = paths.find(cell);
      const int straight = manhattan(start, cell); // 直线只是下界估计，未知地形仍逐格试探。
      if (straight > remaining_ticks) continue; // 直线都来不及就不再向它探。
      if (node != paths.end() && node->second.distance <= 3 * straight + 6) continue; // 绕路不远时直接采集更划算。
      double benefit = tile.value;
      if (stage_ == 2) {
        benefit = rmtc::score_delta(resource_value, tile.value, 120, 30) + std::min(tile.value, 20.0) * 0.001;
        if ((tile.high && need_high) || (!tile.high && need_normal)) benefit += parameters_.resource_mission_bonus;
        if (need_value) benefit += tile.value * 0.1;
      }
      benefit *= 1.0 / (1.0 + std::max<Tick>(0, tick - tile.last_seen - parameters_.stale_grace_ticks) * parameters_.stale_resource_decay);
      int best_cost = std::numeric_limits<int>::max(), best_first = -1;
      for (const auto &[bcell, distance] : boundary) {
        const int cost = distance + manhattan(bcell, cell);
        if (cost < best_cost) { best_cost = cost; best_first = paths.at(bcell).first; }
      }
      if (best_first >= 0) {
        // 已知绕路没远到值得试探时，继续直接采集；试探只用于“绕路过远/未连通”的资源。
        if (node != paths.end() && best_cost * 2 >= node->second.distance) continue;
        consider(best_first, benefit * parameters_.explore_toward_resource_value / (best_cost + 1),
                 "explore_toward_resource", cell, benefit);
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
  if (stage_ == 1 && !reverse_unknown_) {
    // 第一阶段没有对手和事件：对全部已知资源做巡回排序，避免近邻顺序在死路分支间反复折返。
    std::vector<Cell> tour_targets;
    tour_targets.reserve(resource_options.size());
    for (const auto &option : resource_options) if (option.second != start) tour_targets.push_back(option.second);
    if (const auto tour = resource_tour(start, remaining_ticks, tour_targets, occupied))
      if (consider(tour->first, tour->utility, "collect_resource_tour", tour->first_resource, tour->benefit))
        direction_plan[tour->first]=tour->stops;
  }
  // 扫图候选：按BFS顺序评估能增加可见格的落点；不假设未知格里有什么。
  // boundary_only只评有未知邻格的边界格（第二阶段公平竞争用）；false保留“所有能新增视野的格子”的旧语义。
  const auto consider_frontier = [&](bool boundary_only) {
    const double max_visible = (2 * radius + 1) * (2 * radius + 1); // 视野半径内最多未知格数，仅作提前停止的上界。
    double frontier_best = -1; // BFS距离不降；远处候选的最大收益低于已有近处候选时提前结束。
    for (const Cell &cell : order) {
      const auto node = paths.at(cell); // 到观察点的真实绕障距离和首方向。
      if (node.first < 0 || node.distance >= remaining_ticks) continue;
      if (frontier_best >= 0 && parameters_.frontier_value * max_visible / (node.distance + 1) < frontier_best) break;
      if (boundary_only) {
        bool boundary = false;
        for (const auto &delta : steps) {
          const Cell next{cell.first + delta.first, cell.second + delta.second};
          if (next.first >= 0 && next.first < width && next.second >= 0 && next.second < height &&
              !tiles_.count(next)) { boundary = true; break; }
        }
        if (!boundary) continue;
      }
      int newly_visible = 0; // 用本帧实际视野半径计数，不固定写成9×9。
      for (int y = std::max(0, cell.second - radius); y <= std::min(height - 1, cell.second + radius); ++y) {
        for (int x = std::max(0, cell.first - radius); x <= std::min(width - 1, cell.first + radius); ++x) {
          const auto tile = tiles_.find({x, y}); // 只估计能多看多少格，未知内容仍然未知。
          newly_visible += tile == tiles_.end() || !tile->second.seen;
        }
      }
      if (newly_visible > 0) {
        const double utility = parameters_.frontier_value * newly_visible / (node.distance + 1);
        if (utility > frontier_best) frontier_best = utility;
        consider(node.first, utility, "explore_frontier", cell);
      }
    }
  };
  if (best_utility < 0 && remaining_ticks > 1) {
    // 没有资源/任务等更明确目标时，前往能增加可见范围的已知落点；不假设未知格里有什么。
    if (stage_ == 1 && !reverse_unknown_ && parameters_.explore_tour_value > 0) {
      // 近邻边界单格只是局部最优；把散布的边界观察点排成小巡回，系统扫过多个方向，减少同区反复。
      std::vector<std::pair<int,Cell>> waypoints; // 观察收益与有未知邻格的已知格。
      for (const Cell &cell : order) {
        const auto node = paths.at(cell);
        if (node.first < 0 || node.distance >= remaining_ticks) continue;
        bool boundary = false;
        for (const auto &delta : steps) {
          const Cell next{cell.first + delta.first, cell.second + delta.second};
          if (next.first >= 0 && next.first < width && next.second >= 0 && next.second < height &&
              !tiles_.count(next)) { boundary = true; break; }
        }
        if (!boundary) continue;
        int newly_visible = 0; // 用本帧实际视野半径计数，不固定写成9×9。
        for (int y = std::max(0, cell.second - radius); y <= std::min(height - 1, cell.second + radius); ++y)
          for (int x = std::max(0, cell.first - radius); x <= std::min(width - 1, cell.first + radius); ++x) {
            const auto tile = tiles_.find({x, y}); // 只估计能多看多少格，未知内容仍然未知。
            newly_visible += tile == tiles_.end() || !tile->second.seen;
          }
        if (newly_visible > 0) waypoints.push_back({newly_visible, cell});
      }
      std::sort(waypoints.begin(), waypoints.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
      // 上一帧巡回首停点仍在边界上就固定为本周起点，避免对称边界上首停点每帧来回翻转（原地抖动）。
      const Cell hint = (previous_goal && previous_goal->kind == "explore_tour") ? previous_goal->position : Cell{-1, -1};
      std::vector<Cell> picked; // 少而分散的观察点；只排序并执行首步，每帧重排。
      std::map<Cell,double> values;
      if (hint.first >= 0) {
        const auto found = std::find_if(waypoints.begin(), waypoints.end(),
            [&](const auto &entry) { return entry.second == hint; });
        if (found != waypoints.end()) { picked.push_back(hint); values[hint] = static_cast<double>(found->first); }
      }
      for (const auto &[gain, cell] : waypoints) {
        if (picked.size() >= 8) break;
        if (std::any_of(picked.begin(), picked.end(), [&](const Cell &other) { return manhattan(other, cell) <= 2; })) continue;
        picked.push_back(cell);
        values[cell] = static_cast<double>(gain);
      }
      if (picked.size() >= 2) {
        if (const auto tour = resource_tour(start, remaining_ticks, picked, occupied, &values, hint)) {
          consider(tour->first, parameters_.explore_tour_value * tour->utility, "explore_tour",
                   tour->first_resource, tour->benefit);
        }
      }
    }
    if (stage_ == 1) consider_frontier(false);
  }
  // 第二阶段到点任务没有截止时间、资源先到先得：扫近处边界必须与任务/交战公平竞争，不能只在空档期生效。
  if (stage_ == 2 && !reverse_unknown_ && remaining_ticks > 1) consider_frontier(true);
  if (best_utility < 0 && exploration >= 0) consider(exploration, 0.01, "explore");

  int attack_direction = -1;
  double attack_utility = -1;
  if (stage_ == 2 && !protected_spawn) {
    for (int direction = 0; direction < 4; ++direction) {
      std::vector<const Enemy *> targets;
      if (empowered_) {
        for (const auto &enemy : enemies)
          if (std::abs(enemy.position.first - start.first) <= 2 && std::abs(enemy.position.second - start.second) <= 2)
            targets.push_back(&enemy);
      } else {
        const Enemy *nearest = nullptr;
        for (const auto &enemy : enemies)
          if (ray(start, enemy.position) == direction && (!nearest ||
              manhattan(start, enemy.position) < manhattan(start, nearest->position))) nearest = &enemy;
        if (nearest) targets.push_back(nearest);  // 近处保护敌车仍阻挡后方，障碍不挡攻击。
      }
      double contribution = 0, reflected = 0;
      for (const auto *enemy : targets) {
        if (enemy->protected_spawn) continue;
        const bool predicted_defend = enemy->defending || enemy_will_defend(*enemy);
        if (reflection && predicted_defend) { reflected += reflection_amount; continue; }
        const double damage = predicted_defend ? defended_damage_ : attack_damage_; // 此目标预计受到的普通/防御伤害。
        // 按独自击杀估值；多人同时贡献时奖励会平分，所以只是机会估计。
        contribution += std::min(damage, enemy->hp) + (enemy->hp <= damage ? 60.0 : 0.0);
      }
      double utility = combat_value(contribution) + (need_damage && contribution > 0 ? 0.5 : 0.0);
      utility -= reflected * 0.05;
      // D已封顶且没有伤害任务时，不主动开战；本Tick已被火力波及时的还手不受限（不白挨打）。
      // 封顶只封官方原始分；同分平局按D决胜，故按 post_cap_combat_weight 留平局价值（0=旧行为：完全归零）。
      if (effective_damage >= 500 && !need_damage && incoming == 0)
        utility = parameters_.post_cap_combat_weight * contribution;
      // 派窗口刺杀：攻击正在扫资源簇的对手。击杀/击退令其重生移位，把本簇剩余资源留给我们——
      // 这是对lap先到先得结构的唯一信息内干预；D收益上面已计（封顶归零），此处按目标脚边未收集资源估值加归属收益。
      if (parameters_.stage2_assassin_value > 0 && tick - last_resource_sighting_ < 60) {
        double contested = 0;
        for (const auto *enemy : targets) {
          if (enemy->protected_spawn || enemy->hp > attack_damage_) continue; // 只补刀一击必杀目标，不为一击打不死的对手放弃近处采集。
          for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
              const auto tile = tiles_.find({enemy->position.first + dx, enemy->position.second + dy});
              if (tile != tiles_.end() && tile->second.seen && tile->second.value > 0) contested += tile->second.value;
            }
        }
        if (contested > 0) utility += parameters_.stage2_assassin_value * contested;
      }
      // 饼窗口攻击折扣：竞速期攻击会把我们从抢饼线路吸走（clusters-495：t20-26攻击致死、错过中簇70R）；
      // 还手（incoming>0）不折扣，不白挨打。与race gate不同：只压攻击估值，不动任务/接近等其他行为。
      if (parameters_.stage2_pie_attack_discount > 0 && tick - last_resource_sighting_ < 60 && incoming == 0)
        utility *= 1.0 - parameters_.stage2_pie_attack_discount;
      if (reflected + pre_damage >= hp) continue;
      if (utility > attack_utility) { attack_utility = utility; attack_direction = direction; }
    }
  }
  // 脱困只覆盖无益循环；当前致命防御/公告避险已在前面处理，有实际攻击收益时仍允许攻击。
  if(parameters_.loop_recovery && tick<recovery_until_ && !reverse_unknown_ && attack_utility<=0) {
    if(recovery_goal_ && *recovery_goal_==start) {recovery_goal_.reset();recovery_until_=-1;}
    else {
      const auto available=[&](Cell cell) {
        const auto path=paths.find(cell);
        return path!=paths.end() && path->second.first>=0 && path->second.distance<=recovery_until_-tick;
      };
      if(recovery_goal_ && !available(*recovery_goal_)) recovery_goal_.reset(); // 动态占位或危险改变就重选安全目标。
      if(!recovery_goal_) {
        std::optional<std::tuple<bool,double,bool,int,double,int>> best_escape;
        const std::string team=self.value("team",std::string("A"));
        const int seat=team.size()==1 && team[0]>='A' && team[0]<='D' ? team[0]-'A' : 0;
        for(const Cell &cell:order) {
          const auto path=paths.at(cell);const auto &tile=tiles_.at(cell);
          if(!available(cell) || loop_cells_.count(cell) || path.distance>6) continue;
          int unknown=0;
          for(int y=std::max(0,cell.second-radius);y<=std::min(height-1,cell.second+radius);++y)
            for(int x=std::max(0,cell.first-radius);x<=std::min(width-1,cell.first+radius);++x) {
              const auto at=tiles_.find({x,y});unknown+=at==tiles_.end() || !at->second.seen;
            }
          // 先争取新视野，再少访问格；相同条件按公开席位打破两车完全对称的选择。
          const auto value=std::make_tuple(unknown>0,unknown/((path.distance+1.0)*fire_cost[path.first]),tile.visits==0,
            -static_cast<int>(std::min(tile.visits,1000000U)),path.distance/fire_cost[path.first],-((path.first-seat+4)%4));
          if(!best_escape || value>*best_escape) {best_escape=value;recovery_goal_=cell;}
        }
      }
      if(recovery_goal_) {
        recovery_blocked_=false;goal_=Goal{*recovery_goal_,"unstuck",tick};
        return move(paths.at(*recovery_goal_).first,"unstuck_relocate");
      }
      recovery_blocked_=true;return fallback("stuck_no_safe_exit"); // 真没有安全出口就明确守住，不能靠随机撞墙制造“正在动”。
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
          occupied.count(next) || !safe_steps[i] || split_wall(next, tick) || dangerous(next, tick + 1)) continue;
      const auto tile = tiles_.find(next); // 邻格的已知地图记录，可能不存在。
      if (tile != tiles_.end() && (tile->second.seen || tile->second.wall)) continue;
      // 未知格试探也只排序假设风险，不把“可能受伤”误写成“没有路”。
      if (probe<0 || fire_damage[i]<fire_damage[probe] ||
          (fire_damage[i]==fire_damage[probe] && probe_attempts_[next]<fewest)) {
        fewest=probe_attempts_[next];probe=i;
      }
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

