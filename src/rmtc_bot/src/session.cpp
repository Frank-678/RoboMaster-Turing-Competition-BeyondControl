#include "rmtc_bot/session.hpp"
#include "rmtc_bot/protocol.hpp"
#include <exception>

// 感知入口：换场/去重 → 接纳AI结果 → 排新请求 → 本地决策 → 动作检查和缓存。
namespace rmtc {
Session::Session(int stage, AiConfig config)
    : stage_(stage), strategy_(stage), ai_(stage == 2 ? std::move(config) : AiConfig{}) {}

Json Session::event_diagnostics() const {
  auto result=strategy_.event_diagnostics(latest_tick_); // 当前效果在本帧决策时由真实历史重建。
  result.update({{"replies",event_flow_.replies},{"validated",event_flow_.validated},{"rejected",event_flow_.rejected},
    {"partial",event_flow_.partial},{"imported",event_flow_.imported},{"after_start",event_flow_.after_start},
    {"arrived_expired",event_flow_.expired},{"request_tick",event_flow_.requested},{"received_tick",event_flow_.received},
    {"last_error",event_flow_.error},{"last_events",event_flow_.last_events}});
  return result;
}

std::optional<Json> Session::act(const Json &p) {
  const auto tick = frame_tick(p); // 先检查当前帧身份；信封不可信时不构造动作。
  const auto session = p.at("session_id").get<std::string>(); // 此帧所属场次。
  if (retired_.count(session)) return std::nullopt;
  if (session != session_) {
    // 新场清记忆。旧场ID留在retired_，防止迟到消息再次触发“换场”。
    if (!session_.empty()) retired_.insert(session_);
    session_ = session; latest_tick_ = -1; cached_ = Json();
    strategy_.reset(); ai_.reset(session); event_flow_=EventFlow{};
  }
  if (tick < latest_tick_) return std::nullopt;
  // 同一Tick只重发缓存，不重复访问格子、累计驻留或排模型请求。
  if (tick == latest_tick_) return cached_.is_null() ? std::nullopt : std::optional<Json>(cached_);
  if (const auto result = ai_.poll()) {
    ++event_flow_.replies;event_flow_.requested=result->request_tick;event_flow_.received=tick;
    event_flow_.error.clear();event_flow_.last_events=Json::array();
    bool understood = false, partial = false; // 完整理解与部分理解分开，HTTP2xx不替代它们。
    try {
      if (result->http_success && !result->mock_response && result->session == session_) {
        if (result->status == "model_reply_received") strategy_.note_model_reply();
        const auto proposal = decode_event_proposal(result->reply, session_, result->source, // 校验后的模型提案。
                                          p.at("map").at("width").get<int>(),
                                          p.at("map").at("height").get<int>());
        strategy_.merge_events(proposal.events);
        for(const auto &event:proposal.events) {
          ++event_flow_.imported;event_flow_.after_start+=event.start<tick;event_flow_.expired+=event.end<=tick;
          auto summary=event_summary(event);summary.erase("source");event_flow_.last_events.push_back(std::move(summary));
        }
        partial = !proposal.unknown.empty();
        understood = !partial;
        if(partial) {++event_flow_.partial;event_flow_.error="events_incomplete";}
        else ++event_flow_.validated;
      } else {
        ++event_flow_.rejected;event_flow_.error=result->status;
      }
    } catch (const Json::exception &error) {
      ++event_flow_.rejected;event_flow_.error=error.id==101 ? "invalid_json" : "missing_or_wrong_event_field";
    } catch (const std::exception &) {
      ++event_flow_.rejected;event_flow_.error="event_contract_rejected";
    }
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
