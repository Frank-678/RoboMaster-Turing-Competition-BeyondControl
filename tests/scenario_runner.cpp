// 精确场景入口：摆世界 → 可选前缀 → 内存断点分叉 → 跑到条件 → 保存回放包。
// 世界真值只在裁判/断言里；Strategy 和 NPC 始终只收到自己的 observe() 与按时交付的解释。
#include "arena.hpp"
#include "rmtc_bot/strategy.hpp"
#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
using namespace rmtc;
using namespace rmtc::arena;
using Json=nlohmann::json;
const std::array<std::string,4> seats={"A","B","C","D"};

int integer(const Json &j,int low,int high) {
  if(!j.is_number_integer() || j.get<double>()<low || j.get<double>()>high)
    throw std::runtime_error("scene integer out of range");
  return j.get<int>();
}
Cell position(const Json &j,int width,int height) {
  if(!j.is_array() || j.size()!=2) throw std::runtime_error("scene position requires [x,y]");
  return {integer(j[0],0,width-1),integer(j[1],0,height-1)};
}
bool matches(const Json &view,const Json &condition) {
  const auto path=Json::json_pointer(condition.at("path").get<std::string>());
  const auto op=condition.at("op").get<std::string>();
  if(op=="exists") return view.contains(path);
  if(!view.contains(path)) throw std::runtime_error("condition path does not exist: "+path.to_string());
  const auto &a=view.at(path),&b=condition.at("value");
  if(op=="eq") return a==b;
  if(op=="ne") return a!=b;
  if(!a.is_number() || !b.is_number()) throw std::runtime_error("ordered condition requires numbers");
  if(op=="lt") return a.get<double>()<b.get<double>();
  if(op=="le") return a.get<double>()<=b.get<double>();
  if(op=="gt") return a.get<double>()>b.get<double>();
  if(op=="ge") return a.get<double>()>=b.get<double>();
  throw std::runtime_error("unknown condition operator: "+op);
}

struct Delivery { Event event;int notice,tick; }; // tick 是解释送到策略的绝对Tick，不是调用耗时或事件开始。
Scenario make_scene(const Json &spec,std::vector<Delivery> &deliveries) {
  if(spec.at("schema")!=1) throw std::runtime_error("scene schema must be 1");
  Scenario s{};s.name=spec.at("name");s.map_style="fixture";s.seed=spec.value("seed",0U);
  if(s.name.empty()) throw std::runtime_error("scene name must not be empty");
  s.stage=integer(spec.at("stage"),1,2);s.ticks=integer(spec.at("tick_limit"),1,10000);
  const auto &map=spec.at("map");s.width=integer(map.at("width"),1,64);s.height=integer(map.at("height"),1,64);
  s.radius=integer(map.at("vision_radius"),0,4); // 当前arena的实体图固定9×9；不伪装支持更大的实体协议。
  s.simulated_ai_success=spec.at("qualified").get<bool>();s.interpretation_delay=0;
  for(const auto &j:map.at("walls")) if(!s.walls.insert(position(j,s.width,s.height)).second)
    throw std::runtime_error("duplicate scene wall");
  if(map.at("spawns").size()!=4) throw std::runtime_error("four spawns required");
  std::set<Cell> spawns;
  for(int i=0;i<4;++i) {
    s.spawns[i]=position(map.at("spawns")[i],s.width,s.height);
    if(s.walls.count(s.spawns[i]) || !spawns.insert(s.spawns[i]).second)
      throw std::runtime_error("spawn overlaps wall/other spawn");
  }
  for(const auto &j:spec.at("initial").at("resources")) {
    const Cell p=position(j.at("position"),s.width,s.height);
    if(s.walls.count(p) || !s.resources.emplace(p,integer(j.at("value"),1,1000000)).second)
      throw std::runtime_error("resource overlaps wall/duplicate resource");
  }
  s.missions=spec.at("missions");if(!s.missions.is_array()) throw std::runtime_error("missions must be an array");
  std::set<std::string> ids;
  for(const auto &a:spec.at("events")) {
    const auto id=a.at("id").get<std::string>();
    if(id.empty() || !ids.insert(id).second) throw std::runtime_error("event id empty/duplicate");
    const int notice=integer(a.at("notice_tick"),0,10000),delivered=integer(a.at("delivery_tick"),notice,1000000);
    const auto &j=a.at("event");
    // 复用正式事件契约检查字段；这里的JSON是明示的测试oracle，不进行真实HTTP请求。
    auto decoded=decode_events(Json{{"session_id",s.name},{"events",Json::array({j})},{"unknown",Json::array()}}.dump(),
                               s.name,j.at("source").get<std::string>(),s.width,s.height);
    auto event=decoded.at(0);event.key=id;
    s.announcements.push_back({event,notice});deliveries.push_back({event,notice,delivered});
  }
  return s;
}

