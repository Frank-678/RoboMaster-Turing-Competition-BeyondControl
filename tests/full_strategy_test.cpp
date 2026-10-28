// 高阶与HTTP回归；所有事件/敌车/响应都是测试夹具，不是正式模型效果证明。
#include "rmtc_bot/session.hpp"
#include "rmtc_bot/protocol.hpp"
#include "rmtc_bot/scoring.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

using Json = nlohmann::json;
Json frame(int, int, int, int, int);
Json entity(const char *, int, int);
void check(bool, const char *);

namespace {
// 第二阶段测试感知：tick逻辑回合，x/y本车位置，补齐任务/成绩/回执字段。
Json fresh(int tick = 0, int x = 4, int y = 4) {
  auto p = frame(x, y, 9, 9, 4); // 当前场景的公开感知JSON。
  p["tick"] = tick;
  p["self"]["hp"] = 100;
  p["self"]["team"] = "A";
  p["score"] = {{"resource_value", 0}, {"effective_damage", 0}, {"mission_completed_ids", Json::array()}};
  p["missions"] = Json::array();
  p["event_text"] = "";
  if (tick > 0) p["self"]["last_action"] = {{"tick", tick - 1}, {"action", "wait"}, {"moved", false}};
  return p;
}
// 生成可见敌车；dx/dy相对位置，hp血量，protected_spawn/defending为最近状态。
Json robot(int dx, int dy, double hp = 100, bool protected_spawn = false, bool defending = false) {
  auto e = entity("robot", dx, dy); // 待加入感知的敌车实体。
  e["hp"] = hp; e["protected"] = protected_spawn; e["defending"] = defending;
  return e;
}
// 用独立Session处理一帧p，返回完整动作；stage仅决定测试阶段。
Json choose(const Json &p, int stage = 2) {
  rmtc::Session session(stage); // 被测场次对象，保留跨Tick状态。
  return *session.act(p);
}
// 构造模型事件最小字段：kind类型、start生效、end结束。
Json specification(const char *kind, int start, int end) {
  return {{"kind", kind}, {"start", start}, {"end", end}, {"source", "fixture"}};
}
// 把测试事件spec包成模型回复，再经过正式校验器；不直接绕过校验塞进策略。
std::vector<rmtc::Event> events(const Json &spec) {
  return rmtc::decode_events(Json{{"session_id", "test-match"}, {"events", Json::array({spec})}, {"unknown", Json::array()}}.dump(),
                             "test-match", "synthetic fixture announcement", 9, 9);
}
// 检查root提案必须因schema/原文证据source错误而被拒绝。
void rejected(const Json &root, const std::string &source = "fixture") {
  bool failed = false; // 是否捕获到预期的校验异常。
  try { rmtc::decode_events(root.dump(), "test-match", source, 9, 9); }
  catch (const std::exception &) { failed = true; }
  check(failed, "invalid model event was accepted");
}
// 验证先战斗后移动、最近目标、保护、墙不挡攻击和任务完成去重。
void combat_and_missions() {
  auto p = fresh(); // 当前场景的公开感知JSON。
  p["visible_entities"] = {robot(1, 0)};
  auto action = choose(p); // 当前候选动作结果。
  check(action["action"] == "attack" && action["direction"] == "right", "ordinary attack opportunity missed");
  check(choose(p, 1)["action"] == "move", "stage1 emitted combat action");
  p["self"]["hp"] = 15;
  check(choose(p)["action"] == "defend", "MOVE was treated as an escape from current combat");
  p["self"]["hp"] = 100; p["self"]["protected"] = true;
  check(choose(p)["action"] != "attack", "spawn-protected self attacked");
  p["self"]["protected"] = false;
  p["visible_entities"] = {robot(1, 0, 100, true), robot(2, 0, 10)};
  check(choose(p)["action"] != "attack", "ordinary attack pierced a protected nearest robot");
  p["visible_entities"] = {entity("obstacle", 1, 0), robot(2, 0)};
  check(choose(p)["action"] == "attack", "obstacle incorrectly blocked an attack");
  p = fresh();
  p["missions"] = {{{"id", "reach"}, {"type", "reach"}, {"position", {6, 4}}, {"target", 1}}};
  check(choose(p)["direction"] == "right", "structured reach mission ignored");
  p["score"]["mission_completed_ids"] = {"reach"};
  check(choose(p)["direction"] == "up", "completed reach mission was pursued again");
  p["visible_entities"] = {entity("resource_normal", 0, -1), entity("resource_high", 2, 0)};
  p["missions"] = {{{"id", "high"}, {"type", "high_resources"}, {"target", 1}}};
  check(choose(p)["direction"] == "right", "unfinished high-resource task did not affect target choice");
  p["score"]["resource_value"] = 130;
  p["missions"] = {{{"id", "reach2"}, {"type", "reach"}, {"position", {4, 5}}, {"target", 1}}};
  check(choose(p)["direction"] == "down", "resource cap prevented reach task selection");
}
// 验证封顶跨线、小数D、死亡扣除及官方D下降后的重新决策。
void capped_score_decisions() {
  check(std::abs(rmtc::score_delta(119, 20, 120, 30) - 0.25) < 1e-9, "resource cap crossing arithmetic");
  check(rmtc::score_delta(120, 20, 120, 30) == 0, "resource cap arithmetic");
  check(std::abs(rmtc::score_delta(490.5, 20, 500, 15) - 0.285) < 1e-9, "fractional damage cap crossing");
  check(std::abs(rmtc::score_delta(520, -60, 500, 15) + 1.2) < 1e-9, "death score loss at 520 D");
  check(rmtc::score_delta(600, -60, 500, 15) == 0, "excess damage buffer was truncated before death");
  auto p = fresh(); // 当前场景的公开感知JSON。
  p["visible_entities"] = {entity("resource_normal", 0, -1), robot(1, 0)};
  p["score"]["resource_value"] = 119;
  check(choose(p)["action"] == "attack", "resource crossing 120 was credited its full uncapped value");
  rmtc::Session session(2); // 被测场次对象，保留跨Tick状态。
  p["score"]["effective_damage"] = 600.5;
  auto action = *session.act(p); // 当前候选动作结果。
  check(action["action"] == "move" && action["direction"] == "up", "combat beyond 500 retained uncapped direct reward");
  p["tick"] = 1;
  p["score"]["effective_damage"] = 40.5;
  action = *session.act(p);
  check(action["action"] == "attack", "decreased official damage did not restore combat value");
}
// 验证DEFEND回退、第一阶段/死亡不乱发动作、失败试探换方向及Tick去重。
void defend_fallback() {
  auto p = fresh(); // 当前场景的公开感知JSON。
  p["visible_entities"] = {entity("obstacle", 0, -1), entity("obstacle", 1, 0),
                           entity("obstacle", 0, 1), entity("obstacle", -1, 0)};
  check(choose(p)["action"] == "defend", "surrounded stage2 robot fell back to WAIT");
  check(rmtc::action_for(p, Json{{"action", "move"}, {"direction", "invalid"}}, 2)["action"] == "defend",
        "invalid proposal did not fall back to DEFEND");
  check(rmtc::action_for(p, Json::object(), 1).is_null(), "stage1 fallback illegally defended");
  p["self"]["alive"] = false;
  check(rmtc::action_for(p, Json::object(), 2).is_null(), "dead robot emitted a fallback action");
  rmtc::Session probe(2); // 没有周边观测时的试探移动场次。
  p = fresh(); p["vision_radius"] = 0;
  const auto first = probe.act(p); // 首次处理该Tick的动作，供重复处理对照。
  check(first && first->at("direction") == "up", "unknown neighbor was never probed");
  check(probe.act(p) == first, "duplicate tick consumed a probe attempt");
  p["tick"] = 1;
  p["self"]["last_action"] = {{"tick", 0}, {"action", "move"}, {"moved", false}};
  const auto next = probe.act(p); // 失败试探后的下一次动作。
  check(next && next->at("direction") == "right", "failed probe repeated instead of trying another neighbor");
}
// 验证事件字段、原文证据、unknown和同Tick不同区域事件。
void event_validation() {
  auto j = specification("explosion", 5, 6); // 当前待构造/修改的事件JSON。
  j["area"] = {2, 2, 4, 4}; j["amount"] = 80;
  check(events(j).size() == 1, "valid event rejected");
  Json root = {{"session_id", "test-match"}, {"events", {j}}, {"unknown", Json::array()}}; // 完整模型提案，包含场次/events/unknown。
  auto partial = root; partial["unknown"] = {"activation tick missing for another event"}; // 含未知项的提案或对应被测场次。
  const auto proposal = rmtc::decode_event_proposal(partial.dump(), "test-match", "fixture", 9, 9); // 保留已知事件和未知说明的校验结果。
  check(proposal.events.size() == 1 && proposal.unknown.size() == 1, "partial event proposal lost uncertainty");
  auto malformed = root; malformed.erase("unknown"); rejected(malformed); // 故意删字段/改类型的坏提案。
  malformed = root; malformed["unknown"] = "none"; rejected(malformed);
  malformed = root; malformed["unknown"] = {nullptr}; rejected(malformed);
  auto bad = root; bad["session_id"] = "old-match"; rejected(bad); // 故意改变场次、证据或范围的坏提案。
  bad = root; bad["events"][0]["source"] = "invented quote"; rejected(bad);
  bad = root; bad["events"][0]["area"] = {0, 0, 9, 8}; rejected(bad);
  bad = root; bad["events"][0]["start"] = 1.5; rejected(bad);
  bad = root; bad["events"][0]["end"] = 5; rejected(bad);
  bad = root; bad["events"][0]["kind"] = "do_anything"; rejected(bad);
  bad = root; bad["events"][0]["end"] = 8; rejected(bad);
  auto second = j; second["area"] = {5, 5, 7, 7}; // 同Tick另一不同区域的事件。
  root["events"].push_back(second);
  const auto decoded = rmtc::decode_events(root.dump(), "test-match", "fixture", 9, 9); // 正式校验器返回的事件列表。
  check(decoded.size() == 2 && decoded[0].key != decoded[1].key, "same-tick different events collapsed");
}
// HelloCV中出现过的输出包装：接受完整对象，仍拒绝截断、错场次、多对象和缺字段。
void model_formatting() {
  auto spec = specification("explosion", 5, 6); // 待校验的同一事件，包装变化不能改变它。
  spec["area"] = {2, 2, 4, 4}; spec["amount"] = 80;
  spec["source"] = "quote } ,] \\\" stays"; // 字符串里的括号、逗号和转义必须原样保留。
  const Json root = {{"session_id", "test-match"}, {"events", {spec}}, {"unknown", Json::array()}}; // 完整事件提案。
  const auto plain = root.dump(); // 标准序列化，作为包装回复的共同内容。
  auto comma = plain; comma.insert(comma.size() - 1, ","); // 只在人为添加的结尾处制造尾逗号。
  for (const auto &text : {plain, std::string("```json\n") + plain + "\n```",
                          std::string("解释如下：\n") + comma + "\n以上为结果。"}) {
    const auto decoded = rmtc::decode_events(text, "test-match", spec["source"], 9, 9); // 使用正式解析和证据检查。
    check(decoded.size() == 1 && decoded[0].source == spec["source"], "format cleanup changed or rejected valid evidence");
  }
  for (const auto &text : {plain.substr(0, plain.size() - 1), plain + plain, "[" + plain + "]",
                          std::string("```json\n{\"events\":[]}\n```"),
                          std::string("{\"session_id\":\"old-match\",\"events\":[],\"unknown\":[],}")}) {
    bool failed = false; // 所有这些回复必须在采用事件前被拒绝。
    try { rmtc::decode_events(text, "test-match", spec["source"], 9, 9); }
    catch (const std::exception &) { failed = true; }
    check(failed, "format cleanup invented or adopted an invalid event proposal");
  }
}
// 验证爆炸/分裂先于移动，以及幻术的方向、激活资格和到期。
void hazard_and_illusion() {
  auto explosion = specification("explosion", 3, 4); // 本场景公告的爆炸事件。
  explosion["area"] = {3, 3, 5, 5}; explosion["amount"] = 80;
  Strategy s; // 本场景主被测策略/场次。
  s.merge_events(events(explosion));
  auto p = fresh(1); // 当前场景的公开感知JSON。
  check(s.decide(p)["action"] == "move" && s.reason() == "escape_announced_hazard", "explosion not escaped before activation");
  p = fresh(3); p["self"]["hp"] = 70;
  check(s.decide(p).is_null() && s.reason() == "lethal_activation_before_action", "late MOVE survived pre-move explosion");
  p["self"]["protected"] = true;
  check(s.decide(p)["action"] == "move", "explosion ignored spawn protection");

  auto split = specification("split", 2, 8); split["points"] = {{5, 5}}; // 本场景公告的分裂事件。
  Strategy wall; // 被测分裂墙策略。
  wall.merge_events(events(split));
  p = fresh(1, 5, 4);
  const auto escape = wall.decide(p); // 生效前的逃离动作。
  check(escape["action"] == "move" && (escape["direction"] == "right" || escape["direction"] == "left"), "split cross not escaped");
  p = fresh(2, 5, 4);
  check(wall.decide(p).is_null(), "split activation did not force WAIT");

  auto illusion = specification("illusion", 0, 5); illusion["area"] = {3, 3, 5, 5}; // 本场景公告的幻术事件。
  Strategy reversed; // 已确认中幻术的策略。
  reversed.merge_events(events(illusion));
  p = fresh(); p["visible_entities"] = {entity("resource_normal", 1, 0)};
  check(reversed.decide(p)["direction"] == "left", "MOVE direction was not compensated");
  p["visible_entities"] = {robot(1, 0)};
  check(reversed.decide(p)["direction"] == "right", "ATTACK direction was incorrectly inverted");
  p = fresh(2, 7, 7); p["self"]["position_reason"] = "respawn";
  p["visible_entities"] = {entity("resource_normal", 1, 0)};
  check(reversed.decide(p)["direction"] == "left", "illusion ended on respawn or region exit");
  p["tick"] = 5;
  check(reversed.decide(p)["direction"] == "right", "illusion remained after absolute expiry");
  Strategy uncertain; // 缺少幻术激活历史的策略。
  uncertain.merge_events(events(illusion));
  p = fresh(2); p["visible_entities"] = {entity("resource_normal", 1, 0)};
  check(uncertain.decide(p)["action"] == "defend", "late join guessed unobserved illusion eligibility");
}
// 验证未来会封死的出口不能走，以及爆炸结算后可以进入该格。
void future_escape_route() {
  Strategy strategy; // 被测逃生策略，保留自己的地图和效果历史。
  strategy.decide(fresh(0, 1, 1));  // Initial spawn is elsewhere; [4,4] is not exempt.
  auto split = specification("split", 12, 16); split["points"] = {{0, 4}}; // 本场景公告的分裂事件。
  auto explosion = specification("explosion", 13, 14); // 本场景公告的爆炸事件。
  explosion["area"] = {4, 3, 4, 3}; explosion["amount"] = 80;
  strategy.merge_events(events(split)); strategy.merge_events(events(explosion));
  auto p = fresh(10); p["self"]["hp"] = 50; // 当前场景的公开感知JSON。
  p["visible_entities"] = {entity("resource_normal", 0, -1), entity("obstacle", -1, -1),
                           entity("obstacle", 1, -1), entity("obstacle", 0, -2)};
  const auto action = strategy.decide(p); // 当前候选动作结果。
  check(action["action"] == "move" && action["direction"] == "down",
        "nearest escape entered a dead end sealed before the next explosion");

  Strategy corridor; // 走廊中的被测策略，验证原地防御等爆炸。
  auto blast = specification("explosion", 11, 12); // 走廊前方下一Tick的爆炸。
  blast["area"] = {4, 3, 4, 3}; blast["amount"] = 80;
  corridor.merge_events(events(blast));
  auto later_blast = specification("explosion", 15, 16); // 后续爆炸，让时空过滤在首次爆炸后仍保持启用。
  later_blast["area"] = {8, 8, 8, 8}; later_blast["amount"] = 80;
  corridor.merge_events(events(later_blast));  // Keep the temporal filter active after the first blast.
  p = fresh(10);
  p["visible_entities"] = {entity("resource_high", 0, -2), entity("obstacle", -1, 0),
                           entity("obstacle", 1, 0), entity("obstacle", 0, 1)};
  check(corridor.decide(p)["action"] == "defend", "route did not wait for next-tick explosion");
  p["tick"] = 11;
  const auto after_blast = corridor.decide(p); // 爆炸已结算后的进入动作。
  check(after_blast["action"] == "move" && after_blast["direction"] == "up",
        "movement into a cell after its explosion was incorrectly prohibited");
}
// 验证堡垒驻留、跨死亡保留、回血、反伤、辣椒和易伤时序。
void fortress_and_effects() {
  auto fortress = specification("fortress", 0, 20); // 非固定中心范围的堡垒事件。
  fortress["area"] = {2, 2, 3, 3}; fortress["capture_ticks"] = 2;
  fortress["amount"] = 50; fortress["defended_damage"] = 10;
  Strategy s; s.merge_events(events(fortress)); // 本场景主被测策略/场次。
  auto p = fresh(0, 3, 3); // 当前场景的公开感知JSON。
  check(s.decide(p)["action"] == "defend", "offset fortress ignored or hardcoded");
  p = fresh(1, 3, 3); p["visible_entities"] = {robot(1, 1, 30)};
  check(s.decide(p)["action"] != "attack", "fortress upgrade applied before battle-after capture");
  p["tick"] = 2; p["self"]["last_action"]["tick"] = 1;
  check(s.decide(p)["action"] == "attack", "fortress did not upgrade diagonal area attack");
  p = fresh(3, 3, 3); p["self"]["alive"] = false;
  check(s.decide(p).is_null(), "dead fortress holder acted");
  p = fresh(4, 7, 7); p["self"]["position_reason"] = "respawn";
  p["visible_entities"] = {robot(-1, -1, 30)};
  check(s.decide(p)["action"] == "attack", "fortress buff lost on exit/death");
  p["tick"] = 20; p["self"]["last_action"]["tick"] = 19;
  check(s.decide(p)["action"] != "attack", "expired fortress buff retained");
  fortress["amount"] = 20;
  Strategy same_damage; same_damage.merge_events(events(fortress)); // 不同动作回执但相似血量的策略对照。
  for (int t = 0; t <= 2; ++t) {
    p = fresh(t, 3, 3);
    if (t == 2) p["visible_entities"] = {robot(1, 1, 10)};
    const auto result = same_damage.decide(p);
    if (t == 2) check(result["action"] == "attack", "custom fortress damage disabled area geometry");
  }
  Strategy interrupted; interrupted.merge_events(events(fortress)); // 驻留被死亡打断的堡垒策略。
  interrupted.decide(fresh(0, 3, 3)); interrupted.decide(fresh(1, 3, 3));
  p = fresh(2, 3, 3); p["self"]["alive"] = false; interrupted.decide(p);
  p = fresh(3, 3, 3); p["self"]["position_reason"] = "respawn";
  p["visible_entities"] = {robot(1, 1, 10)};
  check(interrupted.decide(p)["action"] != "attack", "unfinished fortress capture survived death");

  auto vulnerable = specification("vulnerability", 0, 5); // 移动触发的易伤事件。
  vulnerable["trigger_end"] = 2; vulnerable["multiplier"] = 2;
  Strategy v; v.merge_events(events(vulnerable)); // 易伤事件策略。
  p = fresh(); p["self"]["hp"] = 30; p["visible_entities"] = {robot(1, 0)};
  check(v.decide(p)["action"] == "attack", "vulnerability applied before movement");
  p = fresh(1, 5, 4); p["self"]["hp"] = 30; p["visible_entities"] = {robot(1, 0)};
  p["self"]["last_action"] = {{"tick", 0}, {"action", "move"}, {"moved", true}};
  check(v.decide(p)["action"] == "defend", "successful move did not trigger next-tick vulnerability");
  p = fresh(3, 5, 4); p["self"]["hp"] = 30; p["visible_entities"] = {robot(1, 0)};
  p["self"]["position_reason"] = "respawn";
  check(v.decide(p)["action"] == "defend", "vulnerability lost on respawn");
  p["tick"] = 5; p["self"]["last_action"]["tick"] = 4;
  check(v.decide(p)["action"] == "attack", "vulnerability expiry extended by respawn");

  auto reflect = specification("reflect", 0, 10); reflect["amount"] = 20; // 攻击防御者会反伤的事件。
  Strategy r; r.merge_events(events(reflect)); // 反伤事件策略。
  p = fresh(); p["visible_entities"] = {robot(1, 0, 100, false, true)};
  check(r.decide(p)["action"] != "attack", "reflection into a defending target ignored");

  auto heal = specification("heal", 0, 10); // 成功入点才触发的治疗事件。
  heal["points"] = {{5, 4}}; heal["amount"] = 10; heal["effect_ticks"] = 3;
  Strategy h; h.merge_events(events(heal)); // 治疗入点策略。
  p = fresh(); p["self"]["hp"] = 30;
  check(h.decide(p)["direction"] == "right" && h.reason() == "seek_healing", "healing point not pursued at low HP");

  auto chili = specification("chili", 0, 10); chili["points"] = {{5, 4}}; chili["amount"] = 50; // 成功入点产生火线的辣椒事件。
  Strategy c; c.merge_events(events(chili)); // 辣椒事件策略。
  p = fresh(); p["visible_entities"] = {robot(1, 3, 20)};
  check(c.decide(p)["direction"] == "right" && c.reason() == "trigger_chili", "chili entry opportunity ignored");
  p = fresh(1, 5, 3); p["self"]["hp"] = 30;
  p["visible_entities"] = {robot(1, 1)};
  const auto dodge = c.decide(p); // 针对可能辣椒火线的避险动作。
  check(dodge["action"] == "move" && c.reason() == "avoid_possible_chili_fire", "post-move chili risk ignored");
  p = fresh(2, 5, 4); p["self"]["hp"] = 30;
  p["visible_entities"] = {robot(1, 0)};
  c.decide(p);
  check(c.reason() != "avoid_possible_chili_fire", "chili intersection incorrectly treated as damaged");
}
// 验证新Tick、同Tick、旧Tick、换场和坏输入回退。
void session_contract() {
  rmtc::Session s(2); // 本场景主被测策略/场次。
  auto p = fresh(10); p["visible_entities"] = {entity("resource_normal", 1, 0)}; // 当前场景的公开感知JSON。
  const auto first = s.act(p); // 首次处理该Tick的动作，供重复处理对照。
  p["visible_entities"] = {entity("resource_normal", 0, -1)};
  check(s.act(p) == first, "same tick advanced strategy or overwrote paused action");
  p["tick"] = 9; check(!s.act(p), "stale tick produced an action");
  p["tick"] = 11; check(s.act(p).has_value(), "fresh tick rejected");
  p["session_id"] = "new-session"; p["tick"] = 0;
  check(s.act(p)->at("session_id") == "new-session", "new session was not adopted");
  p["session_id"] = "test-match"; p["tick"] = 100;
  check(!s.act(p), "late old-session perception reset new match");
  p = fresh(); p["session_id"] = "third-session"; p["self"]["position"] = {3.5, 4};
  check(s.act(p)->at("action") == "defend", "bad strategy input did not fall back to DEFEND");
}
}  // namespace

