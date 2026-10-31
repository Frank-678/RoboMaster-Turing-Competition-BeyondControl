#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace rmtc { // 选手程序公共数据类型，不是裁判内部事件接口。
using Tick = std::int64_t; // 64位逻辑回合，避免回合运算被小整数截断。
using Cell = std::pair<int, int>; // first=x，second=y；左上角[0,0]。
enum class EventKind { explosion, illusion, heal, fortress, split, chili, reflect, vulnerability }; // 爆炸、幻术、回血、堡垒、分裂、辣椒、反伤、易伤。

// 本地模型输出契约，不是裁判提供的感知字段。期限统一采用 [start,end)。
struct Event {
  EventKind kind; // 具体是哪种事件。
  Tick start = 0, end = 0, trigger_end = 0; // 生效Tick、结束Tick、易伤的移动触发截止Tick。
  std::array<int, 4> area{}; // 矩形[left,top,right,bottom]，包含边界。
  std::vector<Cell> points; // 点位事件的点；分裂事件则是十字线交点。
  double amount = 0, defended_damage = 0, multiplier = 1; // 主要伤害/回血值、无双对防御伤害、易伤倍率。
  int effect_ticks = 0, capture_ticks = 0; // 入点后回血时长、获得无双所需连续驻留次数。
  std::string source, key; // 原公告证据、由类型/起点/范围生成的稳定去重键。
  bool active(Tick tick) const { return start <= tick && tick < end; } // 是否在有效期内。
  bool contains(Cell cell) const; // cell是否在矩形/点/十字线上；全场事件始终为true。
};

// 只解析模型返回的 JSON；不匹配或解释公告中的关键词、句式、时间表。
struct EventProposal {
  std::vector<Event> events; // 已经通过类型/范围/证据校验的部分。
  std::vector<std::string> unknown; // 模型明确说明仍无法解释的部分；不擅自补齐。
};
// reply是模型文本；session是当前场次；source是本次原公告；width/height是合法地图边界。
// 返回校验过的提案；任何格式/证据错误都抛异常，由Session保持本地策略。
EventProposal decode_event_proposal(const std::string &reply, const std::string &session,
                                    const std::string &source, int width, int height);
// 测试等只关心已校验事件时使用；正式Session用上面的接口保留unknown。
std::vector<Event> decode_events(const std::string &reply, const std::string &session,
                                 const std::string &source, int width, int height);
nlohmann::json event_summary(const Event &event); // 把已知事件变成下一次请求可携带的JSON。
std::string event_system_prompt(); // 告诉LLM事件机制、输出格式和禁止猜测的内容。
}  // namespace rmtc