Json legal_script_action(const Json &value) {
  if(value.is_null()) return nullptr; // 明确不提交，常用于第一阶段；不作为第二阶段的默认动作。
  const auto action=rules::decode_action(value);
  if(action.kind==rules::ActionKind::none) throw std::runtime_error("script action must be move/attack/defend or null");
  return rules::encode_action(action);
}
struct Agent {
  Json spec; // 手写对手的绝对Tick动作表，或strategy/NPC的声明；不含世界真值。
  std::optional<Strategy> strategy;
  std::optional<Opponent> npc;
  Agent(const Json &j,int stage,unsigned seed):spec(j) {
    const auto type=spec.at("type").get<std::string>();
    const auto delivery=spec.at("event_delivery").get<std::string>();
    if(delivery!="oracle" && delivery!="none") throw std::runtime_error("event_delivery must be oracle/none");
    if(type=="strategy") strategy.emplace(stage);
    else if(type=="npc") {
      const auto style=spec.at("style").get<std::string>();
      const std::set<std::string> styles={"collector","aggressive","defender","fortress","opportunist","wanderer","berserker","sentry","trickster"};
      if(!styles.count(style)) throw std::runtime_error("unknown NPC style");
      npc.emplace(style,seed,stage);
    } else if(type=="script") {
      legal_script_action(spec.at("otherwise"));
      for(const auto &entry:spec.at("actions").items()) {
        const int tick=std::stoi(entry.key());
        if(tick<0 || std::to_string(tick)!=entry.key()) throw std::runtime_error("script tick must be canonical nonnegative integer");
        legal_script_action(entry.value());
      }
    } else if(type!="external") throw std::runtime_error("agent type must be strategy/npc/script/external");
  }
  Json act(const Json &p,const std::vector<Event> &known,bool qualified) {
    if(spec.at("type")=="external") throw std::runtime_error("external agent needs league worker action");
    if(strategy) {
      strategy->merge_events(known);if(qualified) strategy->note_model_reply();
      return strategy->decide(p);
    }
    if(npc) return rules::encode_action(npc->act(p,known));
    if(!p.at("self").at("alive").get<bool>()) return nullptr;
    const auto tick=std::to_string(p.at("tick").get<int>());
    return legal_script_action(spec.at("actions").contains(tick) ? spec.at("actions").at(tick) : spec.at("otherwise"));
  }
  std::string reason() const { return strategy ? strategy->reason() : spec.at("type").get<std::string>(); }
};