// 汇总运行全部第二阶段核心场景；不需要网络或ROS。
void full_scenarios() {
  combat_and_missions(); capped_score_decisions(); defend_fallback(); event_validation(); model_formatting(); hazard_and_illusion();
  future_escape_route(); fortress_and_effects(); session_contract();
  std::cout << "combat, tasks, eight event kinds, and session contracts passed\n";
}

// base为本机HTTP夹具地址；验证真实客户端代码的并发、退避、取消和错误分类。
void gateway_scenarios(const std::string &base) {
  using namespace std::chrono;
  const auto config = [&](const std::string &path, long timeout = 1800) {
    return rmtc::AiConfig{base + path, "fixture-token", "fixture-model", timeout, 2200, "", std::min(5000L, timeout), "disabled", true};
  };
  rmtc::AiWorker missing({}); // 没有URL/令牌的客户端，用于检查零请求诊断。
  missing.reset("missing-config");
  check(missing.status() == "disabled_missing_url_and_token" && missing.diagnostics()["started"] == 0,
        "zero requests did not show missing AI configuration");
  auto no_model = config("/ok"); no_model.model.clear(); // 故意删除模型名的完整客户端配置。
  bool rejected_model = false; // 空模型名是否已在请求前被拒绝。
  try { rmtc::AiWorker incomplete(no_model); }
  catch (const std::exception &) { rejected_model = true; }
  check(rejected_model, "empty model name silently used a gateway default");
  rmtc::Session mock(2, config("/practice-mock")); // 返回practice-mock标记的测试场次。
  auto mocked = fresh(); mocked["event_text"] = "fixture announcement"; // 带公告的模拟响应测试感知。
  bool detected_mock = false; // 是否识别出明确的固定模拟响应。
  for (int t = 0; t < 50; ++t) {
    mocked["tick"] = t; mock.act(mocked);
    if (mock.ai_status() == "mock_gateway") {
      check(mock.ai_http_success() && mock.ai_diagnostics()["http_successes"] == 1,
            "mock HTTP completion was not recorded as transport success");
      check(mock.reason() != "capture_fortress", "known mock events were adopted as model understanding");
      detected_mock = true; break;
    }
    std::this_thread::sleep_for(milliseconds(10));
  }
  check(detected_mock, "practice mock was confused with a real model response");
  for (int t = 50; t < 60; ++t) { mocked["tick"] = t; mock.act(mocked); }
  rmtc::Session session(2, config("/ok")); // 被测场次对象，保留跨Tick状态。
  auto p = fresh(); // 当前场景的公开感知JSON。
  p["event_text"] = "fixture: fortress area [6,6,7,7], tick 0 to 1000, capture 2.";
  bool applied = false; // 模型事件是否进入真实策略路径。
  for (int t = 0; t < 50; ++t) {
    p["tick"] = t;
    const auto begin = steady_clock::now(); // 当前操作开始的单调时钟时刻，用于检查动作回调耗时。
    session.act(p);
    check(duration_cast<milliseconds>(steady_clock::now() - begin).count() < 100, "AI blocked action loop");
    if (session.ai_http_success() && session.reason() == "capture_fortress") { applied = true; break; }
    std::this_thread::sleep_for(milliseconds(20));
  }
  check(applied, "mock gateway event never reached strategy");
  rmtc::Session fenced(2, config("/fenced")); // 经HTTP收到围栏JSON时，也须进入正式校验及真实策略。
  bool adopted_fenced = false; // 防止只单测解析器而实际HTTP链路仍拒绝包装输出。
  for (int t = 0; t < 50; ++t) {
    p["tick"] = t; fenced.act(p);
    if (fenced.ai_status() == "events_validated" && fenced.reason() == "capture_fortress") {
      adopted_fenced = true; break;
    }
    std::this_thread::sleep_for(milliseconds(10));
  }
  check(adopted_fenced, "fenced HTTP model output never reached strategy");
  rmtc::Session partial(2, config("/partial")); // 含未知项的提案或对应被测场次。
  bool saw_incomplete = false, resolved = false; // 是否观察到unknown尚未补齐的状态。
  for (int t = 0; t < 120; ++t) {
    p["tick"] = t; partial.act(p);
    if (partial.ai_http_success() && !saw_incomplete) {
      check(partial.ai_status() == "events_incomplete", "unknown fields were marked fully understood");
      check(partial.reason() == "capture_fortress", "validated part of model reply was discarded");
      saw_incomplete = true;
    }
    if (saw_incomplete && partial.ai_status() == "events_validated") { resolved = true; break; }
    std::this_thread::sleep_for(milliseconds(20));
  }
  check(saw_incomplete && resolved, "unknown fields were never retried and resolved");
  // 一个成功的空公告请求也必须真实发往网关，不在本地伪造成功。
  rmtc::AiWorker quota(config("/quota")); // 模拟配额耗尽的工作线程。
  quota.reset("quota");
  auto q = fresh(); q["session_id"] = "quota"; // 直接测试AiWorker时使用的公开感知。
  quota.observe(q, Json::array());
  bool stopped = false; // 配额耗尽后是否已停止重试。
  for (int i = 0; i < 50; ++i) {
    if (const auto r = quota.poll()) {
      check(r->stop && r->status == "quota_exceeded" && !r->http_success, "quota error classification failed");
      quota.acknowledge(*r, false); stopped = true; break;
    }
    std::this_thread::sleep_for(milliseconds(10));
  }
  check(stopped, "quota response missing");
  for (int i = 0; i < 20; ++i) quota.observe(q, Json::array());
  check(!quota.poll() && !quota.observed_http_success(), "quota exhaustion fabricated AI success");

  const auto receive = [](rmtc::AiWorker &worker) {
    for (int i = 0; i < 200; ++i) {
      if (auto result = worker.poll()) return *result;
      std::this_thread::sleep_for(milliseconds(10));
    }
    throw std::runtime_error("mock gateway result missing");
  };
  // 感知可重复携带整段近期公告。逐渐增长的100批输入不能被拼成100份历史。
  rmtc::AiWorker cumulative(config("/cumulative")); // 使用正式工作线程和HTTP请求体。
  cumulative.reset("cumulative"); q["session_id"] = "cumulative";
  std::string history; // 裁判在这一帧携带的累积原文，每条只应保留一次。
  for (int i = 0; i < 100; ++i) {
    if (!history.empty()) history += '\n';
    history += "fixture notice-" + std::to_string(i) + " " + std::string(200, 'x');
    q["event_text"] = history;
    cumulative.observe(q, Json::array()); // 在途请求期间，新公告也应继续保存。
  }
  const auto first_batch = receive(cumulative); // 第一批可能已完成，但主线程此时才取走。
  cumulative.acknowledge(first_batch, true);
  cumulative.observe(q, Json::array());
  const auto latest_batch = receive(cumulative); // 再次请求必须携带最新完整公告。
  check(latest_batch.source == history, "cumulative announcements duplicated or lost original text");
  cumulative.acknowledge(latest_batch, true);
  q["event_text"] = ""; // 后续限流/错误测试不继承这个专用场景的公告。
  rmtc::AiWorker rate(config("/rate")); // 模拟暂时限流的工作线程。
  rate.reset("rate"); q["session_id"] = "rate"; rate.observe(q, Json::array());
  auto limited = receive(rate); // 第一次429限流结果。
  check(!limited.http_success && !limited.stop && limited.status == "http_429", "rate limit confused with quota");
  rate.acknowledge(limited, false);
  rate.observe(q, Json::array());
  check(!rate.poll(), "rate limit ignored backoff");
  std::this_thread::sleep_for(milliseconds(1050));
  rate.observe(q, Json::array());
  const auto retried = receive(rate); // 退避后再次请求的结果。
  check(retried.http_success, "transient rate limit never retried");
  rate.acknowledge(retried, true);
  check(rate.observed_http_success(), "successful forwarded HTTP reply not recorded");
  auto proxied_config = config("/unused"); // 明确指定转发代理的客户端配置。
  proxied_config.url = "http://upstream.invalid/proxy-target";
  proxied_config.proxy = base;
  rmtc::AiWorker proxied(proxied_config); // 必须通过本机代理访问保留域名的测试客户端。
  proxied.reset("proxy"); q["session_id"] = "proxy"; proxied.observe(q, Json::array());
  check(receive(proxied).http_success, "explicit proxy was not used for outbound request");
  auto standard_config = config("/standard-only"); // 不支持服务商扩展的网关，只发基础协议字段。
  standard_config.thinking.clear(); standard_config.json_output = false;
  rmtc::AiWorker standard(standard_config); // 由夹具确认扩展字段确实省略，不由客户端猜测模型能力。
  standard.reset("standard"); q["session_id"] = "standard"; standard.observe(q, Json::array());
  check(receive(standard).http_success, "standard gateway options were not configurable");
  for (const auto &path : {"/reasoning-only", "/truncated"}) {
    rmtc::AiWorker incomplete(config(path)); // 最终答案不可用，但HTTP传输成功的两个场景。
    incomplete.reset(path); q["session_id"] = path; incomplete.observe(q, Json::array());
    const auto result = receive(incomplete); // 必须记录精确问题，不能读取reasoning当答案或采用截断对象。
    const std::string expected = std::string(path) == "/reasoning-only" ? "model_reasoning_only" : "model_output_truncated";
    check(result.http_success && result.reply.empty() && result.status == expected, "nonfinal model answer was adopted or misdiagnosed");
    incomplete.acknowledge(result, false);
    check(incomplete.status() == expected, "acknowledge erased model failure reason");
  }
  for (const std::string path : {"/auth", "/garbage", "/bad-response"}) {
    rmtc::AiWorker failure(config(path));
    failure.reset(path); q["session_id"] = path; failure.observe(q, Json::array());
    const auto result = receive(failure);
    if (path == "/auth") check(result.stop && !result.http_success, "401 did not disable credential retries");
    if (path == "/garbage") check(!result.stop && !result.http_success, "non-JSON 503 killed fallback");
    if (path == "/bad-response") check(result.http_success && result.reply.empty(), "HTTP success confused with parsed model output");
    failure.acknowledge(result, false);
  }

  rmtc::AiWorker late(config("/delayed")); // 故意延迟返回的旧场请求客户端。
  late.reset("old"); q["session_id"] = "old"; late.observe(q, Json::array());
  std::this_thread::sleep_for(milliseconds(20));
  late.reset("new"); q["session_id"] = "new";
  bool adopted_new = false; // 换场后是否只采纳新场结果。
  for (int i = 0; i < 100; ++i) {
    late.observe(q, Json::array());
    if (const auto result = late.poll()) {
      check(result->session == "new" && result->http_success, "late old-session HTTP result was adopted");
      late.acknowledge(*result, true); adopted_new = true; break;
    }
    std::this_thread::sleep_for(milliseconds(10));
  }
  check(adopted_new, "new-session HTTP request starved after cancellation");

  rmtc::Session slow(2, config("/slow", 100)); // 故意超过100ms超时的测试场次。
  p = fresh(); slow.act(p);
  p["tick"] = 1; p["session_id"] = "next-match";
  const auto begin = steady_clock::now(); // 当前操作开始的单调时钟时刻，用于检查动作回调耗时。
  slow.act(p);
  check(duration_cast<milliseconds>(steady_clock::now() - begin).count() < 100, "session change waited for old HTTP job");
  bool new_success = false; // 超时/换场过程中是否出现不该有的成功标记。
  for (int t = 2; t < 60; ++t) {
    p["tick"] = t; slow.act(p);
    if (slow.ai_http_success()) { new_success = true; break; }
    std::this_thread::sleep_for(milliseconds(10));
  }
  // /slow 新旧场次都故意超过超时，不能出现伪造的成功标志。
  check(!new_success, "timed-out or old-session response qualified the new match");
  check(slow.ai_diagnostics()["last_curl"] == 28 && slow.ai_status() == "timeout",
        "request timeout had no transport diagnostic");
  rmtc::Session stage1(1, config("/stage1-must-not-call")); // 配置了网关但第一阶段必须零请求的场次。
  stage1.act(fresh());
  std::cout << "mock HTTP: async event pipeline, quota, timeout, session cancellation, stage1 isolation passed\n";
}

// endpoint是原版practice处理器监听的本机地址；上游仍是固定夹具，不声称真实LLM/ROS通过。
void practice_gateway_scenario(const std::string &endpoint) {
  rmtc::Session session(2, {endpoint, "practice-A", "fixture-model", 1800, 2200, "", 1000, "disabled", true});
  auto p = fresh(); // 真实Session收到的公开感知；不读取原裁判配置或观赛数据。
  p["event_text"] = "fixture fortress announcement";
  for (int tick = 0; tick < 100; ++tick) {
    p["tick"] = tick;
    session.act(p);
    if (session.ai_status() == "events_validated" && session.reason() == "capture_fortress") {
      check(session.ai_http_success(), "original relay event was adopted without HTTP completion");
      std::cout << "C++ Session -> original practice HTTP -> canned upstream -> validated event passed\n";
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("original practice forwarding did not reach the strategy: " + session.ai_status());
}

