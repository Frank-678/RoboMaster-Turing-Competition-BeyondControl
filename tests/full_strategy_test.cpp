#include "rmtc_bot/session.hpp"
#include "rmtc_bot/protocol.hpp"
#include "rmtc_bot/scoring.hpp"
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
Json fresh(int tick = 0, int x = 4, int y = 4) {
  auto p = frame(x, y, 9, 9, 4);
  p["tick"] = tick;
  p["self"]["hp"] = 100;
  p["self"]["team"] = "A";
  p["score"] = {{"resource_value", 0}, {"effective_damage", 0}, {"mission_completed_ids", Json::array()}};
  p["missions"] = Json::array();
  p["event_text"] = "";
  if (tick > 0) p["self"]["last_action"] = {{"tick", tick - 1}, {"action", "wait"}, {"moved", false}};
  return p;
}
Json robot(int dx, int dy, double hp = 100, bool protected_spawn = false, bool defending = false) {
  auto e = entity("robot", dx, dy);
  e["hp"] = hp; e["protected"] = protected_spawn; e["defending"] = defending;
  return e;
}
Json choose(const Json &p, int stage = 2) {
  rmtc::Session session(stage);
  return *session.act(p);
}
Json specification(const char *kind, int start, int end) {
  return {{"kind", kind}, {"start", start}, {"end", end}, {"source", "fixture"}};
}
std::vector<rmtc::Event> events(const Json &spec) {
  return rmtc::decode_events(Json{{"session_id", "test-match"}, {"events", Json::array({spec})}}.dump(),
                             "test-match", "synthetic fixture announcement", 9, 9);
}
void rejected(const Json &root, const std::string &source = "fixture") {
  bool failed = false;
  try { rmtc::decode_events(root.dump(), "test-match", source, 9, 9); }
  catch (const std::exception &) { failed = true; }
  check(failed, "invalid model event was accepted");
}
void combat_and_missions() {
  auto p = fresh();
  p["visible_entities"] = {robot(1, 0)};
  auto action = choose(p);
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
void capped_score_decisions() {
  check(std::abs(rmtc::score_delta(119, 20, 120, 30) - 0.25) < 1e-9, "resource cap crossing arithmetic");
  check(rmtc::score_delta(120, 20, 120, 30) == 0, "resource cap arithmetic");
  check(std::abs(rmtc::score_delta(490.5, 20, 500, 15) - 0.285) < 1e-9, "fractional damage cap crossing");
  check(std::abs(rmtc::score_delta(520, -60, 500, 15) + 1.2) < 1e-9, "death score loss at 520 D");
  check(rmtc::score_delta(600, -60, 500, 15) == 0, "excess damage buffer was truncated before death");
  auto p = fresh();
  p["visible_entities"] = {entity("resource_normal", 0, -1), robot(1, 0)};
  p["score"]["resource_value"] = 119;
  check(choose(p)["action"] == "attack", "resource crossing 120 was credited its full uncapped value");
  rmtc::Session session(2);
  p["score"]["effective_damage"] = 600.5;
  auto action = *session.act(p);
  check(action["action"] == "move" && action["direction"] == "up", "combat beyond 500 retained uncapped direct reward");
  p["tick"] = 1;
  p["score"]["effective_damage"] = 40.5;
  action = *session.act(p);
  check(action["action"] == "attack", "decreased official damage did not restore combat value");
}
void event_validation() {
  auto j = specification("explosion", 5, 6);
  j["area"] = {2, 2, 4, 4}; j["amount"] = 80;
  check(events(j).size() == 1, "valid event rejected");
  Json root = {{"session_id", "test-match"}, {"events", {j}}};
  auto bad = root; bad["session_id"] = "old-match"; rejected(bad);
  bad = root; bad["events"][0]["source"] = "invented quote"; rejected(bad);
  bad = root; bad["events"][0]["area"] = {0, 0, 9, 8}; rejected(bad);
  bad = root; bad["events"][0]["start"] = 1.5; rejected(bad);
  bad = root; bad["events"][0]["end"] = 5; rejected(bad);
  bad = root; bad["events"][0]["kind"] = "do_anything"; rejected(bad);
  bad = root; bad["events"][0]["end"] = 8; rejected(bad);
  auto second = j; second["area"] = {5, 5, 7, 7};
  root["events"].push_back(second);
  const auto decoded = rmtc::decode_events(root.dump(), "test-match", "fixture", 9, 9);
  check(decoded.size() == 2 && decoded[0].key != decoded[1].key, "same-tick different events collapsed");
}
void hazard_and_illusion() {
  auto explosion = specification("explosion", 3, 4);
  explosion["area"] = {3, 3, 5, 5}; explosion["amount"] = 80;
  Strategy s;
  s.merge_events(events(explosion));
  auto p = fresh(1);
  check(s.decide(p)["action"] == "move" && s.reason() == "escape_announced_hazard", "explosion not escaped before activation");
  p = fresh(3); p["self"]["hp"] = 70;
  check(s.decide(p)["action"] == "wait" && s.reason() == "lethal_activation_before_action", "late MOVE survived pre-move explosion");
  p["self"]["protected"] = true;
  check(s.decide(p)["action"] == "move", "explosion ignored spawn protection");

  auto split = specification("split", 2, 8); split["points"] = {{5, 5}};
  Strategy wall;
  wall.merge_events(events(split));
  p = fresh(1, 5, 4);
  const auto escape = wall.decide(p);
  check(escape["action"] == "move" && (escape["direction"] == "right" || escape["direction"] == "left"), "split cross not escaped");
  p = fresh(2, 5, 4);
  check(wall.decide(p)["action"] == "wait", "split activation did not force WAIT");

  auto illusion = specification("illusion", 0, 5); illusion["area"] = {3, 3, 5, 5};
  Strategy reversed;
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
  Strategy uncertain;
  uncertain.merge_events(events(illusion));
  p = fresh(2); p["visible_entities"] = {entity("resource_normal", 1, 0)};
  check(uncertain.decide(p)["action"] == "wait", "late join guessed unobserved illusion eligibility");
}
void fortress_and_effects() {
  auto fortress = specification("fortress", 0, 20);
  fortress["area"] = {2, 2, 3, 3}; fortress["capture_ticks"] = 2;
  fortress["amount"] = 50; fortress["defended_damage"] = 10;
  Strategy s; s.merge_events(events(fortress));
  auto p = fresh(0, 3, 3);
  check(s.decide(p)["action"] == "wait", "offset fortress ignored or hardcoded");
  p = fresh(1, 3, 3); p["visible_entities"] = {robot(1, 1, 30)};
  check(s.decide(p)["action"] != "attack", "fortress upgrade applied before battle-after capture");
  p["tick"] = 2; p["self"]["last_action"]["tick"] = 1;
  check(s.decide(p)["action"] == "attack", "fortress did not upgrade diagonal area attack");
  p = fresh(3, 3, 3); p["self"]["alive"] = false;
  check(s.decide(p)["action"] == "wait", "dead fortress holder acted");
  p = fresh(4, 7, 7); p["self"]["position_reason"] = "respawn";
  p["visible_entities"] = {robot(-1, -1, 30)};
  check(s.decide(p)["action"] == "attack", "fortress buff lost on exit/death");
  p["tick"] = 20; p["self"]["last_action"]["tick"] = 19;
  check(s.decide(p)["action"] != "attack", "expired fortress buff retained");
  fortress["amount"] = 20;
  Strategy same_damage; same_damage.merge_events(events(fortress));
  for (int t = 0; t <= 2; ++t) {
    p = fresh(t, 3, 3);
    if (t == 2) p["visible_entities"] = {robot(1, 1, 10)};
    const auto result = same_damage.decide(p);
    if (t == 2) check(result["action"] == "attack", "custom fortress damage disabled area geometry");
  }
  Strategy interrupted; interrupted.merge_events(events(fortress));
  interrupted.decide(fresh(0, 3, 3)); interrupted.decide(fresh(1, 3, 3));
  p = fresh(2, 3, 3); p["self"]["alive"] = false; interrupted.decide(p);
  p = fresh(3, 3, 3); p["self"]["position_reason"] = "respawn";
  p["visible_entities"] = {robot(1, 1, 10)};
  check(interrupted.decide(p)["action"] != "attack", "unfinished fortress capture survived death");

  auto vulnerable = specification("vulnerability", 0, 5);
  vulnerable["trigger_end"] = 2; vulnerable["multiplier"] = 2;
  Strategy v; v.merge_events(events(vulnerable));
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

  auto reflect = specification("reflect", 0, 10); reflect["amount"] = 20;
  Strategy r; r.merge_events(events(reflect));
  p = fresh(); p["visible_entities"] = {robot(1, 0, 100, false, true)};
  check(r.decide(p)["action"] != "attack", "reflection into a defending target ignored");

  auto heal = specification("heal", 0, 10);
  heal["points"] = {{5, 4}}; heal["amount"] = 10; heal["effect_ticks"] = 3;
  Strategy h; h.merge_events(events(heal));
  p = fresh(); p["self"]["hp"] = 30;
  check(h.decide(p)["direction"] == "right" && h.reason() == "seek_healing", "healing point not pursued at low HP");

  auto chili = specification("chili", 0, 10); chili["points"] = {{5, 4}}; chili["amount"] = 50;
  Strategy c; c.merge_events(events(chili));
  p = fresh(); p["visible_entities"] = {robot(1, 3, 20)};
  check(c.decide(p)["direction"] == "right" && c.reason() == "trigger_chili", "chili entry opportunity ignored");
  p = fresh(1, 5, 3); p["self"]["hp"] = 30;
  p["visible_entities"] = {robot(1, 1)};
  const auto dodge = c.decide(p);
  check(dodge["action"] == "move" && c.reason() == "avoid_possible_chili_fire", "post-move chili risk ignored");
  p = fresh(2, 5, 4); p["self"]["hp"] = 30;
  p["visible_entities"] = {robot(1, 0)};
  c.decide(p);
  check(c.reason() != "avoid_possible_chili_fire", "chili intersection incorrectly treated as damaged");
}
void session_contract() {
  rmtc::Session s(2);
  auto p = fresh(10); p["visible_entities"] = {entity("resource_normal", 1, 0)};
  const auto first = s.act(p);
  p["visible_entities"] = {entity("resource_normal", 0, -1)};
  check(s.act(p) == first, "same tick advanced strategy or overwrote paused action");
  p["tick"] = 9; check(!s.act(p), "stale tick produced an action");
  p["tick"] = 11; check(s.act(p).has_value(), "fresh tick rejected");
  p["session_id"] = "new-session"; p["tick"] = 0;
  check(s.act(p)->at("session_id") == "new-session", "new session was not adopted");
  p["session_id"] = "test-match"; p["tick"] = 100;
  check(!s.act(p), "late old-session perception reset new match");
  p = fresh(); p["session_id"] = "third-session"; p["self"]["position"] = {3.5, 4};
  check(s.act(p)->at("action") == "wait", "bad strategy input did not fall back to WAIT");
}
}  // namespace