struct Controller {
  Json spec,tape=Json::array(),decisions=Json::object(); // tape只保存每个agent自己的公开输入、交付事件ID和实际选择。
  std::vector<Delivery> deliveries;
  Game game;
  std::array<std::optional<Agent>,4> agents;
  std::string build_id;
  explicit Controller(const Json &j,std::string identity):spec(j),game(make_scene(j,deliveries)),build_id(std::move(identity)) {
    auto state=game.checkpoint();state["tick"]=spec.at("initial").at("tick");
    const auto &cars=spec.at("initial").at("cars");
    for(int i=0;i<4;++i) {
      const auto &seat=seats[i];
      if(cars.contains(seat)!=spec.at("agents").contains(seat)) throw std::runtime_error("each enabled car needs one agent");
      if(!cars.contains(seat)) { // 省略的席位是明示的测试停用车，不参与短片段；不是正式比赛复活规则。
        state["cars"][seat]["alive"]=false;state["cars"][seat]["hp"]=0;state["cars"][seat]["respawn_in"]=1000000000;
        continue;
      }
      if(spec.at("stage")==1 && i!=0) throw std::runtime_error("stage1 fixture only enables A");
      const auto &patch=cars.at(seat);
      patch.at("position");patch.at("hp"); // 人工摆局必须明确位置与血量，其余字段在模板/文档列明初值。
      for(const auto &entry:patch.items()) if(!state["cars"][seat].contains(entry.key()))
        throw std::runtime_error("unknown car field: "+entry.key());
      state["cars"][seat].merge_patch(patch);
      if(state.at("tick").get<int>()>0 && !patch.contains("position_reason")) state["cars"][seat]["position_reason"]="";
      agents[i].emplace(spec.at("agents").at(seat),game.scenario().stage,game.scenario().seed+101*i);
    }
    for(const auto &entry:cars.items()) if(std::find(seats.begin(),seats.end(),entry.key())==seats.end())
      throw std::runtime_error("unknown car seat");
    game.restore(state);
  }
  std::vector<Event> known(int seat,int tick) const {
    std::vector<Event> result;
    if(agents[seat]->spec.at("event_delivery")=="none") return result;
    for(const auto &a:deliveries) if(a.notice<=tick && a.tick<=tick) result.push_back(a.event);
    return result;
  }
  Json tick(const Json &external=Json::object()) {
    Json row={{"tick",game.tick()},{"perceptions",Json::object()},{"event_ids",Json::object()},{"actions",Json::object()}};
    std::array<rules::Action,4> actions{};decisions=Json::object();
    for(int i=0;i<4;++i) if(agents[i]) {
      const auto p=game.observe(i);const auto events=known(i,game.tick());
      const bool outside=agents[i]->spec.at("type")=="external";
      const auto action=outside ? legal_script_action(external.at(seats[i])) : agents[i]->act(p,events,game.scenario().simulated_ai_success);
      Json ids=Json::array();for(const auto &e:events) ids.push_back(e.key);
      row["perceptions"][seats[i]]=p;row["event_ids"][seats[i]]=ids;row["actions"][seats[i]]=action;
      decisions[seats[i]]={{"chosen",action},{"reason",agents[i]->reason()}};
      actions[i]=rules::decode_action(action);
    }
    game.step(actions);tape.push_back(row);return row;
  }
  Json checkpoint() const {
    return {{"schema",1},{"kind","rmtc_scenario_checkpoint"},{"build_id",build_id},{"scene",spec},
            {"world",game.checkpoint()},{"tape",tape},{"decisions",decisions}};
  }
  void resume(const Json &bundle) {
    if(bundle.at("kind")!="rmtc_scenario_checkpoint" || bundle.at("schema")!=1 || bundle.at("build_id")!=build_id)
      throw std::runtime_error("checkpoint schema/build mismatch; use original build or create a new cold fixture");
    int expected=game.tick();
    for(const auto &row:bundle.at("tape")) {
      if(row.at("tick")!=expected++) throw std::runtime_error("checkpoint tape has a gap");
      for(int i=0;i<4;++i) if(agents[i]) {
        std::vector<Event> events;
        for(const auto &id:row.at("event_ids").at(seats[i])) {
          const auto found=std::find_if(deliveries.begin(),deliveries.end(),[&](const Delivery &e){return e.event.key==id;});
          if(found==deliveries.end() || found->notice>row.at("tick").get<int>() || found->tick>row.at("tick").get<int>())
            throw std::runtime_error("checkpoint tape contains unannounced/not-yet-delivered event");
          events.push_back(found->event);
        }
        const auto &p=row.at("perceptions").at(seats[i]);
        if(p.at("tick")!=row.at("tick") || p.at("session_id")!=game.scenario().name)
          throw std::runtime_error("checkpoint perception tick/session mismatch");
        if(agents[i]->act(p,events,game.scenario().simulated_ai_success)!=row.at("actions").at(seats[i]))
          throw std::runtime_error("warm replay diverged: "+seats[i]+" tick "+row.at("tick").dump());
      }
    }
    if(bundle.at("world").at("tick")!=expected) throw std::runtime_error("checkpoint world/tape tick mismatch");
    game.restore(bundle.at("world"));tape=bundle.at("tape");decisions=bundle.at("decisions");
  }
  void fork(const Json &changes) {
    for(const auto &entry:changes.items()) {
      const auto it=std::find(seats.begin(),seats.end(),entry.key());
      if(it==seats.end() || !agents[std::distance(seats.begin(),it)]) throw std::runtime_error("fork references disabled seat");
      auto &agent=*agents[std::distance(seats.begin(),it)];
      auto updated=agent.spec;
      for(const auto &change:entry.value().items()) {
        if(change.key()=="event_delivery") updated[change.key()]=change.value();
        else if(change.key()=="actions" && agent.spec.at("type")=="script") {
          for(const auto &action:change.value().items()) {
            if(std::stoi(action.key())<game.tick()) throw std::runtime_error("fork cannot rewrite earlier actions");
            updated["actions"][action.key()]=action.value();
          }
        } else throw std::runtime_error("fork only changes future scripted actions or event delivery");
      }
      Agent checked(updated,game.scenario().stage,game.scenario().seed); // 只校验声明；绝不替换已经暖起来的策略/NPC对象。
      (void)checked;agent.spec=updated;spec["agents"][entry.key()]=updated;
    }
  }
};

