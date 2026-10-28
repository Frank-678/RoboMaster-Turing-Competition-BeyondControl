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

// 每帧由裁判状态进入四种动作权限状态；不自行倒数复活或猜保护结束。
enum class TurnMode { dead, collecting, guarded, combat_ready };
// guarded包含出生保护和保护字段不可靠：允许移动/防御，但不能攻击。
inline TurnMode turn_mode(const Json &p, int stage) {
  frame_tick(p); // 信封不可信时不建立动作状态。
  if (stage != 1 && stage != 2) throw std::runtime_error("invalid stage");
  if (!p.at("self").at("alive").get<bool>()) return TurnMode::dead;
  if (stage == 1) return TurnMode::collecting;
  const auto &self = p.at("self"); // 出生保护只采用本帧公开字段。
  if (!self.contains("protected") || !self.at("protected").is_boolean() || self.at("protected").get<bool>())
    return TurnMode::guarded;
  return TurnMode::combat_ready;
}
inline const char *turn_mode_name(TurnMode mode) { // 日志与状态机图使用相同名称。
  switch (mode) {
    case TurnMode::dead: return "dead";
    case TurnMode::collecting: return "collecting";
    case TurnMode::guarded: return "guarded";
    case TurnMode::combat_ready: return "combat_ready";
  }
  throw std::runtime_error("invalid turn mode");
}

// 第二阶段存活时，坏提案改为DEFEND。第一阶段/阵亡时不能DEFEND，返回null表示不发布。
// proposed是本地策略提案；stage是明确的比赛阶段；输出只带协议允许字段。
inline Json action_for(const Json &p, const Json &proposed, int stage) {
  const auto mode = turn_mode(p, stage); // 先进入权限状态，再校验候选动作。
  const auto tick = p.at("tick").get<std::int64_t>(); // turn_mode已核对本帧Tick。
  if (mode == TurnMode::dead) return nullptr;
  Json out = {{"tick", tick}, {"session_id", p["session_id"]}, {"action", "defend"}}; // 场次/Tick只从感知取。
  const Json fallback = mode == TurnMode::collecting ? Json() : out; // 合法防御或不发布，绝不生成默认WAIT。
  if (!proposed.is_object() ||
      !proposed.contains("action") || !proposed["action"].is_string()) {
    return fallback;
  }
  const auto action = proposed["action"].get<std::string>(); // 准备发布的动作名。
  if (action == "wait" || action == "defend") {
    if (action == "defend" && mode == TurnMode::collecting) return fallback;
    out["action"] = action;
    return out;
  }
  if (action != "move" && action != "attack") {
    return fallback;
  }
  if (action == "attack" && mode != TurnMode::combat_ready) {
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

