#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace rmtc {
using Json = nlohmann::json;

inline std::int64_t frame_tick(const Json &p) {
  if (!p.is_object() || !p.contains("tick") ||
      !p["tick"].is_number_integer() || !p.contains("session_id") ||
      !p["session_id"].is_string() ||
      p["session_id"].get_ref<const std::string &>().empty() ||
      !p.contains("self") || !p["self"].is_object() ||
      !p["self"].contains("alive") || !p["self"]["alive"].is_boolean()) {
    throw std::runtime_error("invalid perception envelope");
  }
  if (p["tick"].is_number_unsigned() &&
      p["tick"].get<std::uint64_t>() >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::runtime_error("tick is out of range");
  }
  const auto tick = p["tick"].get<std::int64_t>();
  if (tick < 0) {
    throw std::runtime_error("tick must not be negative");
  }
  return tick;
}

inline Json action_for(const Json &p, const Json &proposed) {
  const auto tick = frame_tick(p);
  Json out = {{"tick", tick}, {"session_id", p["session_id"]}, {"action", "wait"}};
  if (!p["self"]["alive"].get<bool>() || !proposed.is_object() ||
      !proposed.contains("action") || !proposed["action"].is_string()) {
    return out;
  }
  const auto action = proposed["action"].get<std::string>();
  if (action == "wait" || action == "defend") {
    out["action"] = action;
    return out;
  }
  if (action != "move" && action != "attack") {
    return out;
  }
  if (action == "attack" && p["self"].contains("protected") &&
      (!p["self"]["protected"].is_boolean() ||
       p["self"]["protected"].get<bool>())) {
    return out;
  }
  if (!proposed.contains("direction") || !proposed["direction"].is_string()) {
    return out;
  }
  const auto direction = proposed["direction"].get<std::string>();
  if (direction == "up" || direction == "down" ||
      direction == "left" || direction == "right") {
    out["action"] = action;
    out["direction"] = direction;
  }
  return out;
}
}  // namespace rmtc