Json run(Controller controller,const Json &control) {
  const int begin=controller.game.tick(),until=integer(control.at("until_tick"),begin,controller.game.scenario().ticks);
  Json trace=Json::array(),saved=Json::object(),failures=Json::array();std::string stop="until_tick";
  const auto view=[&] {return Json{{"world",controller.game.checkpoint()},{"decisions",controller.decisions},{"result",controller.game.result()}};};
  while(true) {
    const auto current=view();
    for(const auto &condition:control.value("always",Json::array())) if(!matches(current,condition)) failures.push_back(condition);
    if(!failures.empty()) {stop="assertion_failed";break;}
    if(control.contains("save_at") && std::find(control.at("save_at").begin(),control.at("save_at").end(),controller.game.tick())!=control.at("save_at").end())
      saved[std::to_string(controller.game.tick())]=controller.checkpoint();
    if(controller.game.tick()>=until) break;
    bool hit=false;
    for(const auto &condition:control.value("break_when",Json::array())) hit=hit || matches(current,condition);
    if(hit) {stop="breakpoint";break;}
    const auto row=controller.tick();
    trace.push_back({{"tick",row.at("tick")},{"decisions",controller.decisions},{"after",controller.game.checkpoint()}});
    for(const auto &condition:control.value("always",Json::array())) if(!matches(view(),condition)) failures.push_back(condition);
    if(!failures.empty()) {stop="assertion_failed";break;}
  }
  for(const auto &condition:control.value("expect",Json::array())) if(!matches(view(),condition)) failures.push_back(condition);
  auto result=view();result.update({{"start_tick",begin},{"end_tick",controller.game.tick()},
    {"steps",controller.game.tick()-begin},{"stop",stop},{"passed",failures.empty()},{"failed_conditions",failures},
    {"mode",controller.tape.size()==static_cast<std::size_t>(controller.game.tick()-begin) ? "cold" : "warm_prefix"},
    {"trace",trace},{"checkpoint",controller.checkpoint()},{"saved",saved}});
  return result;
}
} // namespace

