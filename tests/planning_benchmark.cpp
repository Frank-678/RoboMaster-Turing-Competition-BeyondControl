// 采集策略的配对夹具：同一地图/视野/席位/步数，分别链接旧、新Strategy。
// 只模拟无敌车、无任务、无事件的采集；不代替原版裁判、AI资格或标准车胜率。
#include "rmtc_bot/strategy.hpp"
#include "rmtc_bot/scoring.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json; // 公开感知与结果输出。
using Cell = std::pair<int, int>; // 测试端真实格子[x,y]。
struct Fixture {
  std::string name; int stage, width, height, radius, first_tick, moves;
  Cell start; double initial_r; // 初始坐标、初始累计R。
  std::map<Cell, int> resources; std::set<Cell> walls; // 只有测试端拥有全图；选手只收到局部感知。
};
Json entity(const char *type, Cell offset, int value = 0) {
  std::string grid; // 协议中的单实体9×9相对位置图。
  for (int y = 0; y < 9; ++y) {
    if (y) grid += '\n';
    for (int x = 0; x < 9; ++x)
      grid += x == 4 && y == 4 ? '1' : x == offset.first + 4 && y == offset.second + 4 ? '2' : '0';
  }
  return {{"type", type}, {"position", grid}, {"value", value}};
}
Json run(const Fixture &f) {
  Strategy strategy(f.stage); // 被测的生产策略，夹具不向它传递任何隐藏地图。
  Cell current = f.start;
  auto resources = f.resources;
  std::set<Cell> seen; // 测试端统计实际覆盖格数，不反馈成选手的额外观测。
  std::vector<double> durations; // 仅Strategy::decide耗时，不含ROS、序列化、网络或LLM。
  double added_r = 0;
  int blocked_moves = 0, used_ticks = 0; // 普通受阻MOVE不等同于裁判invalid_actions。
  Json receipt = f.first_tick == 0 ? Json() : Json{{"tick",f.first_tick - 1}, {"action","wait"}, {"moved",false}};
  Json trace = Json::array(); // 保留坐标、意图和实际执行，便于核对没有凭指令加坐标。
  const auto observe = [&] {
    for (int y = std::max(0,current.second-f.radius); y <= std::min(f.height-1,current.second+f.radius); ++y)
      for (int x = std::max(0,current.first-f.radius); x <= std::min(f.width-1,current.first+f.radius); ++x)
        seen.insert({x,y});
  };
  for (int n = 0; n < f.moves; ++n) {
    const int tick = f.first_tick + n;
    observe();
    Json p = {{"session_id",f.name}, {"tick",tick}, {"vision_radius",f.radius},
      {"map",{{"width",f.width},{"height",f.height}}},
      {"self",{{"team","A"},{"position",{current.first,current.second}},{"alive",true},
               {"hp",100},{"protected",false},{"last_action",receipt}}},
      {"visible_entities",Json::array()}, {"missions",Json::array()}, {"event_text",""},
      {"score",{{"resource_value",f.initial_r+added_r},{"effective_damage",0},{"mission_completed_ids",Json::array()}}}};
    if (tick == 0) p["self"]["position_reason"] = "initial";
    for (const auto &[cell,value] : resources) {
      const Cell offset{cell.first-current.first,cell.second-current.second};
      if (std::abs(offset.first)<=f.radius && std::abs(offset.second)<=f.radius)
        p["visible_entities"].push_back(entity(value>=20 ? "resource_high" : "resource_normal",offset,value));
    }
    for (const Cell &cell : f.walls) {
      const Cell offset{cell.first-current.first,cell.second-current.second};
      if (std::abs(offset.first)<=f.radius && std::abs(offset.second)<=f.radius)
        p["visible_entities"].push_back(entity("obstacle",offset));
    }
    const auto before = std::chrono::steady_clock::now();
    const Json action = strategy.decide(p);
    durations.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count());
    Cell next = current; bool moved = false;
    const std::string type = action.is_null() ? "none" : action.at("action").get<std::string>();
    if (type == "move") {
      const std::string direction = action.at("direction");
      if (direction=="up") --next.second;
      else if (direction=="right") ++next.first;
      else if (direction=="down") ++next.second;
      else if (direction=="left") --next.first;
      else throw std::runtime_error("invalid fixture direction");
      moved = next.first>=0 && next.first<f.width && next.second>=0 && next.second<f.height && !f.walls.count(next);
      if (moved) current = next; else ++blocked_moves;
    } else if (type!="none" && type!="defend" && type!="wait") throw std::runtime_error("unexpected non-collection action");
    const auto resource = resources.find(current);
    if (resource!=resources.end()) { added_r+=resource->second; resources.erase(resource); }
    receipt = {{"tick",tick},{"action",type},{"moved",moved}};
    trace.push_back({{"tick",tick},{"action",action},{"reason",strategy.reason()},
                     {"position",{current.first,current.second}},{"moved",moved},{"added_R",added_r}});
    used_ticks = n+1;
    if (!f.resources.empty() && resources.empty()) break;
  }
  observe(); // 最后一个MOVE后的下一帧会看到什么；不再调用策略或虚构额外拾取。
  std::sort(durations.begin(),durations.end());
  const std::size_t p95 = (durations.size()*95+99)/100-1; // 夹具内样本分位，不代表真实比赛延迟分布。
  return {{"name",f.name},{"stage",f.stage},{"first_tick",f.first_tick},{"tick_budget",f.moves},
          {"initial_R",f.initial_r},{"added_R",added_r},{"stage2_resource_score_delta",f.stage==2 ? Json(rmtc::score_delta(f.initial_r,added_r,120,30)) : Json()},
          {"ticks_used",used_ticks},{"remaining_resources",resources.size()},{"seen_cells",seen.size()},
          {"blocked_moves",blocked_moves},{"decide_p95_ms",durations.at(p95)},
          {"decide_max_ms",durations.back()},{"trace",trace}};
}
} // namespace
int main(int argc, char **argv) {
  if (argc!=2) return 2; // 标签由配对测试脚本明确传入，和正式机器人配置无关。
  const std::map<Cell,int> chain = {{{4,3},20},{{5,4},10},{{6,4},20}};
  std::vector<Fixture> fixtures = {
    {"late_chain",2,9,9,4,498,2,{4,4},0,chain,{}},
    {"capped_chain",2,9,9,4,498,2,{4,4},110,chain,{}},
    {"last_move",2,9,9,4,499,1,{4,4},0,chain,{}},
    {"four_move_chain",1,9,9,4,0,4,{4,4},0,chain,{}},
    {"maze",1,6,6,4,0,80,{0,0},0,{{{4,0},10},{{5,5},10},{{0,5},10}},{{2,0},{2,1},{2,2},{2,3}}},
    {"frontier_edge",1,9,9,1,0,1,{4,1},0,{},{}},
    {"dense_resources",1,20,20,4,0,100,{10,10},0,{},{}},
  };
  for (int y : {7,9,11,13}) for (int x : {7,9,11,13})
    fixtures.back().resources[{x,y}] = x==7 ? 20 : 10; // 12普通+4高价值，总R=200。
  Json cases = Json::array();
  for (const auto &fixture : fixtures) cases.push_back(run(fixture));
  std::cout << Json{{"revision",argv[1]}, {"scope","synthetic collection only; no official judge, opponents, events, missions or AI"},
                   {"cases",cases}}.dump(2) << '\n';
}
