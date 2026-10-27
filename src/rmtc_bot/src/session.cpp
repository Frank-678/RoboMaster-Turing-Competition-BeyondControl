#include "rmtc_bot/session.hpp"
#include "rmtc_bot/protocol.hpp"
#include <exception>

namespace rmtc {
Session::Session(int stage, AiConfig config)
    : stage_(stage), strategy_(stage), ai_(stage == 2 ? std::move(config) : AiConfig{}) {}

std::optional<Json> Session::act(const Json &p) {
  const auto tick = frame_tick(p);
  const auto session = p.at("session_id").get<std::string>();
  if (retired_.count(session)) return std::nullopt;
  if (session != session_) {
    if (!session_.empty()) retired_.insert(session_);
    session_ = session; latest_tick_ = -1; cached_ = Json();
    strategy_.reset(); ai_.reset(session);
  }
  if (tick < latest_tick_) return std::nullopt;
  if (tick == latest_tick_) return cached_.is_null() ? std::nullopt : std::optional<Json>(cached_);
  if (const auto result = ai_.poll()) {
    bool understood = false, partial = false;
    try {
      if (result->http_success && !result->mock_response && result->session == session_) {
        const auto proposal = decode_event_proposal(result->reply, session_, result->source,
                                          p.at("map").at("width").get<int>(),
                                          p.at("map").at("height").get<int>());
        strategy_.merge_events(proposal.events);
        partial = !proposal.unknown.empty();
        understood = !partial;
      }
    } catch (const std::exception &) { /* 模型异常降级，不能把模型输出当作动作。 */ }
    ai_.acknowledge(*result, understood, partial);
  }
  try { ai_.observe(p, strategy_.known_events()); }
  catch (const std::exception &) { /* 非法输入不阻塞本地决策。 */ }
  Json proposed = Json::object();  // 异常交给动作检查层：存活第二阶段回退DEFEND。
  try { proposed = strategy_.decide(p); reason_ = strategy_.reason(); }
  catch (const std::exception &) { reason_ = "invalid_strategy_input"; }
  // 策略明确返回null：已知会在动作前死亡/被强制等待，或第一阶段无合法动作。
  cached_ = proposed.is_null() ? Json() : action_for(p, proposed, stage_);
  latest_tick_ = tick;
  return cached_.is_null() ? std::nullopt : std::optional<Json>(cached_);
}
}  // namespace rmtc
