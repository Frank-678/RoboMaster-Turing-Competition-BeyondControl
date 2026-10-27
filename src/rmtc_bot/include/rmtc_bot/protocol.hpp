#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace rmtc { // 协议小工具与策略共用的工程命名空间。
using Json = nlohmann::json; // JSON的短名字，避免每个函数反复写完整类型。

// p是感知。先保证场次、Tick和存活状态可靠，才允许构造任何动作。
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
  const auto tick = p["tick"].get<std::int64_t>(); // 通过无符号上界检查后才转换。
  if (tick < 0) {
    throw std::runtime_error("tick must not be negative");
  }
  return tick;
}

// 第二阶段存活时，坏提案改为DEFEND。第一阶段/阵亡时不能DEFEND，返回null表示不发布。
// proposed是本地策略提案；stage是明确的比赛阶段；输出只带协议允许字段。
inline Json action_for(const Json &p, const Json &proposed, int stage) {
  const auto tick = frame_tick(p); // 本帧通过信封校验的Tick。
  if (stage != 1 && stage != 2) throw std::runtime_error("invalid stage");
  if (!p["self"]["alive"].get<bool>()) return nullptr;
  Json out = {{"tick", tick}, {"session_id", p["session_id"]}, {"action", "defend"}}; // 场次/Tick只从感知取。
  const Json fallback = stage == 2 ? out : Json(); // 合法防御或不发布，绝不生成默认WAIT。
  if (!proposed.is_object() ||
      !proposed.contains("action") || !proposed["action"].is_string()) {
    return fallback;
  }
  const auto action = proposed["action"].get<std::string>(); // 准备发布的动作名。
  if (action == "wait" || action == "defend") {
    if (action == "defend" && stage == 1) return fallback;
    out["action"] = action;
    return out;
  }
  if (action != "move" && action != "attack") {
    return fallback;
  }
  if (action == "attack" && (stage == 1 || !p["self"].contains("protected") ||
      !p["self"]["protected"].is_boolean() || p["self"]["protected"].get<bool>())) {
    return fallback;
  }
  if (!proposed.contains("direction") || !proposed["direction"].is_string()) {
    return fallback;
  }
  const auto direction = proposed["direction"].get<std::string>(); // MOVE/ATTACK都必须有四向之一。
  if (direction == "up" || direction == "down" ||
      direction == "left" || direction == "right") {
    out["action"] = action;
    out["direction"] = direction;
    return out;
  }
  return fallback;
}
}  // namespace rmtc