void full_scenarios() {
  combat_and_missions(); capped_score_decisions(); event_validation(); hazard_and_illusion();
  fortress_and_effects(); session_contract();
  std::cout << "combat, tasks, eight event kinds, and session contracts passed\n";
}

void gateway_scenarios(const std::string &base) {
  using namespace std::chrono;
  const auto config = [&](const std::string &path, long timeout = 1800) {
    return rmtc::AiConfig{base + path, "fixture-token", "fixture-model", timeout, 2200};
  };
  rmtc::Session session(2, config("/ok"));
  auto p = fresh();
  p["event_text"] = "fixture: fortress area [6,6,7,7], tick 0 to 1000, capture 2.";
  bool applied = false;
  for (int t = 0; t < 50; ++t) {
    p["tick"] = t;
    const auto begin = steady_clock::now();
    session.act(p);
    check(duration_cast<milliseconds>(steady_clock::now() - begin).count() < 100, "AI blocked action loop");
    if (session.ai_http_success() && session.reason() == "capture_fortress") { applied = true; break; }
    std::this_thread::sleep_for(milliseconds(20));
  }
  check(applied, "mock gateway event never reached strategy");
  // 一个成功的空公告请求也必须真实发往网关，不在本地伪造成功。
  rmtc::AiWorker quota(config("/quota"));
  quota.reset("quota");
  auto q = fresh(); q["session_id"] = "quota";
  quota.observe(q, Json::array());
  bool stopped = false;
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
  rmtc::AiWorker rate(config("/rate"));
  rate.reset("rate"); q["session_id"] = "rate"; rate.observe(q, Json::array());
  auto limited = receive(rate);
  check(!limited.http_success && !limited.stop && limited.status == "http_429", "rate limit confused with quota");
  rate.acknowledge(limited, false);
  rate.observe(q, Json::array());
  check(!rate.poll(), "rate limit ignored backoff");
  std::this_thread::sleep_for(milliseconds(1050));
  rate.observe(q, Json::array());
  const auto retried = receive(rate);
  check(retried.http_success, "transient rate limit never retried");
  rate.acknowledge(retried, true);
  check(rate.observed_http_success(), "successful forwarded HTTP reply not recorded");
  for (const std::string path : {"/auth", "/garbage", "/bad-response"}) {
    rmtc::AiWorker failure(config(path));
    failure.reset(path); q["session_id"] = path; failure.observe(q, Json::array());
    const auto result = receive(failure);
    if (path == "/auth") check(result.stop && !result.http_success, "401 did not disable credential retries");
    if (path == "/garbage") check(!result.stop && !result.http_success, "non-JSON 503 killed fallback");
    if (path == "/bad-response") check(result.http_success && result.reply.empty(), "HTTP success confused with parsed model output");
    failure.acknowledge(result, false);
  }

  rmtc::AiWorker late(config("/delayed"));
  late.reset("old"); q["session_id"] = "old"; late.observe(q, Json::array());
  std::this_thread::sleep_for(milliseconds(20));
  late.reset("new"); q["session_id"] = "new";
  bool adopted_new = false;
  for (int i = 0; i < 100; ++i) {
    late.observe(q, Json::array());
    if (const auto result = late.poll()) {
      check(result->session == "new" && result->http_success, "late old-session HTTP result was adopted");
      late.acknowledge(*result, true); adopted_new = true; break;
    }
    std::this_thread::sleep_for(milliseconds(10));
  }
  check(adopted_new, "new-session HTTP request starved after cancellation");

  rmtc::Session slow(2, config("/slow", 100));
  p = fresh(); slow.act(p);
  p["tick"] = 1; p["session_id"] = "next-match";
  const auto begin = steady_clock::now();
  slow.act(p);
  check(duration_cast<milliseconds>(steady_clock::now() - begin).count() < 100, "session change waited for old HTTP job");
  bool new_success = false;
  for (int t = 2; t < 60; ++t) {
    p["tick"] = t; slow.act(p);
    if (slow.ai_http_success()) { new_success = true; break; }
    std::this_thread::sleep_for(milliseconds(10));
  }
  // /slow 新旧场次都故意超过超时，不能出现伪造的成功标志。
  check(!new_success, "timed-out or old-session response qualified the new match");
  rmtc::Session stage1(1, config("/stage1-must-not-call"));
  stage1.act(fresh());
  std::cout << "mock HTTP: async event pipeline, quota, timeout, session cancellation, stage1 isolation passed\n";
}