// 联赛用逐Tick裁判服务。全图只回调度器，调度器只将inputs[席位]交给对应版本。
// 每行请求/响应都是完整JSON；外部版本缺动作直接报错，不偷偷换成WAIT或DEFEND。
int serve() {
  std::string line;
  if(!std::getline(std::cin,line)) throw std::runtime_error("engine needs initial scene");
  const auto first=Json::parse(line);
  Controller controller(first.at("scene"),first.at("build_id").get<std::string>());
  while(true) {
    Json inputs=Json::object();
    for(int i=0;i<4;++i) if(controller.agents[i] && controller.agents[i]->spec.at("type")=="external") {
      Json events=Json::array();
      for(const auto &event:controller.known(i,controller.game.tick()))
        events.push_back({{"key",event.key},{"event",event_summary(event)}});
      inputs[seats[i]]={{"perception",controller.game.observe(i)},{"events",events},
                       {"qualified",controller.game.scenario().simulated_ai_success}};
    }
    std::cout<<Json{{"world",controller.game.checkpoint()},{"result",controller.game.result()},
                   {"decisions",controller.decisions},{"inputs",inputs}}.dump()<<std::endl;
    if(!std::getline(std::cin,line)) break;
    const auto request=Json::parse(line);
    if(request.at("op")=="quit") break;
    if(request.at("op")!="step" || controller.game.tick()>=controller.game.scenario().ticks)
      throw std::runtime_error("engine expects step before final tick");
    controller.tick(request.at("actions"));
  }
  return 0;
}

int main(int argc,char **argv) {
  try {
    if(argc==2 && std::string(argv[1])=="--serve") return serve();
    if(argc!=1) throw std::runtime_error("unknown scenario runner argument");
    const auto input=Json::parse(std::cin);const auto &spec=input.contains("resume") ? input.at("resume").at("scene") : input.at("scene");
    Controller controller(spec,input.at("build_id").get<std::string>());
    if(input.contains("resume")) controller.resume(input.at("resume"));
    const auto control=input.contains("run") ? input.at("run") : spec.at("run");
    const int warm_until=integer(control.value("warm_until",Json(controller.game.tick())),controller.game.tick(),controller.game.scenario().ticks);
    if(warm_until>integer(control.at("until_tick"),controller.game.tick(),controller.game.scenario().ticks))
      throw std::runtime_error("warm_until must not exceed until_tick");
    while(controller.game.tick()<warm_until) controller.tick(); // 前缀只跑一次，后续fork复制整个Game/Strategy/NPC及其随机数状态。
    Json output={{"schema",1},{"scope","offline fixture; no HTTP/ROS qualification"},{"build_id",input.at("build_id")},{"runs",Json::array()}};
    if(control.contains("forks")) {
      std::set<std::string> names;
      for(const auto &branch:control.at("forks")) {
        const auto name=branch.at("name").get<std::string>();
        if(name.empty() || !names.insert(name).second) throw std::runtime_error("fork name empty/duplicate");
        auto copy=controller;copy.fork(branch.value("agents",Json::object()));
        auto commands=control;commands.erase("forks");commands.erase("warm_until");commands.merge_patch(branch.value("run",Json::object()));
        auto result=run(std::move(copy),commands);result["name"]=name;output["runs"].push_back(std::move(result));
      }
      if(output["runs"].empty()) throw std::runtime_error("forks must not be empty");
    } else {auto result=run(std::move(controller),control);result["name"]="main";output["runs"].push_back(std::move(result));}
    const bool passed=std::all_of(output["runs"].begin(),output["runs"].end(),[](const Json &r){return r.at("passed").get<bool>();});
    output["passed"]=passed;std::cout<<output.dump()<<'\n';return passed ? 0 : 1;
  } catch(const std::exception &error) {std::cerr<<"scenario error: "<<error.what()<<'\n';return 2;}
}
