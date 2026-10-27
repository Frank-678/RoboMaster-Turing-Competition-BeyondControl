#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace rmtc {
using Tick = std::int64_t;
using Cell = std::pair<int, int>;
enum class EventKind { explosion, illusion, heal, fortress, split, chili, reflect, vulnerability };

// 本地模型输出契约，不是裁判提供的感知字段。期限统一采用 [start,end)。
struct Event {
  EventKind kind;
  Tick start = 0, end = 0, trigger_end = 0;
  std::array<int, 4> area{};
  std::vector<Cell> points;
  double amount = 0, defended_damage = 0, multiplier = 1;
  int effect_ticks = 0, capture_ticks = 0;
  std::string source, key;
  bool active(Tick tick) const { return start <= tick && tick < end; }
  bool contains(Cell cell) const;
};

// 只解析模型返回的 JSON；不匹配或解释公告中的关键词、句式、时间表。
struct EventProposal {
  std::vector<Event> events;
  std::vector<std::string> unknown;
};
EventProposal decode_event_proposal(const std::string &reply, const std::string &session,
                                    const std::string &source, int width, int height);
// Convenience for callers interested only in already-validated events.
std::vector<Event> decode_events(const std::string &reply, const std::string &session,
                                 const std::string &source, int width, int height);
nlohmann::json event_summary(const Event &event);
std::string event_system_prompt();
}  // namespace rmtc
